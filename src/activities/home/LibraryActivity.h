#pragma once
#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "LibraryIndex.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // for Rect

class LibraryActivity final : public Activity {
 public:
  // The library is the default view. Back opens a cascading popup anchored
  // above the footer (Sort / Files / Settings); Sort and Files expand to a
  // sub-panel to the right, Settings opens directly.
  enum class View { Library, Popup };
  enum class PopupLevel { Top, SortSub, FilesSub };

 private:
  // ---- Layout constants ---------------------------------------------------
  static constexpr int COLS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLS * ROWS;
  static constexpr int POPUP_TOP_ITEMS = 3;    // Sort / Files / Settings
  static constexpr int POPUP_SORT_ITEMS = 4;   // Recent / Title / Author / Progress
  static constexpr int POPUP_FILES_ITEMS = 2;  // Browse Files / File Transfer

  // ---- View state ---------------------------------------------------------
  View view = View::Library;
  PopupLevel popupLevel = PopupLevel::Top;

  // Library grid position
  int libraryPage = 0;          // 0-indexed
  int librarySelected = 0;      // 0..PER_PAGE-1 within current page

  // Popup row selection per level
  int popupTopSel = 0;
  int popupSortSel = 0;
  int popupFilesSel = 0;

  // True when the Confirm release that brought us into this activity should
  // not also trigger an open-book (typical when launched from a parent menu).
  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // No per-slot cover cache here — the renderer owns a path-keyed image
  // cache (see GfxRenderer::drawCachedBitmap). Library just calls
  // drawCachedBitmap for each visible thumb; the renderer hides the
  // SD-I/O / decode cost and LRU-evicts under its own budget.

  // ---- Navigation helpers -------------------------------------------------
  int currentRow() const { return librarySelected / COLS; }
  int currentCol() const { return librarySelected % COLS; }
  int totalPages() const { return LIBRARY_INDEX.totalPages(PER_PAGE); }

  void moveUp();
  void moveDown();
  void moveLeft();
  void moveRight();
  void doSelect();
  void togglePopup();
  void activatePopupItem();
  // Re-sort the index from current settings and reset selection to the top.
  void applySort();
  // Persist the active sort field/direction and re-sort.
  void setSort(uint8_t field, uint8_t direction);
  int popupItemsAt(PopupLevel level) const;

  // ---- Rendering helpers --------------------------------------------------
  // Body of the render: header + library shelf or menu + footer hints.
  // Split out from render() so the prewarming and clear/display bookends
  // stay tidy.
  void renderPasses();
  void renderHeader();
  void renderLibraryShelf();
  void renderPageRail();
  void renderPopup();
  void renderBookTile(int slotIndex, const LibraryBook& book, bool selected);
  void renderEmptyState();

  // Returns the rect actually occupied by a book tile's content (cover +
  // title + author + progress bar) inside its cell. Shorter than the cell
  // itself when the title fits on one line or there's no author / progress.
  // Used by the selection frame so it hugs the visible content the way the
  // prototype's CSS outline does.
  Rect tileContentRect(const LibraryBook& book, const Rect& cell) const;


  // Bounding rect of the (row, col) cell inside the shelf area. Pure geometry,
  // marked static so the renderer code keeps direct access to COLS/ROWS without
  // a `this` indirection.
  static Rect cellRect(int row, int col, int shelfX, int shelfY, int shelfW, int shelfH);

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void declareText(TextCollector& tc) override;
};
