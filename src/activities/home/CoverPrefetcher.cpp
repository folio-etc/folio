#include "CoverPrefetcher.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow

#include <cassert>
#include <cstdio>
#include <utility>

namespace {
constexpr char LOG_TAG[] = "COVP";
}  // namespace

void CoverPrefetcher::thumbPath(
  uint32_t pathHash, uint8_t format, char* out, std::size_t outSize
) {
  // Cache dir prefix per format (Epub/Xtc share the thumb_<h>.bmp convention; the hash
  // is the same std::hash<path> all three subsystems agree on — see util/PathHash.h).
  const char* prefix = nullptr;
  switch (static_cast<BookFormat>(format)) {
    case BookFormat::Epub:
      prefix = "epub";
      break;
    case BookFormat::Xtc:
      prefix = "xtc";
      break;
    case BookFormat::Txt:
      break;  // no thumbnail — placeholder
  }
  if (prefix == nullptr) {
    if (outSize > 0) out[0] = '\0';
    return;
  }
  snprintf(
    out,
    outSize,
    "/.crosspoint/%s_%lu/thumb_%d.bmp",
    prefix,
    static_cast<unsigned long>(pathHash),
    LibraryIndex::THUMB_HEIGHT
  );
}

bool CoverPrefetcher::buildThumbPath(
  uint8_t page, uint8_t slot, char* out, std::size_t outSize
) const {
  if (out == nullptr || outSize < 64) return false;
  const int idx = static_cast<int>(page) * perPage_ + static_cast<int>(slot);
  const std::optional<CoverRef> ref = resolver_(idx);
  if (!ref) {
    out[0] = '\0';
    return false;
  }
  thumbPath(ref->hash, ref->format, out, outSize);
  return out[0] != '\0';  // empty path (e.g. txt has no thumb) → no cover to fetch
}

void CoverPrefetcher::start(int coverBoxW, int coverBoxH) {
  // Seed the cover draw envelope (the worker scales each cover to fit it) and the
  // reused decode scratch BEFORE the worker starts. A null scratch is tolerated —
  // decodeThumbInto fails gracefully and the cover shows a placeholder.
  coverBoxW_ = coverBoxW;
  coverBoxH_ = coverBoxH;
  decodeScratch_ = makeUniqueNoThrow<uint8_t[]>(DECODE_SCRATCH_BYTES);
  if (!decodeScratch_)
    LOG_ERR(
      LOG_TAG,
      "OOM: cover decode scratch (%u bytes)",
      static_cast<unsigned>(DECODE_SCRATCH_BYTES)
    );

  // Reserve the cover-cache arena now (deferred from construction so it didn't
  // compete with EPUB parsing for heap during a cold index). Sized to one page.
  pageCache_.arena().init(coverArenaBytes());

  cacheLock_ = xSemaphoreCreateBinary();
  signal_ = xSemaphoreCreateBinary();
  exited_ = xSemaphoreCreateBinary();
  batchDone_ = xSemaphoreCreateBinary();
  assert(
    cacheLock_ != nullptr && signal_ != nullptr && exited_ != nullptr &&
    batchDone_ != nullptr
  );
  // Binary semaphores from xSemaphoreCreateBinary are born "taken" (count = 0).
  // Give once so the first acquirer can take it.
  xSemaphoreGive(cacheLock_);
  cancel_ = false;
  shutdown_ = false;
  queueCount_ = 0;
  for (auto& slot : queue_) slot = 0xFF;
  xTaskCreate(
    &taskTrampoline,
    "LibPrefetch",
    4096,  // stack: SD I/O + bitmap parsing
    this,
    1,  // priority: same tier as render
    &task_
  );
  assert(task_ != nullptr);
}

void CoverPrefetcher::stop() {
  if (task_ != nullptr) {
    xSemaphoreTake(cacheLock_, portMAX_DELAY);
    shutdown_ = true;
    cancel_ = true;
    queueCount_ = 0;
    xSemaphoreGive(cacheLock_);
    xSemaphoreGive(signal_);
    xSemaphoreTake(exited_, portMAX_DELAY);
    vTaskDelete(task_);
    task_ = nullptr;
  }

  if (signal_ != nullptr) {
    vSemaphoreDelete(signal_);
    signal_ = nullptr;
  }
  if (exited_ != nullptr) {
    vSemaphoreDelete(exited_);
    exited_ = nullptr;
  }
  if (batchDone_ != nullptr) {
    vSemaphoreDelete(batchDone_);
    batchDone_ = nullptr;
  }

  pageCache_.clear();
  // Worker has joined, so the scratch has no remaining reader and can be freed.
  decodeScratch_.reset();

  if (cacheLock_ != nullptr) {
    vSemaphoreDelete(cacheLock_);
    cacheLock_ = nullptr;
  }
}

bool CoverPrefetcher::enqueueLocked(uint8_t page, uint8_t pageCount) {
  if (pageCount == 0 || page >= pageCount) return false;

  for (uint8_t i = 0; i < queueCount_; ++i) {
    if (queue_[i] == page) return true;  // already queued
  }

  if (queueCount_ >= PREFETCH_QUEUE_CAPACITY) {
    // Drop the oldest pending — newer requests reflect more recent user intent.
    for (uint8_t i = 1; i < queueCount_; ++i) {
      queue_[i - 1] = queue_[i];
    }
    --queueCount_;
  }

  queue_[queueCount_++] = page;
  xSemaphoreGive(signal_);
  return true;
}

void CoverPrefetcher::cancelAll() {
  if (cacheLock_ == nullptr) return;
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  cancel_ = true;
  queueCount_ = 0;
  for (auto& slot : queue_) slot = 0xFF;
  xSemaphoreGive(cacheLock_);
  xSemaphoreGive(signal_);
}

void CoverPrefetcher::loadPage(uint8_t page, uint8_t pageCount) {
  if (cacheLock_ == nullptr) return;

  // Cancel any in-flight decode, swap the single-page working set to `page`, and arm
  // the batch-done repaint. The caller paints immediately afterward — the cleared
  // cache peek-misses into placeholders, then the worker's batch-done repaints with
  // the now-resident real covers.
  cancelAll();

  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  pageCache_.clear();
  pendingRender_ =
    enqueueLocked(page, pageCount);  // false → empty page, no batch-done to await
  xSemaphoreTake(batchDone_, 0);     // drop any stale completion signal
  xSemaphoreGive(cacheLock_);
}

bool CoverPrefetcher::setCoverBox(int w, int h) {
  if (w == coverBoxW_ && h == coverBoxH_) return false;
  coverBoxW_ = w;
  coverBoxH_ = h;
  return true;
}

bool CoverPrefetcher::consumeBatchDone() {
  if (batchDone_ == nullptr) return false;
  if (xSemaphoreTake(batchDone_, 0) != pdTRUE) return false;
  if (!pendingRender_) return false;
  pendingRender_ = false;
  return true;
}

void CoverPrefetcher::taskTrampoline(void* ctx) {
  auto* self = static_cast<CoverPrefetcher*>(ctx);
  self->taskLoop();

  xSemaphoreGive(self->exited_);
  while (true) vTaskDelay(portMAX_DELAY);
}

void CoverPrefetcher::taskLoop() {
  while (true) {
    // Wait for work. The owner gives the semaphore on enqueue or on cancel/shutdown
    // (so we always wake to re-check our state).
    xSemaphoreTake(signal_, portMAX_DELAY);

    while (true) {
      uint8_t targetPage = 0xFF;
      xSemaphoreTake(cacheLock_, portMAX_DELAY);
      if (shutdown_) {
        xSemaphoreGive(cacheLock_);
        return;
      }
      if (queueCount_ > 0) {
        targetPage = queue_[0];
        for (uint8_t i = 1; i < queueCount_; ++i) {
          queue_[i - 1] = queue_[i];
        }
        queue_[--queueCount_] = 0xFF;
        // Reset the cancel flag for the new work unit. Old in-flight cancellations
        // were already honored by the previous iteration.
        cancel_ = false;
      }
      xSemaphoreGive(cacheLock_);
      if (targetPage == 0xFF) break;  // queue empty, go back to sleep

      // Decode each of the page's covers. Path build, cache check, scaling, and
      // cache.set are lock-protected; only the SD decode runs unlocked so the render
      // thread can still touch the cache for hits while we're working.
      bool batchCompleted = true;  // false if we broke early (cancel/shutdown)
      for (uint8_t slot = 0; slot < perPage_; ++slot) {
        if (cancel_ || shutdown_) {
          batchCompleted = false;
          break;
        }
        char path[64];
        bool pathOk = false;
        xSemaphoreTake(cacheLock_, portMAX_DELAY);
        pathOk = buildThumbPath(targetPage, slot, path, sizeof(path));
        bool alreadyCached = false;
        if (pathOk) {
          alreadyCached = (pageCache_.get(path) != nullptr);
        }
        xSemaphoreGive(cacheLock_);
        if (!pathOk || alreadyCached) continue;

        // Slow path — decode the 2bpp source into the reused scratch off-lock.
        // HalStorage serializes its own SD access, so the render thread can still
        // read the cache for hits while this runs. The scratch is single-producer
        // (only this worker touches it).
        int srcW = 0;
        int srcH = 0;
        bool topDown = false;
        if (!GfxRenderer::decodeThumbInto(
              path, decodeScratch_.get(), DECODE_SCRATCH_BYTES, srcW, srcH, topDown
            )) {
          continue;  // decode failed / no scratch — nothing to insert
        }

        int targetW = 0;
        int targetH = 0;
        GfxRenderer::computeThumbTarget(
          srcW, srcH, coverBoxW_, coverBoxH_, targetW, targetH
        );
        if (targetW <= 0 || targetH <= 0) continue;

        if (cancel_ || shutdown_) {
          batchCompleted = false;
          break;
        }

        // Scale into a fresh 1bpp arena raster and store, under the lock. The scaling
        // reads the scratch (no other writer) and the arena mutates only here / in
        // eviction — all serialized by cacheLock_.
        xSemaphoreTake(cacheLock_, portMAX_DELAY);
        if (!shutdown_) {
          BitmapCacheManager::Entry entry;
          entry.path = path;
          entry.width = srcW;
          entry.height = srcH;
          entry.topDown = topDown;
          if (
            renderer_.buildScaledFromSource(
              entry,
              decodeScratch_.get(),
              srcW,
              srcH,
              topDown,
              targetW,
              targetH,
              pageCache_.arena()
            )
          ) {
            pageCache_.set(std::move(entry));
          }
          // else: arena full — skip; the cover paints a placeholder.
        }
        xSemaphoreGive(cacheLock_);
      }
      // Signal that this page's covers are resident. Suppressed on cancel — the
      // partial state shouldn't paint.
      if (batchCompleted && !shutdown_) {
        xSemaphoreGive(batchDone_);
      }
    }
  }
}
