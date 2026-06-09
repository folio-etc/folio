#include "LibraryActivity.h"

#include <Bitmap.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "LibraryIndex.h"
#include "MappedInputManager.h"
#include "util/Flex.h"
#include "util/GridHelper.h"
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "components/ui/Cover/Cover.h"
#include "components/ui/ProgressBar/ProgressBar.h"
#include "components/ui/TextBlock/TextBlock.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.generated.h"
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"

namespace {
constexpr char LOG_TAG[] = "LIBA";

// Resolve font roles against the currently-active theme so Library follows
// whichever theme the user has selected. Each theme provides its own role
// mapping (and SD-installed face, if any) via ThemeData; Library now picks
// up sans-serif Lyra fonts under Lyra, Folio serif under Folio, etc.
int libFont(FontRole role) { return GUI.getFontForRole(role); }

// Layout (in physical-pixel coordinates, portrait orientation 480 × 800).
// Header is matched to the prototype: 89 px tall with a 3 px inner border.
constexpr int HEADER_HEIGHT = 89;
constexpr int HEADER_BOTTOM_BORDER = 3;
constexpr int FOOTER_HEIGHT = 40;

constexpr int CONTENT_PAD_X = 18;
constexpr int CONTENT_PAD_Y = 8;
constexpr int RAIL_WIDTH = 18;
constexpr int RAIL_GAP = 10;

// Book tile geometry. Tile height is fixed (not adaptive to content) so the
// selection frame, drop shadow, and inter-tile spacing read the same for
// every book — whether the title fits on one line or two, whether the
// author is set, whether the book has progress, etc. Title + author are
// then centered vertically inside the reserved text area.
constexpr int COVER_W = 80;
constexpr int COVER_H = 120;
constexpr int CELL_GAP = 14;
constexpr int TILE_PAD_TOP = 4;
constexpr int TILE_TEXT_AREA_H = 60;             // reserved for title + author, regardless of content
constexpr int TILE_TITLE_AUTHOR_GAP = 1;         // gap between title's last line and author
constexpr int TILE_PROGRESS_MARGIN_TOP = 3;
constexpr int TILE_PROGRESS_HEIGHT = 4;          // inner fill height
constexpr int TILE_PROGRESS_TOTAL_H = TILE_PROGRESS_HEIGHT + 2;  // + 2 for 1-px border each side
// Total fixed tile content height — same for every book.
constexpr int TILE_CONTENT_H = COVER_H + TILE_TEXT_AREA_H + TILE_PROGRESS_MARGIN_TOP + TILE_PROGRESS_TOTAL_H;

// Page rail tick geometry.
constexpr int RAIL_PAD_TOP = 8;

}  // namespace

// ---- Lifecycle --------------------------------------------------------------

void LibraryActivity::onEnter() {
  Activity::onEnter();

  // Ignore the Confirm release that brought us here (otherwise we'd
  // immediately auto-open the first book).
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);

  initPopup();

  // The on-disk library.bin survives onExit; load it back into memory.
  LIBRARY_INDEX.loadFromFile();

  // Always do a refresh pass — picks up newly added EPUBs, drops removed ones,
  // and refreshes progress. New EPUBs trigger a blocking "Indexing library..."
  // popup; the popup never appears when nothing has changed.
  LIBRARY_INDEX.refreshFromSdCard(&renderer);

  // Apply persisted sort. LibraryIndex no longer pre-sorts in
  // refreshFromSdCard — order is the LibraryActivity's responsibility now.
  LIBRARY_INDEX.sortBy(static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
                      static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection));

  this->gridHelper = GridHelper(LIBRARY_INDEX.getBookCount(), ROWS, COLS, 0);
  this->lastObservedPage_ = this->gridHelper.currentPage();

  // Spin up the prefetch worker before requesting the first paint so it's
  // ready by the time we ask for neighbor-page fills.
  // cacheLock_ is a binary semaphore (created taken, then given) rather
  // than a true xSemaphoreCreateMutex. A mutex tracks ownership for
  // priority inheritance — its release path calls xTaskPriorityDisinherit
  // which assertions on `holder == currentTask`. Under Lyra we were
  // tripping that assertion; switching to a binary semaphore avoids the
  // inheritance accounting entirely while preserving mutual exclusion.
  // All tasks here run at priority 1 (Arduino main loop, render task,
  // prefetch worker), so priority inversion isn't a concern.
  cacheLock_ = xSemaphoreCreateBinary();
  prefetchSignal_ = xSemaphoreCreateBinary();
  prefetchExited_ = xSemaphoreCreateBinary();
  prefetchBatchDone_ = xSemaphoreCreateBinary();
  assert(cacheLock_ != nullptr && prefetchSignal_ != nullptr && prefetchExited_ != nullptr &&
         prefetchBatchDone_ != nullptr);
  // Binary semaphores from xSemaphoreCreateBinary are born "taken"
  // (count = 0). Give once so the first acquirer can take it.
  xSemaphoreGive(cacheLock_);
  prefetchCancel_ = false;
  prefetchShutdown_ = false;
  prefetchQueueCount_ = 0;
  for (auto& slot : prefetchQueue_) slot = 0xFF;
  xTaskCreate(&prefetchTaskTrampoline, "LibPrefetch",
              4096,            // stack: SD I/O + bitmap parsing
              this, 1,         // priority: same tier as render
              &prefetchTask_);
  assert(prefetchTask_ != nullptr);

  // First impression matters — prefetch both neighbor pages so the first
  // page-turn (in either direction) paints from cache. The current page
  // is still loaded synchronously by the render path on the first paint
  // below; it's the neighbors that benefit from getting a head start.
  const uint8_t cur = lastObservedPage_;
  const uint8_t pages = std::max<uint8_t>(1, gridHelper.pageCount());
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  if (pages > 1) {
    enqueuePrefetchLocked((cur + pages - 1) % pages);  // prev (wraps)
    enqueuePrefetchLocked((cur + 1) % pages);          // next (wraps)
  }
  xSemaphoreGive(cacheLock_);

  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();

  // Free Library-only state that other activities don't need. SD role fonts
  // are NOT unloaded here — that would silently drop typography in every
  // subsequent UI activity (Settings, Browse, etc. would lose the Folio
  // serif and fall back to embedded sans). The book-open path in doSelect()
  // does its own font unload before goToReader, which is the one exit that
  // actually needs the RAM.

  // Tear down the prefetch worker BEFORE clearing the cache or destroying
  // the lock — the worker reads both, so the order matters. Set shutdown
  // + cancel under the lock (so any worker wake observes them coherently),
  // signal once to wake the worker, then wait on prefetchExited_ which
  // the worker gives just before parking. Parent calls vTaskDelete so we
  // never poll a potentially-dangling self-deleted handle.
  if (prefetchTask_ != nullptr) {
    xSemaphoreTake(cacheLock_, portMAX_DELAY);
    prefetchShutdown_ = true;
    prefetchCancel_ = true;
    prefetchQueueCount_ = 0;
    xSemaphoreGive(cacheLock_);
    xSemaphoreGive(prefetchSignal_);
    // Worker may be mid-decode; wait for it to observe shutdown between
    // covers and exit its main loop. Worst case ~500 ms (one cover decode).
    xSemaphoreTake(prefetchExited_, portMAX_DELAY);
    // Worker is parked in vTaskDelay(portMAX_DELAY) and holds no
    // resources; safe to delete from here.
    vTaskDelete(prefetchTask_);
    prefetchTask_ = nullptr;
  }
  if (prefetchSignal_ != nullptr) {
    vSemaphoreDelete(prefetchSignal_);
    prefetchSignal_ = nullptr;
  }
  if (prefetchExited_ != nullptr) {
    vSemaphoreDelete(prefetchExited_);
    prefetchExited_ = nullptr;
  }
  if (prefetchBatchDone_ != nullptr) {
    vSemaphoreDelete(prefetchBatchDone_);
    prefetchBatchDone_ = nullptr;
  }

  // The cover cache holds up to 27 1bpp pre-scaled covers (~2 KB each →
  // ~55 KB total worst case at 120×120; ~35 KB for typical 80×120 covers).
  // The 2bpp decoded source is freed inside buildScaledBitmap as soon as
  // the 1bpp raster is built, so resident-set is scaled-only. Drop them
  // on exit — the next paint of any other UI doesn't render thumbnails.
  pageCache_.clear();

  if (cacheLock_ != nullptr) {
    vSemaphoreDelete(cacheLock_);
    cacheLock_ = nullptr;
  }

  LIBRARY_INDEX.unload();
}

// ---- Prefetch worker --------------------------------------------------------

bool LibraryActivity::buildThumbPath(uint8_t page, uint8_t slot, char* out,
                                     std::size_t outSize) const {
  if (out == nullptr || outSize < 64) return false;
  const int idx = static_cast<int>(page) * PER_PAGE + static_cast<int>(slot);
  const LibraryBook* book = LIBRARY_INDEX.getAt(idx);
  if (book == nullptr) {
    out[0] = '\0';
    return false;
  }
  snprintf(out, outSize, "/.crosspoint/epub_%lu/thumb_%d.bmp",
           static_cast<unsigned long>(book->pathHash), LibraryIndex::THUMB_HEIGHT);
  return true;
}

void LibraryActivity::enqueuePrefetchLocked(uint8_t page) {
  // Validate against the current library size — pageCount() includes a
  // partial trailing page when applicable.
  const uint8_t pages = gridHelper.pageCount();
  if (pages == 0 || page >= pages) return;

  // Dedupe against the existing queue so repeated requests for the same
  // page don't pile up (e.g., quick A→B→A bouncing).
  for (uint8_t i = 0; i < prefetchQueueCount_; ++i) {
    if (prefetchQueue_[i] == page) return;
  }

  if (prefetchQueueCount_ >= PREFETCH_QUEUE_CAPACITY) {
    // Drop the oldest pending — newer requests reflect more recent
    // user intent. Shift down one slot.
    for (uint8_t i = 1; i < prefetchQueueCount_; ++i) {
      prefetchQueue_[i - 1] = prefetchQueue_[i];
    }
    --prefetchQueueCount_;
  }
  prefetchQueue_[prefetchQueueCount_++] = page;
  xSemaphoreGive(prefetchSignal_);
}

void LibraryActivity::cancelAllPrefetch() {
  if (cacheLock_ == nullptr) return;
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  prefetchCancel_ = true;
  prefetchQueueCount_ = 0;
  for (auto& slot : prefetchQueue_) slot = 0xFF;
  xSemaphoreGive(cacheLock_);
  // Wake the worker if it's currently sleeping on the signal so it can
  // observe the cleared queue and go back to sleep. Without this, a
  // worker that just finished its previous page would sit on the signal
  // forever if we never gave it again.
  xSemaphoreGive(prefetchSignal_);
}

void LibraryActivity::evictPagesOutsideKeepSetLocked(uint8_t centerPage) {
  const uint8_t pages = std::max<uint8_t>(1, gridHelper.pageCount());
  const uint8_t prev = (centerPage + pages - 1) % pages;
  const uint8_t next = (centerPage + 1) % pages;
  // Materialize the keep-set thumb paths up front (at most 27), then
  // evictIf does a linear scan of cached entries against this list.
  // Stack cost is ~1.7 KB — main task has 8 KB so it's well within budget.
  char keepPaths[PER_PAGE * 3][64];
  std::size_t keepCount = 0;
  auto addPagePaths = [&](uint8_t page) {
    for (uint8_t slot = 0; slot < PER_PAGE; ++slot) {
      if (buildThumbPath(page, slot, keepPaths[keepCount], sizeof(keepPaths[0]))) {
        ++keepCount;
      }
    }
  };
  addPagePaths(prev);
  addPagePaths(centerPage);
  if (next != prev) addPagePaths(next);  // single-page library: prev==cur==next

  pageCache_.evictIf([&](const std::string& path) {
    for (std::size_t i = 0; i < keepCount; ++i) {
      if (path == keepPaths[i]) return false;
    }
    return true;
  });
}

void LibraryActivity::primeNeighborhoodLazy(uint8_t centerPage) {
  cancelAllPrefetch();
  const uint8_t pages = std::max<uint8_t>(1, gridHelper.pageCount());

  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  evictPagesOutsideKeepSetLocked(centerPage);
  enqueuePrefetchLocked(centerPage);
  if (pages > 1) {
    enqueuePrefetchLocked((centerPage + pages - 1) % pages);
    enqueuePrefetchLocked((centerPage + 1) % pages);
  }
  // Drain any pending batch-done signal from a previous batch — we only
  // care about the batch we're enqueuing now. Then enter lazy mode so
  // the render path uses peek instead of triggering 1–2 s of synchronous
  // SD decode for covers that may already be in the prefetch queue
  // ahead of the render request.
  xSemaphoreTake(prefetchBatchDone_, 0);
  lazyLoadCurrentPage_ = true;
  xSemaphoreGive(cacheLock_);
}

void LibraryActivity::onPageChanged(uint8_t oldPage, uint8_t newPage) {
  if (oldPage == newPage || cacheLock_ == nullptr) return;

  // Don't disturb the cache or prefetch queue while a rapid-jump is in
  // progress — endRapidJumpIfActive owns the cleanup once the button
  // releases. (A user can still press Left/Right while holding Down; in
  // that rare case, we just let lastObservedPage_ drift; the rapid-jump
  // release path treats the landed page as cold anyway.)
  if (rapidJumping_) {
    lastObservedPage_ = newPage;
    return;
  }

  // Cancel any in-flight prefetch and drop queued requests — they refer
  // to a neighborhood that's about to shift. We'll re-enqueue below.
  cancelAllPrefetch();

  // A normal page step implies the new current page is already cached
  // (it was the prev/next of the previous current page). If somehow
  // it isn't, getCachedBitmapDimensions will do a synchronous decode —
  // the same fallback as before this commit. Either way, lazy-load
  // shouldn't persist past an explicit page change.
  lazyLoadCurrentPage_ = false;

  const uint8_t pages = std::max<uint8_t>(1, gridHelper.pageCount());

  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  evictPagesOutsideKeepSetLocked(newPage);

  // Detect single-step transitions (the common case: user pages one over,
  // so the opposite-direction neighbor is still resident). Multi-page
  // jumps (rapid-jump release, sort-induced reposition) need to refill
  // both neighbors since neither is cached.
  const uint8_t forwardNeighbor = (newPage + 1) % pages;
  const uint8_t backwardNeighbor = (newPage + pages - 1) % pages;
  const bool steppedForward = (newPage == (oldPage + 1) % pages);
  const bool steppedBackward = (oldPage == (newPage + 1) % pages);
  if (steppedForward && pages > 1) {
    enqueuePrefetchLocked(forwardNeighbor);
  } else if (steppedBackward && pages > 1) {
    enqueuePrefetchLocked(backwardNeighbor);
  } else if (pages > 1) {
    enqueuePrefetchLocked(backwardNeighbor);
    enqueuePrefetchLocked(forwardNeighbor);
  }
  xSemaphoreGive(cacheLock_);

  lastObservedPage_ = newPage;
}

void LibraryActivity::prefetchTaskTrampoline(void* ctx) {
  auto* self = static_cast<LibraryActivity*>(ctx);
  self->prefetchTaskLoop();
  // Signal that the worker has exited its main loop and is no longer
  // touching cacheLock_ or pageCache_. The parent waits on this in
  // onExit before calling vTaskDelete on us, so we know we won't be
  // killed mid-lock-hold. Park here until the parent reaps.
  xSemaphoreGive(self->prefetchExited_);
  while (true) vTaskDelay(portMAX_DELAY);
}

void LibraryActivity::prefetchTaskLoop() {
  while (true) {
    // Wait for work. Activity gives the semaphore on enqueue or on
    // cancel/shutdown (so we always wake to re-check our state).
    xSemaphoreTake(prefetchSignal_, portMAX_DELAY);

    while (true) {
      uint8_t targetPage = 0xFF;
      xSemaphoreTake(cacheLock_, portMAX_DELAY);
      if (prefetchShutdown_) {
        xSemaphoreGive(cacheLock_);
        return;
      }
      if (prefetchQueueCount_ > 0) {
        targetPage = prefetchQueue_[0];
        for (uint8_t i = 1; i < prefetchQueueCount_; ++i) {
          prefetchQueue_[i - 1] = prefetchQueue_[i];
        }
        prefetchQueue_[--prefetchQueueCount_] = 0xFF;
        // Reset the cancel flag for the new work unit. Old in-flight
        // cancellations were already honored by the previous iteration.
        prefetchCancel_ = false;
      }
      xSemaphoreGive(cacheLock_);
      if (targetPage == 0xFF) break;  // queue empty, go back to sleep

      // Decode each of the 9 covers. Path build, cache check, and
      // cache.set are lock-protected; the SD decode itself runs
      // unlocked so the render thread can still touch the cache for
      // hits while we're working.
      bool batchCompleted = true;  // false if we broke early (cancel/shutdown)
      for (uint8_t slot = 0; slot < PER_PAGE; ++slot) {
        if (prefetchCancel_ || prefetchShutdown_) {
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

        // Slow path — decode off-lock. HalStorage serializes its own
        // SD access, so the render thread can still read the cache for
        // hits while this runs.
        auto entry = GfxRenderer::decodeBitmapEntry(path);
        if (entry.path.empty()) continue;  // decode failed; nothing to insert

        if (prefetchCancel_ || prefetchShutdown_) {
          batchCompleted = false;
          break;
        }

        xSemaphoreTake(cacheLock_, portMAX_DELAY);
        if (!prefetchShutdown_) {
          pageCache_.set(std::move(entry));
        }
        xSemaphoreGive(cacheLock_);
      }
      // Signal the activity loop that this page's covers are resident
      // in the cache. Activity uses this to clear lazyLoadCurrentPage_
      // and trigger a repaint when the awaited page lands. Suppressed
      // on cancel — the partial state shouldn't drop the placeholder.
      if (batchCompleted && !prefetchShutdown_) {
        xSemaphoreGive(prefetchBatchDone_);
      }
    }
  }
}

void LibraryActivity::initPopup() {
  // Configure the cascading popup. Top rows: Sort (→ submenu), Files (→
  // submenu), Settings (leaf). The cascade derives chevron glyphs and the
  // muted-owning-row indicator from this config — LibraryActivity only owns
  // the labels and the action dispatch.
  std::vector<CascadingPopupMenu::SubmenuConfig> subs;
  subs.resize(POPUP_TOP_COUNT);

  // Sort submenu: 4 rows; pre-select the active sort field on entry; show a
  // direction arrow on the active field row.
  subs[POPUP_TOP_SORT].itemCount = POPUP_SORT_COUNT;
  subs[POPUP_TOP_SORT].rowLabel = [](int i) -> const char* {
    switch (i) {
      case 0: return tr(STR_SORT_RECENT);
      case 1: return tr(STR_SORT_TITLE);
      case 2: return tr(STR_SORT_AUTHOR);
      case 3: return tr(STR_SORT_PROGRESS);
    }
    return "";
  };
  subs[POPUP_TOP_SORT].rowGlyph = [](int i) -> PopupMenu::Glyph {
    if (i != SETTINGS.librarySortField) return PopupMenu::Glyph::None;
    return (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC)
               ? PopupMenu::Glyph::ArrowUp
               : PopupMenu::Glyph::ArrowDown;
  };
  subs[POPUP_TOP_SORT].initialSelection = []() { return static_cast<int>(SETTINGS.librarySortField); };

  // Files submenu: 2 rows; always opens at row 0.
  subs[POPUP_TOP_FILES].itemCount = POPUP_FILES_COUNT;
  subs[POPUP_TOP_FILES].rowLabel = [](int i) -> const char* {
    switch (i) {
      case 0: return tr(STR_BROWSE);
      case 1: return tr(STR_TRANSFER);
    }
    return "";
  };

  // Power Button submenu: 2 rows; opens with cursor on the active option so
  // the user can see at-a-glance which behavior is current (no glyph
  // needed — the pre-selection serves as the indicator).
  subs[POPUP_TOP_POWER].itemCount = POPUP_POWER_COUNT;
  subs[POPUP_TOP_POWER].rowLabel = [](int i) -> const char* {
    switch (i) {
      case 0: return tr(STR_POWER_NEXT_IN_ROW);
      case 1: return tr(STR_POWER_NEXT_BOOK);
    }
    return "";
  };
  subs[POPUP_TOP_POWER].initialSelection = []() { return static_cast<int>(SETTINGS.libraryPowerButton); };

  // Settings is a leaf — no submenu config needed (itemCount=0 default).
  popup_.configure(
      [](int i) -> const char* {
        switch (i) {
          case POPUP_TOP_SORT: return tr(STR_SORT);
          case POPUP_TOP_FILES: return tr(STR_FILES);
          case POPUP_TOP_POWER: return tr(STR_POWER_BUTTON);
          case POPUP_TOP_SETTINGS: return tr(STR_SETTINGS_TITLE);
        }
        return "";
      },
      std::move(subs));
}

// ---- Input ------------------------------------------------------------------

void LibraryActivity::loop() {
  // Pick up the prefetch worker's batch-completion signal. The current
  // page is enqueued first by applySort / endRapidJumpIfActive, so the
  // first batch-done after entering lazy mode means current-page covers
  // are now resident — clear the flag and repaint to swap placeholders
  // for the real artwork. Subsequent batch-dones (prev / next neighbors)
  // arrive with the flag already clear and are simply drained.
  if (prefetchBatchDone_ != nullptr &&
      xSemaphoreTake(prefetchBatchDone_, 0) == pdTRUE) {
    if (lazyLoadCurrentPage_) {
      lazyLoadCurrentPage_ = false;
      requestUpdate();
    }
  }

  // End-of-rapid-jump detection: once both continuous-bound buttons are
  // released, restore normal prefetch + cache loading and repaint with the
  // real covers for the landed page. ButtonNavigator doesn't expose a
  // release-after-continuous hook, so we sample the input state here.
  if (rapidJumping_ && !mappedInput.isPressed(MappedInputManager::Button::Up) &&
      !mappedInput.isPressed(MappedInputManager::Button::Down)) {
    endRapidJumpIfActive();
  }

  // Suppress the just-pressed Confirm release that brought us here.
  if (lockNextConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      lockNextConfirmRelease = false;
    }
    return;
  }

  if (lockNextBackRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      lockNextBackRelease = false;
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if(popup_.isOpen()) {
      popup_.moveLeft();
    } else {
      popup_.open();
    }

    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSelect();
    return;
  }

  // Up/Down: tap = single-row move; hold ≥500ms = page jump (wraps past
  // the first/last page) repeating every 500ms. ButtonNavigator's
  // onRelease skips the tap callback if a continuous fired during the
  // hold, so tap-vs-hold are separated without an explicit timer here.
  // Note: this shifts Up/Down from press-time to release-time firing,
  // matching every settings-list activity in the app.
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] { moveUp(); });
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] { moveDown(); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this] {
    if (popup_.isOpen()) return;
    jumpPageBack();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this] {
    if (popup_.isOpen()) return;
    jumpPageForward();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveLeft();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    moveRight();
    return;
  }
}

// ---- Navigation -------------------------------------------------------------

void LibraryActivity::moveUp() {
  if (popup_.isOpen()) {
    if (popup_.moveUp() != CascadingPopupMenu::Nav::Ignored) requestUpdate();
    return;
  }

  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.up();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) onPageChanged(oldPage, newPage);
  requestUpdate();
}

void LibraryActivity::moveDown() {
  if (popup_.isOpen()) {
    if (popup_.moveDown() != CascadingPopupMenu::Nav::Ignored) {
      requestUpdate();
    }

    return;
  }

  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.down();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) onPageChanged(oldPage, newPage);
  requestUpdate();
}


void LibraryActivity::jumpPageForward() {
  if (popup_.isOpen()) return;
  const uint8_t pages = gridHelper.pageCount();
  if (pages <= 1) return;
  const uint8_t oldPage = gridHelper.currentPage();
  const uint8_t row = gridHelper.currentRow();
  const uint8_t col = gridHelper.currentCol();
  const uint8_t newPage = (oldPage + 1) % pages;
  gridHelper.setByRowColPage(row, col, newPage);
  // During a held rapid-jump, suppress prefetch / SD work — we want the
  // render path to take the placeholder fallback for any uncached covers.
  // Page-aware retention + prefetch resumes when the button is released
  // (see the onContinuous binding's release handler).
  if (!rapidJumping_) {
    rapidJumping_ = true;
    cancelAllPrefetch();
  }
  lastObservedPage_ = gridHelper.currentPage();
  requestUpdate();
}

void LibraryActivity::jumpPageBack() {
  if (popup_.isOpen()) return;
  const uint8_t pages = gridHelper.pageCount();
  if (pages <= 1) return;
  const uint8_t oldPage = gridHelper.currentPage();
  const uint8_t row = gridHelper.currentRow();
  const uint8_t col = gridHelper.currentCol();
  const uint8_t newPage = (oldPage + pages - 1) % pages;
  gridHelper.setByRowColPage(row, col, newPage);
  if (!rapidJumping_) {
    rapidJumping_ = true;
    cancelAllPrefetch();
  }
  lastObservedPage_ = gridHelper.currentPage();
  requestUpdate();
}

void LibraryActivity::moveLeft() {
  if (popup_.isOpen()) {
    return;
  }

  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.left();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) onPageChanged(oldPage, newPage);
  requestUpdate();
}

void LibraryActivity::moveRight() {
  if (popup_.isOpen()) {
    return;
  }

  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.right();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) onPageChanged(oldPage, newPage);
  requestUpdate();
}

void LibraryActivity::moveNext() {
  if (popup_.isOpen()) {
    return;
  }

  const uint8_t oldPage = gridHelper.currentPage();
  this->gridHelper.nextItem();
  const uint8_t newPage = gridHelper.currentPage();
  if (oldPage != newPage) onPageChanged(oldPage, newPage);
  requestUpdate();
}

// Called when the rapid-jump continuous binding stops firing (button
// released). Resumes normal page-aware prefetch for the landed page,
// using lazy-load so the user sees the new layout immediately with
// placeholders that fill in as covers decode (no 1–2 s freeze waiting
// on synchronous SD reads for nine covers).
void LibraryActivity::endRapidJumpIfActive() {
  if (!rapidJumping_) return;
  rapidJumping_ = false;

  const uint8_t cur = gridHelper.currentPage();
  primeNeighborhoodLazy(cur);
  lastObservedPage_ = cur;
  requestUpdate();  // paint placeholders; real covers follow async
}

void LibraryActivity::doSelect() {
  if (popup_.isOpen()) {
    const auto nav = popup_.activate();
    if (nav == CascadingPopupMenu::Nav::EnteredSubmenu) {
      requestUpdate();
    } else if (nav == CascadingPopupMenu::Nav::LeafActivated ||
               nav == CascadingPopupMenu::Nav::SubItemActivated) {
      dispatchPopupActivation(nav);
    }
    return;
  }

  const LibraryBook* book = LIBRARY_INDEX.getAt(gridHelper.currentIndex());
  if (book == nullptr) return;

  LOG_DBG(LOG_TAG, "Opening book: %s", book->path.c_str());

  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->clearCache();

  activityManager.goToReader(book->path);
}

void LibraryActivity::dispatchPopupActivation(CascadingPopupMenu::Nav navResult) {
  const int top = popup_.topSelectedIndex();
  if (navResult == CascadingPopupMenu::Nav::LeafActivated) {
    // Only Settings is a leaf at the top level.
    if (top == POPUP_TOP_SETTINGS) {
      activityManager.goToSettings();
    }
    return;
  }
  // SubItemActivated: dispatch by which submenu the user is in.
  const int sub = popup_.subSelectedIndex();
  if (top == POPUP_TOP_SORT) {
    // If the user re-confirms the active sort field, flip direction;
    // otherwise switch the active field and keep the persisted direction.
    const uint8_t newField = static_cast<uint8_t>(sub);
    uint8_t newDirection = SETTINGS.librarySortDirection;
    if (newField == SETTINGS.librarySortField) {
      newDirection = (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC)
                         ? CrossPointSettings::LIB_SORT_DESC
                         : CrossPointSettings::LIB_SORT_ASC;
    }
    setSort(newField, newDirection);
  } else if (top == POPUP_TOP_FILES) {
    if (sub == POPUP_FILES_BROWSE) {
      activityManager.goToFileBrowser();
    } else if (sub == POPUP_FILES_TRANSFER) {
      activityManager.goToFileTransfer();
    }
  } else if (top == POPUP_TOP_POWER) {
    const uint8_t newBehavior = static_cast<uint8_t>(sub);
    if (newBehavior != SETTINGS.libraryPowerButton) {
      SETTINGS.libraryPowerButton = newBehavior;
      SETTINGS.saveToFile();
    }
    popup_.close();
    requestUpdate();
  }
}

void LibraryActivity::applySort() {
  // Cancel any in-flight prefetch BEFORE taking cacheLock_ — the worker
  // may be holding the lock briefly between covers, and we don't want
  // to deadlock waiting for it. cancelAllPrefetch is non-blocking from
  // our perspective; the worker observes the cancel flag between
  // covers and parks itself.
  cancelAllPrefetch();

  // Hold cacheLock_ across sortBy. The worker reads LIBRARY_INDEX from
  // inside buildThumbPath (called under cacheLock_), so sorting outside
  // the lock would race with a worker that won the lock between
  // cancelAllPrefetch's release and our sortBy. That race was the
  // double-sort crash.
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  LIBRARY_INDEX.sortBy(static_cast<LibraryIndex::SortField>(SETTINGS.librarySortField),
                      static_cast<LibraryIndex::SortDirection>(SETTINGS.librarySortDirection));
  xSemaphoreGive(cacheLock_);

  // Land on page 0 of the new ordering, then reseed the cache around it.
  // book.pathHash is invariant under sort, so any cached cover whose
  // book lands in the new {prev, cur, next} neighborhood survives the
  // eviction step — for libraries that fit in three pages, no eviction
  // happens at all.
  this->gridHelper.setByIndex(0);
  lastObservedPage_ = 0;
  primeNeighborhoodLazy(0);

  requestUpdate();
}

void LibraryActivity::setSort(uint8_t field, uint8_t direction) {
  const bool changed =
      (field != SETTINGS.librarySortField) || (direction != SETTINGS.librarySortDirection);
  if (changed) {
    SETTINGS.librarySortField = field;
    SETTINGS.librarySortDirection = direction;
    SETTINGS.saveToFile();
  }
  applySort();
}

// ---- Render -----------------------------------------------------------------

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderPasses();
  renderer.displayBuffer();
}

void LibraryActivity::renderPasses() {
  // Top-level layout: header band, body (shelf + rail or empty state),
  // footer hint band. Outer Vstack spans the full screen — header and
  // footer are full-width by design; the body Hstack applies the content
  // padding that insets the shelf and anchors the rail to the right edge.
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  flex::Vstack top(screen,
                   {flex::fixed(HEADER_HEIGHT), flex::grow(), flex::fixed(FOOTER_HEIGHT)});

  renderHeader(top[0]);
  if (LIBRARY_INDEX.isEmpty()) {
    renderEmptyState(top[1]);
  } else {
    flex::Hstack body(top[1], {flex::grow(), flex::fixed(RAIL_WIDTH)}, RAIL_GAP,
                      flex::xy(CONTENT_PAD_X, CONTENT_PAD_Y));
    renderLibraryShelf(body[0]);
    renderPageRail(body[1]);
  }

  if (popup_.isOpen()) {
    renderPopup();
  }

  // Side-rail power-button hint. Only the power slot is populated — the
  // page-turn side buttons aren't bound in Library, so their hint slots
  // stay empty.
  // ButtonHints::renderSide(renderer, "", "", tr(STR_DIR_RIGHT));

  // Library-view footer hints. When the popup is open the cascade owns the
  // hint scheme (Close/Back, Select/Enter — see CascadingPopupMenu::renderFooterHints).
  if (popup_.isOpen()) {
    popup_.renderFooterHints(renderer, mappedInput);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_MENU_LABEL), tr(STR_SELECT), tr(STR_DIR_LEFT),
                                              tr(STR_DIR_RIGHT));
    ButtonHints::render(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

bool LibraryActivity::handlePowerShortPress() {
  // Short-press behavior is user-selectable from the Library popup:
  //  - Next in Row: stepRight within the current row, wrapping at row end
  //  - Next Book:   linear advance through the whole library (wraps at end)
  // Consuming the press suppresses the global FORCE_REFRESH dispatch.
  if (SETTINGS.libraryPowerButton == CrossPointSettings::LIB_PWR_NEXT_BOOK) {
    moveNext();
  } else {
    moveRight();
  }
  return true;
}

void LibraryActivity::renderHeader(const Rect& headerBox) {
  // Header band — 89 px tall with a 3 px bottom rule. Outer Vstack in
  // renderPasses sizes the band; this function lays out the interior.
  const int titleFont = libFont(FontRole::Title);
  const int captionFont = libFont(FontRole::Caption);
  const int titleLineH = renderer.getLineHeight(titleFont);
  const int captionLineH = renderer.getLineHeight(captionFont);

  // Title text starts at y=20, subtitle text starts at y=56 (both
  // historical — they match FolioTheme::drawHeader). The intervening band
  // has to absorb the title's line height; the trailing grow() takes
  // whatever's left above the 3 px bottom border.
  constexpr int kTitleTopOffset = 20;
  constexpr int kSubtitleTopOffset = 56;
  const int titleToSubtitleGap = kSubtitleTopOffset - kTitleTopOffset - titleLineH;
  flex::Vstack header(headerBox,
                      {flex::fixed(kTitleTopOffset),
                       flex::fixed(titleLineH),
                       flex::fixed(titleToSubtitleGap),
                       flex::fixed(captionLineH),
                       flex::grow(),
                       flex::fixed(HEADER_BOTTOM_BORDER)});
  const Rect& titleRow = header[1];
  const Rect& subtitleRow = header[3];
  const Rect& bottomBorder = header[5];

  // Battery icon top-right. Delegates to whichever theme is active —
  // intentional, the battery is one of the few elements LibraryActivity
  // happily inherits from the user's chosen theme.
  const auto& td = *GUI.getData();
  constexpr int kBatteryEraseWidth = 80;
  constexpr int kBatteryTopOffset = 5;
  constexpr int kBatteryRightInset = 12;

  // Eraser strip: 80 px wide, right-flush against the header's right edge,
  // tall enough to cover the battery widget. Wipes any prior content under
  // where the battery + percentage will render.
  const Rect eraseRow{headerBox.x, headerBox.y + kBatteryTopOffset, headerBox.width,
                      td.battery.height + 10};
  const Rect eraseRect = flex::align(eraseRow, kBatteryEraseWidth, eraseRow.height,
                                     flex::HAlign::End, flex::VAlign::Start);
  renderer.fillRect(eraseRect.x, eraseRect.y, eraseRect.width, eraseRect.height, false);

  // Battery widget: same row, right-flush after subtracting the 12 px right
  // inset that breathes the icon away from the bezel.
  const bool showBatteryPct =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const Rect batteryRow{headerBox.x, headerBox.y + kBatteryTopOffset,
                        headerBox.width - kBatteryRightInset, td.battery.height};
  const Rect batteryRect = flex::align(batteryRow, td.battery.width, td.battery.height,
                                       flex::HAlign::End, flex::VAlign::Start);
  GUI.drawBatteryRight(renderer, batteryRect, showBatteryPct);

  // Title — match FolioTheme::drawHeader's positions so the Library reads
  // identically whether or not Folio is the active theme.
  const char* title = tr(STR_LIBRARY);
  const int titleMaxWidth = batteryRect.x - (headerBox.x + CONTENT_PAD_X) - CONTENT_PAD_X;
  const std::string truncatedTitle =
      renderer.truncatedText(titleFont, title, titleMaxWidth, EpdFontFamily::BOLD);
  renderer.drawText(titleFont, titleRow.x + CONTENT_PAD_X, titleRow.y, truncatedTitle.c_str(),
                    true, EpdFontFamily::BOLD);

  // Subtitle — sort state and pagination indicator.
  std::string subtitleText;
  if (!LIBRARY_INDEX.isEmpty()) {
    StrId sortedKey = StrId::STR_LIBRARY_SORTED_RECENT;
    switch (SETTINGS.librarySortField) {
      case CrossPointSettings::LIB_SORT_TITLE:    sortedKey = StrId::STR_LIBRARY_SORTED_TITLE; break;
      case CrossPointSettings::LIB_SORT_AUTHOR:   sortedKey = StrId::STR_LIBRARY_SORTED_AUTHOR; break;
      case CrossPointSettings::LIB_SORT_PROGRESS: sortedKey = StrId::STR_LIBRARY_SORTED_PROGRESS; break;
      case CrossPointSettings::LIB_SORT_RECENT:
      default:                                    sortedKey = StrId::STR_LIBRARY_SORTED_RECENT; break;
    }
    // Direction marker uses ASCII so it renders in any font. The popup-menu
    // primitive paints proper triangle glyphs for the in-popup arrows; the
    // subtitle stays light-touch text.
    const char* arrow =
        (SETTINGS.librarySortDirection == CrossPointSettings::LIB_SORT_ASC) ? "(asc)" : "(desc)";
    subtitleText = std::string(I18n::getInstance().get(sortedKey)) + " " + arrow + "  ·  " +
                   std::to_string(this->gridHelper.currentPage()) + " / " +
                   std::to_string(this->gridHelper.pageCount());
  }
  if (!subtitleText.empty()) {
    const std::string truncatedSub =
        renderer.truncatedText(captionFont, subtitleText.c_str(),
                               headerBox.width - CONTENT_PAD_X * 2, EpdFontFamily::ITALIC);
    renderer.drawText(captionFont, subtitleRow.x + CONTENT_PAD_X, subtitleRow.y,
                      truncatedSub.c_str(), true, EpdFontFamily::ITALIC);
  }

  // 3 px inner bottom rule separating header from the shelf.
  renderer.fillRect(bottomBorder.x, bottomBorder.y, bottomBorder.width, bottomBorder.height);
}

void LibraryActivity::renderLibraryShelf(const Rect& shelfArea) {
  // 3×3 grid of book tiles. Flex::Grid handles the cell math (integer
  // truncation rounding, same as the prior hand-rolled helper).
  flex::Grid cells(shelfArea, ROWS, COLS, CELL_GAP, CELL_GAP);

  const uint8_t currentIndexOnPage = gridHelper.currentIndexOnPage();
  const uint8_t currentPage = gridHelper.currentPage();
  const uint8_t itemsPerPage = gridHelper.itemsPerPage();
  const uint16_t baseIndex = currentPage * itemsPerPage;

  for (int slot = 0; slot < PER_PAGE; ++slot) {
    const LibraryBook* book = LIBRARY_INDEX.getAt(baseIndex + slot);
    if (book == nullptr) continue;

    const Rect& cell = cells[slot];
    const bool selected = slot == currentIndexOnPage;

    // Selection is split into a background pass (fills — drawn BEFORE tile
    // content so the cover bitmap and text paint on top of the wash) and a
    // foreground pass (borders + brackets — drawn AFTER content so the
    // frame sits on top). Themes with only fills (Lyra's RoundedFill) are
    // no-op in foreground; themes with only borders (Folio's LayeredFrame)
    // are no-op in background.
    //
    // Horizontal inset hugs the content (3 px clears Folio's layered outer
    // + gap + inner stroke). Vertical inset is deliberately larger so the
    // frame "lifts" the tile with breathing room above and below instead
    // of clinging tightly to the cover/text bounds.
    constexpr int kFrameInsetX = 3;
    constexpr int kFrameInsetY = 8;
    Rect selectionRect{};
    if (selected) {
      const Rect content = tileContentRect(*book, cell);
      selectionRect = Rect{content.x - kFrameInsetX, content.y - kFrameInsetY,
                           content.width + kFrameInsetX * 2, content.height + kFrameInsetY * 2};
      GUI.drawSelectionBackground(renderer, selectionRect);
    }
    renderBookTile(cell, slot, *book, selected);
    if (selected) {
      GUI.drawSelectionForeground(renderer, selectionRect);
    }
  }
}

Rect LibraryActivity::tileContentRect(const LibraryBook& /*book*/, const Rect& cell) const {
  // Fixed-height tile: every card occupies the same vertical extent
  // regardless of title length, presence of author, or read state. Makes
  // the selection frame and inter-tile spacing read uniformly across the
  // shelf. Title + author get centered vertically inside the reserved
  // text area in renderBookTile.
  return Rect{cell.x, cell.y + TILE_PAD_TOP, cell.width, TILE_CONTENT_H};
}

void LibraryActivity::renderBookTile(const Rect& cell, int /*slotIndex*/,
                                     const LibraryBook& book, bool selected) {
  // When the active theme paints a dark selection background (Classic's
  // SolidFill, RoundedRaff's RoundedRowAlways), the tile's text + progress
  // bar need to render in inverted (white) ink to stay legible. The cover
  // bitmap is unaffected — Cover::render fills the cover area to white
  // before drawing the BMP, so the cover always sits on a white substrate
  // regardless of selection background.
  const bool invertText = selected && GUI.getData()->selection.textInverted;

  const int captionFont = libFont(FontRole::CaptionCompact);
  const int captionLineH = renderer.getLineHeight(captionFont);

  // ---- Cover -----
  // The cover slot is the full cell width × COVER_H — wide thumbnails
  // keep their natural width instead of being clipped to an 80-px slot.
  // Cover::render handles aspect-fit, drop shadow, border, and the
  // title-text fallback for books that don't have a cached thumbnail.
  const int coverY = cell.y + TILE_PAD_TOP;
  const Rect coverBox{cell.x, coverY, cell.width, COVER_H};

  char thumbPath[64];
  snprintf(thumbPath, sizeof(thumbPath), "/.crosspoint/epub_%lu/thumb_%d.bmp",
           static_cast<unsigned long>(book.pathHash), LibraryIndex::THUMB_HEIGHT);

  // Hold cacheLock_ across Cover::render — the widget probes dims and
  // calls drawCachedBitmap, both of which touch pageCache_. The read-only
  // peek variant is used in two cases: (1) while a rapid-jump is in
  // progress (no SD reads at all until the button releases), and (2)
  // while lazy-load is active (sort change / rapid-jump release — the
  // prefetch worker fills the cache asynchronously, so a miss here falls
  // through to the title-text fallback and the activity repaints when
  // the batch-done signal arrives).
  const bool useReadOnly = rapidJumping_ || lazyLoadCurrentPage_;
  const Cover::Fallback coverFallback{book.title.c_str(), captionFont, EpdFontFamily::BOLD};
  xSemaphoreTake(cacheLock_, portMAX_DELAY);
  Cover::render(renderer, coverBox, pageCache_, thumbPath, useReadOnly, invertText,
                COVER_W, COVER_H, &coverFallback);
  xSemaphoreGive(cacheLock_);



  // ---- Text area (fixed-height, title + author centered vertically) -----
  // Card height is fixed (see TILE_CONTENT_H), so the title + author block
  // gets centered inside the reserved space immediately below the cover.
  // Books with single-line titles or no author still occupy the same total
  // vertical extent — the leftover space becomes balanced padding above
  // and below the block.
  //
  // When the book has no progress bar, the reserved progress area (margin
  // + bar) becomes part of the centering range — otherwise unread books
  // look top-heavy with the progress slot sitting empty below.
  const int textAreaY = coverY + COVER_H;
  const int centeringH = book.hasProgress()
                             ? TILE_TEXT_AREA_H
                             : (TILE_TEXT_AREA_H + TILE_PROGRESS_MARGIN_TOP + TILE_PROGRESS_TOTAL_H);

  // Decide how many title lines fit. Author always reserves its line of
  // space when present, so the title gets whatever's left.
  const int authorReserved = book.author.empty() ? 0 : (TILE_TITLE_AUTHOR_GAP + captionLineH);
  const int titleBudget = centeringH - authorReserved;
  const int maxTitleLines = std::min(2, std::max(1, titleBudget / captionLineH));
  const std::vector<std::string> titleLines = renderer.wrappedText(
      captionFont, book.title.c_str(), cell.width - 8, maxTitleLines, EpdFontFamily::BOLD);

  // Pre-truncate the author here so TextBlock's per-line truncation budget
  // (which equals box.width) doesn't grant the author more horizontal room
  // than the title's wrap budget. Empty when the book has no author.
  const std::string authorTrunc =
      book.author.empty()
          ? std::string()
          : renderer.truncatedText(captionFont, book.author.c_str(), cell.width - 8,
                                   EpdFontFamily::ITALIC);

  // Assemble the text block: title lines (BOLD) + optional author (ITALIC,
  // with a 1 px lead-in gap). TextBlock vertically centers the whole stack
  // inside its box and horizontally centers each line on box.x + box.width/2.
  TextBlock::Line tlines[3];  // up to 2 title lines + 1 author line
  std::size_t tcount = 0;
  for (const auto& l : titleLines) {
    tlines[tcount++] = TextBlock::Line{l.c_str(), captionFont, EpdFontFamily::BOLD, 0, false};
  }
  if (!authorTrunc.empty()) {
    tlines[tcount++] = TextBlock::Line{authorTrunc.c_str(), captionFont, EpdFontFamily::ITALIC,
                                       TILE_TITLE_AUTHOR_GAP, false};
  }
  const bool textBlack = !invertText;
  const Rect textBox{cell.x, textAreaY, cell.width, centeringH};
  TextBlock::render(renderer, textBox, tlines, tcount, textBlack);

  // ---- Progress bar (fixed position, only drawn when book has progress) -
  // Bar Y is anchored at the end of the reserved text area, so every row's
  // bars align horizontally regardless of how each title wrapped. Unread
  // books just leave this area blank — preserving fixed card height.
  if (book.hasProgress()) {
    const int barY = textAreaY + TILE_TEXT_AREA_H + TILE_PROGRESS_MARGIN_TOP;
    const Rect progressSlot{cell.x, barY, cell.width, TILE_PROGRESS_TOTAL_H};
    const Rect barBox = flex::align(progressSlot, COVER_W, TILE_PROGRESS_TOTAL_H,
                                    flex::HAlign::Center, flex::VAlign::Start);
    ProgressBar::render(renderer, barBox, book.progressPercent(), textBlack);
  }
}

void LibraryActivity::renderPageRail(const Rect& railArea) {
  // Always render the rail so the user sees their position even on a single-
  // page library — the visual treatment is part of the Library's identity.
  // railArea spans the full vertical extent of the shelf; ticks get an
  // extra RAIL_PAD_TOP gap from the top so the first dot doesn't crowd
  // the header separator.
  const int pages = std::max(1, static_cast<int>(gridHelper.pageCount()));
  const int railTop = railArea.y + RAIL_PAD_TOP;
  const int railBottom = railArea.y + railArea.height;

  const auto& lib = GUI.getData()->library;
  const int tickSize = lib.pageIndicatorSize;
  const uint8_t currentPage = this->gridHelper.currentPage();

  for (int i = 0; i < pages; ++i) {
    const int tickRowY = railTop + i * (tickSize + lib.pageIndicatorGap);
    // Per-tick row slot spans the full rail width; the tick itself is
    // centered horizontally inside it via flex::align.
    const Rect tickRow{railArea.x, tickRowY, railArea.width, tickSize};
    const Rect tick =
        flex::align(tickRow, tickSize, tickSize, flex::HAlign::Center, flex::VAlign::Start);

    const Color fill = (i == currentPage) ? lib.pageIndicatorFillSelected : lib.pageIndicatorFill;
    const Color border = (i == currentPage) ? lib.pageIndicatorBorderSelected : lib.pageIndicatorBorder;

    switch (lib.pageIndicatorShape) {
      case IndicatorShape::Circle: {
        const int r = tickSize / 2;
        renderer.fillCircle(tick.x + r, tick.y + r, r, fill);
        renderer.drawCircle(tick.x + r, tick.y + r, r, border);
        break;
      }
      case IndicatorShape::Square:
        renderer.fillRectDither(tick.x, tick.y, tick.width, tick.height, fill);
        renderer.drawRect(tick.x, tick.y, tick.width, tick.height, border == Color::Black);
        break;
      case IndicatorShape::RoundedRect:
        renderer.fillRoundedRect(tick.x, tick.y, tick.width, tick.height,
                                 lib.pageIndicatorCornerRadius, fill);
        renderer.drawRoundedRect(tick.x, tick.y, tick.width, tick.height, 1,
                                 lib.pageIndicatorCornerRadius, border == Color::Black);
        break;
    }
  }

  // Page count at the bottom of the rail, rotated 90° clockwise for that
  // editorial vertical-marginalia feel from the prototype. Uses the
  // Compact accent so the vertical marginalia stays tight against the
  // rail; falls through to the regular accent when no compact face is
  // installed.
  const int accentFont = libFont(FontRole::AccentCompact);
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d / %d", currentPage + 1, pages);
  const int countY = railBottom - 4;
  const int countX = railArea.x + railArea.width / 2 + 4;
  renderer.drawTextRotated90CW(accentFont, countX, countY, countBuf, true, EpdFontFamily::ITALIC);
}

void LibraryActivity::renderEmptyState(const Rect& body) {
  // "No books on SD card" — italic heading, centered both axes inside the
  // body region. The pre-flex build centered the text *top* on the body
  // midpoint (no line-height subtraction); flex::center places the text
  // *box* in the middle, which reads as the correct center.
  const int font = libFont(FontRole::Heading);
  const char* msg = tr(STR_LIBRARY_NO_BOOKS);
  const int textW = renderer.getTextWidth(font, msg, EpdFontFamily::ITALIC);
  const int textH = renderer.getLineHeight(font);
  const Rect at = flex::center(body, textW, textH);
  renderer.drawText(font, at.x, at.y, msg, true, EpdFontFamily::ITALIC);
}

void LibraryActivity::renderPopup() {
  // Cascading popup over the library shelf, anchored above the footer hints.
  // The cascade computes its own panel widths and heights from the active
  // theme's metrics and the registered submenus — LibraryActivity just
  // supplies the anchor point and the available right edge for the sub-panel.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int leftX = CONTENT_PAD_X + 12;
  const int bottomLimit = screenH - FOOTER_HEIGHT - 6;
  const int rightLimit = screenW - CONTENT_PAD_X;
  popup_.render(renderer, leftX, bottomLimit, rightLimit);
}
