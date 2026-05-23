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
  // Toggled by the Menu button. The library is the default view; Menu replaces
  // the content area with a three-item list (Browse Files, File Transfer,
  // Settings).
  enum class View { Library, Menu };

 private:
  // ---- Layout constants ---------------------------------------------------
  static constexpr int COLS = 3;
  static constexpr int ROWS = 3;
  static constexpr int PER_PAGE = COLS * ROWS;
  static constexpr int MENU_ITEMS = 3;

  // ---- View state ---------------------------------------------------------
  View view = View::Library;

  // Library grid position
  int libraryPage = 0;          // 0-indexed
  int librarySelected = 0;      // 0..PER_PAGE-1 within current page

  // Menu position
  int menuSelected = 0;         // 0..MENU_ITEMS-1

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
  void toggleMenu();
  void openMenuOption(int idx);

  // ---- Rendering helpers --------------------------------------------------
  // Body of the render: header + library shelf or menu + footer hints.
  // Split out from render() so the prewarming and clear/display bookends
  // stay tidy.
  void renderPasses();
  void renderHeader();
  void renderLibraryShelf();
  void renderPageRail();
  void renderMenuView();
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
