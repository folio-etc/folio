#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Fixed-capacity, path-keyed cache for decoded bitmaps. Sized at construction
// to the working set of a single scene (e.g. one Library page) so callers
// can rely on get() hits across all paints of that scene with no eviction
// churn. When the working set rotates (page navigation), the owner calls
// clear() before re-populating.
//
// Storage is an inline vector of Entry slots, allocated once at construction.
// No hash table, no map nodes, no per-get/set heap traffic beyond the
// caller-supplied Entry's own pixel buffers. Lookup is O(capacity) linear
// scan — for the 9-tile Library grid this is faster than a hash map and
// avoids the heap fragmentation that came with the previous global LRU.
class BitmapCacheManager {
 public:
  struct Entry {
    // Path key. Empty string means "slot is unoccupied".
    std::string path;

    // 2-bit-per-pixel packed source pixels (Bitmap::readNextRow format).
    // stride = (width + 3) / 4 bytes; total = stride × height.
    std::unique_ptr<uint8_t[]> pixels;
    std::size_t pixelsBytes = 0;
    int width = 0;
    int height = 0;
    bool topDown = false;

    // Lazily-built 1-bit pre-scaled pixels at the most recently requested
    // target dimensions. stride = (scaledWidth + 7) / 8.
    std::unique_ptr<uint8_t[]> scaledPixels;
    std::size_t scaledPixelsBytes = 0;
    int scaledWidth = 0;
    int scaledHeight = 0;
  };

  explicit BitmapCacheManager(std::size_t capacity) : slots_(capacity) {}

  // Look up the entry for `path`. Returns nullptr on miss. The returned
  // pointer is invalidated by the next clear() / set() call.
  Entry* get(const char* path);

  // Move `entry` into the cache. If the key is already present, overwrites
  // that slot; otherwise fills an empty slot if one is available;
  // otherwise round-robin replaces the slot that has been resident
  // longest. Returns a pointer to the stored slot, or nullptr if capacity
  // is zero. The pointer is invalidated by the next clear() / set() call.
  Entry* set(Entry entry);

  // Drop every entry, releasing pixel buffers. Capacity is preserved.
  void clear();

  // Drop a single entry by path, releasing its pixel buffers. No-op if the
  // path isn't in the cache. Used both to invalidate a stale-dimensioned
  // entry (forcing a re-decode at the new size) and to evict per-page
  // entries when the Library transitions between pages.
  void evict(const char* path);

  // Drop every entry whose path satisfies `pred`. Pred receives the path
  // as a string_view (non-empty). Used by LibraryActivity to bulk-evict
  // the page that just rotated out of the {prev, cur, next} keep-set.
  template <typename Pred>
  void evictIf(Pred pred) {
    for (auto& slot : slots_) {
      if (slot.path.empty()) continue;
      if (pred(static_cast<const std::string&>(slot.path))) slot = Entry{};
    }
  }

  std::size_t capacity() const { return slots_.size(); }

 private:
  std::vector<Entry> slots_;
  // Index of the slot to overwrite next when the cache is full.
  std::size_t nextOverwrite_ = 0;
};
