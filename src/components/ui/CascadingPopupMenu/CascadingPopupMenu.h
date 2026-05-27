#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include "components/themes/BaseTheme.h"  // for Rect
#include "components/ui/PopupMenu/PopupMenu.h"

// Two-level cascading menu: a top panel of rows, where each row is either a
// "leaf" (Confirm fires an action immediately, e.g. Settings) or owns a
// submenu panel that slides out to the right (e.g. Sort, Files).
//
// The cascade owns:
//   - panel chrome and layout (height/width auto-sized from theme metrics +
//     longest label per panel),
//   - the muted-row indicator on the top panel when a submenu is open,
//   - cross-level navigation (Left exits a submenu or closes the popup;
//     Right enters a submenu or activates a leaf),
//   - per-submenu initial-selection sync via the configured callback.
//
// The activity owns:
//   - what each row *means* (labels via callback, action dispatch on
//     LeafActivated / SubItemActivated).
class CascadingPopupMenu {
 public:
  // Outcome of a navigation or activation input. The activity uses this to
  // know whether to redraw, dispatch a leaf action, or close the popup view.
  enum class Nav : uint8_t {
    Ignored,           // input did nothing (e.g. Up at row 0)
    Moved,             // selection changed within the active panel
    ClosedPopup,       // Left from the top panel closed the popup
    EnteredSubmenu,    // top row had a submenu; cascade entered it
    BackToTop,         // Left from a submenu returned to the top panel
    LeafActivated,     // Confirm/Right on a leaf top row — activity dispatches
    SubItemActivated,  // Confirm on a submenu row — activity dispatches
  };

  // Per-top-row configuration. itemCount=0 marks a leaf row (no chevron,
  // no submenu; Confirm/Right fires LeafActivated).
  struct SubmenuConfig {
    int itemCount = 0;
    std::function<const char*(int)> rowLabel;
    std::function<PopupMenu::Glyph(int)> rowGlyph;  // optional
    // Invoked each time this submenu becomes visible; returns the row to
    // pre-select (e.g. the currently-active sort field). Defaults to 0 when
    // not provided.
    std::function<int()> initialSelection;  // optional
  };

  CascadingPopupMenu() = default;

  // Configure the cascade. `topRowLabel(i)` resolves the i-th top row label
  // (typically through tr()). `submenus[i]` is the submenu config for the
  // i-th top row; pass a default-constructed SubmenuConfig for leaf rows.
  // The top item count is taken from `submenus.size()`.
  void configure(std::function<const char*(int)> topRowLabel, std::vector<SubmenuConfig> submenus);

  // Open lands on the top panel at row 0. Close clears all state.
  void open();
  void close();
  bool isOpen() const { return open_; }
  bool inSubmenu() const { return open_ && activeSub_ >= 0; }

  // Selections — only meaningful while open. subSelectedIndex returns -1
  // when the cascade is not currently inside a submenu.
  int topSelectedIndex() const { return top_.selectedIndex(); }
  int subSelectedIndex() const;

  // True when the i-th top row has a submenu (used by the activity to set
  // footer-hint labels, e.g. "Enter" vs blank for the right-button hint).
  bool topRowHasSubmenu(int i) const;

  // Input. The activity routes button presses to these and reacts to the
  // returned Nav value.
  Nav moveUp();
  Nav moveDown();
  Nav moveLeft();
  Nav moveRight();
  Nav activate();  // Confirm

  // Render the cascade. The top panel is anchored with its left edge at
  // `leftX` and its bottom-most rendered pixel (including drop shadow) at
  // or above `bottomLimit`. The submenu panel, when open, sits to the right
  // of the top panel with the theme's panelGap and is width-clamped so its
  // right-most rendered pixel (including drop shadow) stays at or before
  // `rightLimit`. The cascade computes its own panel widths and heights —
  // callers don't pre-measure.
  void render(GfxRenderer& renderer, int leftX, int bottomLimit, int rightLimit) const;

  // Paint the footer button hints for the open popup. The cascade owns its
  // own hint scheme so every activity that hosts a CascadingPopupMenu gets
  // the same affordances:
  //   - back-button slot: "Close" at the top panel, "Back" inside a submenu
  //   - confirm-button slot: "Enter" when the selected top row owns a submenu,
  //     "Select" otherwise (leaves and submenu rows alike)
  //   - left / right slots: blank (navigation is handled by up/down + confirm)
  // The activity routes labels through mappedInput so physical-button
  // remapping still applies.
  void renderFooterHints(GfxRenderer& renderer, const MappedInputManager& mappedInput) const;

 private:
  bool open_ = false;
  int activeSub_ = -1;  // -1 = focus on top panel; >=0 = inside submenu activeSub_
  std::function<const char*(int)> topLabel_;
  std::vector<SubmenuConfig> subs_;
  PopupMenu top_;
  // One PopupMenu per top row (leaves hold a default-constructed PopupMenu
  // that is never rendered). Indexing by top row keeps the cascade-to-
  // submenu mapping trivial.
  std::vector<PopupMenu> subMenus_;

  int panelHeight(int itemCount) const;
  int panelWidth(GfxRenderer& renderer, int itemCount,
                 const std::function<const char*(int)>& rowLabel) const;
};
