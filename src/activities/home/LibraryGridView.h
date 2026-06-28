#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "CoverPrefetcher.h"
#include "LibraryIndex.h"  // LibraryBook
#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"  // Rect, FontRole
#include "util/ButtonNavigator.h"
#include "util/GridHelper.h"

class GfxRenderer;

// The cover-grid view: a 3x3 page of book tiles over a resolved subset. Owns its
// navigation, selection, cover prefetcher, and grid rendering. The hosting shell
// (LibraryActivity) owns the page scaffold (header, scroll indicator, empty state) and
// resolves the subset; it drives this view through the surface below.
//
// Concrete and grid-specific by design — a future list view is a sibling class, and a
// shared interface gets lifted only once that second implementation exists.
class LibraryGridView {
 public:
  // The resolved subset the shell hands in. `hasBackTile` inserts a synthetic "back to
  // All Books" affordance at grid slot 0 (filtered views only).
  struct Subset {
    std::vector<uint32_t> books;  // entry indices into LIBRARY_INDEX, in display order
    std::string title;
    bool hasBackTile = false;
  };

  // Where the cursor lands after a subset swap. FirstBook skips the back tile on a
  // filtered view; Top always lands on slot 0 (the back tile, when present).
  enum class InitialSelection { FirstBook, Top };

  LibraryGridView(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput) {}

  // ---- Lifecycle (called from the shell's onEnter/onExit) ----
  // Set the first subset (before the worker exists, so no lock), size the prefetcher to
  // `content`, and load the current page. Cursor lands on the first real book.
  void onEnter(const Rect& content, Subset&& subset);
  void onExit();  // stop + free the prefetcher

  // Swap to a new subset post-start. `produce` runs INSIDE the prefetcher cache lock
  // (the worker reads subset_ via the resolver under it) — the shell injects sort/resolve
  // there. Does not repaint; the caller drives requestUpdate.
  void setSubset(const std::function<Subset()>& produce, InitialSelection sel);

  // ---- Per-loop (driven by the shell's loop) ----
  bool
  pollCoverUpdates();  // true once per completed prefetch batch while a page is pending
  bool handleInput();  // rapid-jump end + Up/Down tap-or-hold + Left/Right; true if a
                       // repaint is needed

  // ---- Render the grid body into `content` (the shelf area the shell computed) ----
  void renderBody(const Rect& content);

  // ---- Power-button overrides ----
  void moveNext();   // advance linearly, wrapping
  void moveRight();  // next in row

  // ---- Queries for the shell (chrome + selection routing) ----
  const std::string& title() const { return subsetTitle_; }
  int bookCount() const { return static_cast<int>(subset_.size()); }
  bool isEmpty() const { return viewItemCount() == 0; }
  // Non-const: GridHelper's accessors are not const-qualified.
  uint8_t currentPage() { return gridHelper.currentPage(); }
  uint8_t pageCount() { return gridHelper.pageCount(); }
  bool isBackTileSelected() { return isBackTileIndex(gridHelper.currentIndex()); }
  // Entry index of the selected tile, or -1 for the back tile / empty slot.
  int selectedIndex() { return bookForGridIndex(gridHelper.currentIndex()); }

 private:
  // ---- Grid / tile layout ----
  static constexpr int COLS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLS * ROWS;
  static constexpr int CELL_GAP_X = 5;
  static constexpr int CELL_GAP_Y = 0;
  static constexpr int kTilePadTop = 4;
  static constexpr int kTilePadBottom = 16;
  static constexpr int kCoverPercent = 60;
  static constexpr int kCoverBottomPadding = 8;
  static constexpr int kTitleAuthorGap = 1;
  static constexpr int kSelectionInsetX = 3;
  static constexpr int kSelectionLiftAbove = 4;
  static constexpr int kSelectionExtendBelowTileContent = 8;

  // The cover-slot rect for a grid cell, replaying the tile layout within `content`
  // (Grid -> cell -> tile Vstack). Cells are uniform, so this is every cover's envelope.
  Rect computeCoverSlot(const Rect& content) const;

  // Selection frame that hugs measured tile content (floats above the cell top down to
  // just past `contentBottom`). Uses the kSelection* constants above.
  static Rect computeSelectionFrame(const Rect& cell, int contentBottom);

  int viewItemCount() const;
  bool isBackTileIndex(int gridIndex) const;
  // Entry index into LIBRARY_INDEX for a grid slot, or -1 for back tile / OOB.
  int bookForGridIndex(int gridIndex) const;

  void renderTile(const Rect& cell, const BookView& book, bool selected);
  void renderBackTile(const Rect& cell, bool selected);

  void moveUp();
  void moveDown();
  void moveLeft();
  void jumpPageBack();
  void jumpPageForward();
  void endRapidJumpIfActive();

  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  GridHelper gridHelper = GridHelper(0, 0, 0, 0);

  // The active subset (entry indices into LIBRARY_INDEX in sorted order) + its display
  // name. Indices are valid only between sorts; setSubset rebuilds them under the cache
  // lock.
  std::vector<uint32_t> subset_;
  bool hasBackTile_ = false;
  std::string subsetTitle_;

  ButtonNavigator buttonNavigator;

  CoverPrefetcher prefetcher_{
    renderer,
    PER_PAGE,
    [this](int itemIndex) -> std::optional<CoverPrefetcher::CoverRef> {
      const int entryIdx = bookForGridIndex(itemIndex);  // -1 = back tile / OOB
      if (entryIdx < 0) return std::nullopt;
      const BookView b = LIBRARY_INDEX.getAt(entryIdx);
      return CoverPrefetcher::CoverRef{b.pathHash, b.format};
    }
  };

  // True during a continuous Up/Down hold: pages flicked past aren't decoded (the render
  // path shows placeholders); release loads the landed page for real.
  volatile bool rapidJumping_ = false;

  // Set by the nav helpers in lieu of requestUpdate (the view can't repaint itself);
  // handleInput() returns and clears it so the shell repaints.
  bool needsRepaint_ = false;
};
