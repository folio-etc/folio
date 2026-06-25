#pragma once
#include <BitmapCacheManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "LibraryIndex.h"  // THUMB_HEIGHT / THUMB_MAX_WIDTH (thumb-storage convention)

class GfxRenderer;

// Single-page cover cache + async prefetch worker, lifted out of LibraryActivity.
//
// View-type-agnostic: it knows nothing about grids, rows, or LibraryBook. It owns
// a fixed-capacity, arena-backed cache sized to ONE page of `perPage` thumbnails and
// a FreeRTOS worker that decodes + pre-scales each cover off the render path. The
// owner supplies a CoverResolver mapping a flat item index to a cover pathHash; any
// paginated view (cover grid, future list view) reuses this by supplying its own
// resolver, perPage, and cover-box dimensions.
//
// Threading: a single binary semaphore (cacheLock_) guards the cache, the work queue,
// and — via the resolver — the owner's view state. Hold it (lockCache()) around any
// render-path cache read AND around any mutation of the data the resolver reads
// (e.g. re-sorting / rebuilding the view), since the worker invokes the resolver
// under this same lock.
class CoverPrefetcher {
 public:
  // Cover pathHash for a flat item index, or nullopt for slots with no cover
  // (back affordance / past end of the view). Invoked ONLY under cacheLock_
  // (inside buildThumbPath), so the supplied closure may safely read view state.
  using CoverResolver = std::function<std::optional<uint32_t>(int itemIndex)>;

  CoverPrefetcher(GfxRenderer& renderer, uint8_t perPage, CoverResolver resolver)
      : renderer_(renderer),
        perPage_(perPage),
        resolver_(std::move(resolver)),
        pageCache_(perPage, 0) {}

  // Alloc scratch + grow the cover arena + spin up the worker. Deferred from
  // construction so the owner controls timing (after a cold index, when heap is tight).
  void start(int coverBoxW, int coverBoxH);
  // Shut down + join the worker, then free everything. Idempotent and safe to call
  // when never started. Must run before the resolver's backing data is torn down.
  void stop();

  // Swap the single-page cache to `page`: cancel in-flight prefetch, clear, enqueue.
  // Does NOT paint — the caller repaints (placeholders) and consumeBatchDone() drives
  // the real-cover repaint. `pageCount` validates the page index.
  void loadPage(uint8_t page, uint8_t pageCount);
  void cancelAll();

  // Update the cover draw envelope (theme/orientation change). Returns true if it
  // changed — the caller then reloads the current page at the new dims.
  bool setCoverBox(int w, int h);

  // Poll for the worker's batch-completion. Returns true exactly once per completed
  // batch while a page render is pending — the caller repaints when it does.
  bool consumeBatchDone();

  // RAII guard over cacheLock_. Hold around cache() reads and around resolver-backing
  // mutations. Move-only; relies on guaranteed copy elision from lockCache().
  class CacheGuard {
   public:
    explicit CacheGuard(SemaphoreHandle_t lock) : lock_(lock) {
      xSemaphoreTake(lock_, portMAX_DELAY);
    }
    ~CacheGuard() {
      if (lock_ != nullptr) xSemaphoreGive(lock_);
    }
    CacheGuard(const CacheGuard&) = delete;
    CacheGuard& operator=(const CacheGuard&) = delete;
    CacheGuard(CacheGuard&& o) noexcept : lock_(o.lock_) { o.lock_ = nullptr; }
    CacheGuard& operator=(CacheGuard&&) = delete;

   private:
    SemaphoreHandle_t lock_;
  };
  CacheGuard lockCache() { return CacheGuard(cacheLock_); }
  BitmapCacheManager& cache() {
    return pageCache_;
  }  // valid only while a CacheGuard is held

  // The per-book cover thumb path. Shared with the render path so the key it looks
  // up matches the key the worker stores. `out` must be at least 64 bytes.
  static void thumbPath(uint32_t pathHash, char* out, std::size_t outSize);

 private:
  // Worst-case 2bpp source for one thumbnail (THUMB_MAX_WIDTH × THUMB_HEIGHT,
  // stride = (w+3)/4). Reused decode scratch for the worker.
  static constexpr std::size_t DECODE_SCRATCH_BYTES =
    static_cast<std::size_t>((LibraryIndex::THUMB_MAX_WIDTH + 3) / 4) *
    LibraryIndex::THUMB_HEIGHT;
  // Worst-case 1bpp raster for one cached cover. Covers are aspect-fit and only
  // ever downscaled (computeThumbTarget never upscales), so a raster never exceeds
  // the source thumb dims (THUMB_MAX_WIDTH × THUMB_HEIGHT). stride = (w + 7) / 8.
  static constexpr std::size_t MAX_COVER_RASTER_BYTES =
    ((LibraryIndex::THUMB_MAX_WIDTH + 7) / 8) * LibraryIndex::THUMB_HEIGHT;
  // Cover-cache arena: one page of cover rasters + 25% for Arena per-block headers
  // and intra-arena fragmentation. Computed from perPage_ in start() (the page is
  // cleared/reloaded as a unit, so the free list coalesces; an over-budget raster
  // just renders a placeholder — no crash).
  std::size_t coverArenaBytes() const {
    return perPage_ * MAX_COVER_RASTER_BYTES * 5 / 4;
  }
  // Only one page is normally in flight, but a release mid-decode can enqueue a
  // second before the worker drains the first. Sentinel 0xFF = empty slot.
  static constexpr std::size_t PREFETCH_QUEUE_CAPACITY = 4;

  static void taskTrampoline(void* ctx);
  void taskLoop();
  // idx = page*perPage_ + slot; resolver_(idx) -> pathHash; nullopt -> false (no cover).
  bool buildThumbPath(uint8_t page, uint8_t slot, char* out, std::size_t outSize) const;
  // Validate, dedupe, enqueue, signal the worker. Caller holds cacheLock_.
  bool enqueueLocked(uint8_t page, uint8_t pageCount);

  GfxRenderer& renderer_;
  uint8_t perPage_;
  CoverResolver resolver_;

  BitmapCacheManager pageCache_;
  std::unique_ptr<uint8_t[]> decodeScratch_;
  // Cover draw dimensions. Read by the worker off-lock, written by setCoverBox; a
  // stale read at worst scales one batch to old dims before the reload re-decodes.
  int coverBoxW_ = 0;
  int coverBoxH_ = 0;

  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t cacheLock_ = nullptr;
  SemaphoreHandle_t signal_ = nullptr;
  SemaphoreHandle_t exited_ = nullptr;
  SemaphoreHandle_t batchDone_ = nullptr;
  uint8_t queue_[PREFETCH_QUEUE_CAPACITY] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t queueCount_ = 0;
  volatile bool cancel_ = false;
  volatile bool shutdown_ = false;
  // Set by loadPage: a page load is in flight. consumeBatchDone() clears it and
  // returns true so the caller repaints once the worker's covers land.
  volatile bool pendingRender_ = false;
};
