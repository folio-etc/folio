#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

// ---------------------------------------------------------------------------
// Byte-bounded LRU cache (templated, thread-safe)
//
// Capacity is measured in bytes.  The cache stores (key, value) pairs and
// evicts the least-recently-used entry when inserting a new one would exceed
// the byte budget.
//
// Value storage: values are stored by move semantics — no copies.
//   - get(key)     → returns const Value* (nullptr on miss, zero-copy)
//   - get_mut(key) → returns Value*       (touches LRU; reweigh after mutation)
//   - take(key)    → moves Value out      (caller takes ownership, removes entry)
//   - put(key, value) → moves value in
//   - reweigh(key) → recompute weight after in-place mutation
//
// Byte weight: there is intentionally no sizeof-based default — sizeof(V) is
// misleading for pointer-like values (e.g. unique_ptr<uint8_t[]> always
// reports 8 bytes regardless of the buffer it owns), and a misreported
// weight silently breaks the byte budget.  Callers have two options:
//
//   1. Make Value self-describing by giving it a `std::size_t byte_size()
//      const` method, then construct with just `ByteLRUCache(capacity)`.
//      The default weight = sizeof(Key) + value.byte_size().
//   2. Pass an explicit ByteWeight functor:
//      `ByteLRUCache(capacity, [](const K&, const V& v) { ... })`.
//
// Value types without `byte_size()` cannot use option 1 and must supply a
// functor explicitly — the compiler will reject the one-arg constructor.
//
// Heterogeneous lookup: pass a Hash + KeyEqual pair that declare
// `is_transparent` (à la std::unordered_map) and the lookup methods accept
// any type the hash + equal-to support — no temporary Key is constructed.
// With the default std::hash / std::equal_to, lookups take Key only.
//
// Thread safety: all methods are internally synchronized.  However, the
// pointer returned by get() is only valid until the next mutating call
// (put/take/clear) from ANY thread.  Callers that need a longer-lived
// reference must copy the value or use take().
// ---------------------------------------------------------------------------

template <typename V>
concept HasByteSize = requires(const V& v) {
    { v.byte_size() } -> std::convertible_to<std::size_t>;
};

template <typename Key,
          typename Value,
          typename Hash     = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class ByteLRUCache {
    static_assert(!std::is_reference_v<Key> && !std::is_reference_v<Value>,
                  "Key and Value must not be reference types");
    static_assert(std::is_move_constructible_v<Value>,
                  "Value must be move-constructible (unique_ptr, etc.)");

public:
    using ByteWeight = std::function<std::size_t(const Key&, const Value&)>;

    ByteLRUCache(std::size_t capacity_bytes, ByteWeight byte_weight);

    /// Convenience constructor: uses `sizeof(Key) + value.byte_size()` as the
    /// weight.  Only available when Value exposes `byte_size() const`.
    explicit ByteLRUCache(std::size_t capacity_bytes) requires HasByteSize<Value>
        : ByteLRUCache(capacity_bytes,
                       [](const Key& k, const Value& v) {
                           return sizeof(k) + v.byte_size();
                       }) {}

    // -- access ----------------------------------------------------------------

    /// Read-only access — returns a pointer into the cache, nullptr on miss.
    /// The pointer is invalidated by any subsequent put/take/clear.
    template <typename K2>
    const Value* get(const K2& key);

    /// Mutable access — returns a pointer into the cache, nullptr on miss.
    /// Touches LRU.  If the caller mutates the value in a way that changes
    /// its byte weight, it MUST call reweigh(key) afterwards to keep the
    /// byte accounting honest.  Same invalidation rules as get().
    template <typename K2>
    Value* get_mut(const K2& key);

    /// Move the value out of the cache, removing the entry.
    template <typename K2>
    std::optional<Value> take(const K2& key);

    /// Insert or update a (key, value) pair.
    void put(Key key, Value value);

    /// Recompute the entry's weight from its current value (e.g. after the
    /// caller mutated it through get_mut()).  Does NOT trigger eviction —
    /// used_bytes may temporarily exceed capacity until the next put()
    /// brings it back under.  No-op if the key is no longer in the cache.
    template <typename K2>
    void reweigh(const K2& key);

    // -- inspection ------------------------------------------------------------

    std::size_t size() const;
    std::size_t used_bytes() const;
    std::size_t capacity() const;
    void clear();

    /// Change the byte budget.  If the new capacity is below the current
    /// used_bytes, LRU entries are evicted until used_bytes <= capacity.
    void set_capacity(std::size_t capacity_bytes);

private:
    struct Node {
        Key         key;
        Value       value;
        // Cached weight as of last insert/reweigh.  Stored so that eviction
        // and take() don't need to re-run byte_weight_(), and so reweigh()
        // can compute a delta against the previously-accounted size.
        std::size_t weight;
    };

    using ListIter = typename std::list<Node>::iterator;

    // requires mutex_ held
    void evict_lru();

    std::size_t             capacity_;
    std::size_t             used_bytes_ = 0;
    ByteWeight              byte_weight_;
    mutable std::mutex      mutex_;

    // Doubly-linked list: front = LRU, back = MRU.
    std::list<Node>         node_list_;

    // Key → iterator into node_list.  Hash + KeyEqual are caller-supplied so
    // heterogeneous lookup works when both declare is_transparent.
    std::unordered_map<Key, ListIter, Hash, KeyEqual> map_;
};

// ---- Inline implementations (needed for template instantiation) -----------

template <typename K, typename V, typename H, typename E>
ByteLRUCache<K, V, H, E>::ByteLRUCache(std::size_t capacity_bytes, ByteWeight byte_weight)
    : capacity_(capacity_bytes), byte_weight_(std::move(byte_weight)) {}

template <typename K, typename V, typename H, typename E>
template <typename K2>
const V* ByteLRUCache<K, V, H, E>::get(const K2& key) {
    std::lock_guard lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    // Move to MRU position (O(1) splice, no copy)
    node_list_.splice(node_list_.end(), node_list_, it->second);
    return &it->second->value;
}

template <typename K, typename V, typename H, typename E>
template <typename K2>
V* ByteLRUCache<K, V, H, E>::get_mut(const K2& key) {
    std::lock_guard lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    node_list_.splice(node_list_.end(), node_list_, it->second);
    return &it->second->value;
}

template <typename K, typename V, typename H, typename E>
template <typename K2>
std::optional<V> ByteLRUCache<K, V, H, E>::take(const K2& key) {
    std::lock_guard lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    auto list_it = it->second;
    used_bytes_ -= list_it->weight;
    V value = std::move(list_it->value);
    map_.erase(it);
    node_list_.erase(list_it);
    return value;
}

template <typename K, typename V, typename H, typename E>
void ByteLRUCache<K, V, H, E>::put(K key, V value) {
    std::lock_guard lock(mutex_);
    std::size_t weight = byte_weight_(key, value);

    auto it = map_.find(key);
    if (it != map_.end()) {
        auto list_it = it->second;
        used_bytes_ -= list_it->weight;
        list_it->value  = std::move(value);
        list_it->weight = weight;
        node_list_.splice(node_list_.end(), node_list_, list_it);
        used_bytes_ += weight;
        return;
    }

    if (weight > capacity_) return;

    while (used_bytes_ + weight > capacity_ && !node_list_.empty()) {
        evict_lru();
    }

    node_list_.push_back(Node{std::move(key), std::move(value), weight});
    auto list_it = std::prev(node_list_.end());
    map_.emplace(list_it->key, list_it);
    used_bytes_ += weight;
}

template <typename K, typename V, typename H, typename E>
template <typename K2>
void ByteLRUCache<K, V, H, E>::reweigh(const K2& key) {
    std::lock_guard lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return;
    auto& node = *it->second;
    std::size_t new_weight = byte_weight_(node.key, node.value);
    used_bytes_ -= node.weight;
    used_bytes_ += new_weight;
    node.weight = new_weight;
}

template <typename K, typename V, typename H, typename E>
void ByteLRUCache<K, V, H, E>::evict_lru() {
    auto& lru = node_list_.front();
    used_bytes_ -= lru.weight;
    map_.erase(lru.key);
    node_list_.pop_front();
}

template <typename K, typename V, typename H, typename E>
std::size_t ByteLRUCache<K, V, H, E>::size() const {
    std::lock_guard lock(mutex_);
    return map_.size();
}

template <typename K, typename V, typename H, typename E>
std::size_t ByteLRUCache<K, V, H, E>::used_bytes() const {
    std::lock_guard lock(mutex_);
    return used_bytes_;
}

template <typename K, typename V, typename H, typename E>
std::size_t ByteLRUCache<K, V, H, E>::capacity() const {
    std::lock_guard lock(mutex_);
    return capacity_;
}

template <typename K, typename V, typename H, typename E>
void ByteLRUCache<K, V, H, E>::clear() {
    std::lock_guard lock(mutex_);
    map_.clear();
    node_list_.clear();
    used_bytes_ = 0;
}

template <typename K, typename V, typename H, typename E>
void ByteLRUCache<K, V, H, E>::set_capacity(std::size_t capacity_bytes) {
    std::lock_guard lock(mutex_);
    capacity_ = capacity_bytes;
    while (used_bytes_ > capacity_ && !node_list_.empty()) {
        evict_lru();
    }
}
