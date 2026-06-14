#pragma once

#include <Arena.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Fixed-capacity, path-keyed cache for pre-scaled 1-bit bitmaps. Sized at
// construction to the working set of a single scene (e.g. one Library page) so
// callers can rely on get() hits across all paints of that scene with no
// eviction churn. When the working set rotates (page navigation), the owner
// calls clear() / evictIf() before re-populating.
//
// Storage is an inline vector of Entry slots, allocated once at construction.
// Each Entry's raster comes from an owned Arena, so the per-cover alloc/free
// churn stays inside one contiguous region instead of fragmenting the general
// heap. Lookup is an O(capacity) linear scan — for the 9-tile Library grid this
// is faster than a hash map and carries no per-get node allocation.
//
// Entries hold ONLY the final 1-bit raster (no 2bpp source): the producer
// scales to the draw dimensions before calling set(), so the render path blits
// straight from `scaledPixels` with no scaling and no SD I/O.
class BitmapCacheManager {
 public:
  // Custom deleter returning an Entry's raster to the owning Arena. Explicit
  // ctors keep it unambiguously default-constructible so unique_ptr's default
  // constructor (and thus Entry's) stays available.
  struct ArenaFree {
    Arena* arena;
    ArenaFree() noexcept : arena(nullptr) {}
    explicit ArenaFree(Arena* a) noexcept : arena(a) {}
    void operator()(uint8_t* p) const noexcept {
      if (arena != nullptr && p != nullptr) arena->deallocate(p);
    }
  };
  using ArenaBuf = std::unique_ptr<uint8_t[], ArenaFree>;

  struct Entry {
    // Path key. Empty string means "slot is unoccupied".
    std::string path;

    // Native (source) dimensions of the decoded thumbnail. Consumers use these
    // to drive aspect-fit framing; the raster below is already scaled.
    int width = 0;
    int height = 0;
    bool topDown = false;

    // Pre-scaled 1-bit raster at the target draw dimensions. Arena-backed:
    // freed back to the cache's Arena when the slot is cleared/overwritten.
    // stride = (scaledWidth + 7) / 8.
    ArenaBuf scaledPixels;
    std::size_t scaledPixelsBytes = 0;
    int scaledWidth = 0;
    int scaledHeight = 0;
  };

  // `capacity` = number of slots; `arenaBytes` = size of the raster region.
  // On arena-init OOM the cache still functions but every set() that needs a
  // raster fails (allocate() returns null), so callers fall back to placeholder
  // art — the device stays up.
  BitmapCacheManager(std::size_t capacity, std::size_t arenaBytes) : slots_(capacity) {
    arena_.init(arenaBytes);
  }

  // The raster region. Producers allocate `scaledPixels` from here (under the
  // same lock that guards set()/evict()).
  Arena& arena() { return arena_; }

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
  // Declared first so it is destroyed LAST (members tear down in reverse
  // order): slot destruction frees each raster back into the arena, so the
  // arena must still be alive when `slots_` is destroyed.
  Arena arena_;
  std::vector<Entry> slots_;
  // Index of the slot to overwrite next when the cache is full.
  std::size_t nextOverwrite_ = 0;
};
