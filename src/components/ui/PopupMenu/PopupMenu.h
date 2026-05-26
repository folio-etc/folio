#pragma once

#include <cstdint>
#include <functional>

#include "components/themes/BaseTheme.h"  // for Rect

class GfxRenderer;

// Bordered popup-menu panel with offset shadow, e.g. the Library's cascading
// Sort / Files / Settings menu. Owns its row selection — the activity drives
// navigation with moveUp/moveDown and asks for the active row via
// selectedIndex(). Multiple instances compose into a cascading menu (one per
// panel), each with independent state.
//
// Visual chrome (border, radius, fonts, selection style) comes from the
// active theme's popupMenu group at render time, so a theme switch redraws
// the panel in the new style without resetting selection state.
class PopupMenu {
 public:
  // Right-side glyph for popup-menu rows. Drawn as filled polygons so they
  // don't depend on font glyph coverage (the UI .cpfonts don't ship the
  // U+2191 / U+2193 / U+25B6 codepoints).
  enum class Glyph : uint8_t {
    None,
    ArrowUp,       // active sort field, ascending
    ArrowDown,     // active sort field, descending
    ChevronRight,  // row expands into a submenu
  };

  PopupMenu() = default;

  // Configure the number of selectable rows. Clamps the current selection
  // into the new range; pass `initialSelection` to override.
  void setItemCount(int count, int initialSelection = 0);
  int itemCount() const { return itemCount_; }

  int selectedIndex() const { return selectedIndex_; }
  void setSelectedIndex(int i);

  // Returns true when the selection actually moved — callers gate
  // requestUpdate() on the result so a bounded press at the edge of the
  // menu doesn't trigger a redraw.
  bool moveUp();
  bool moveDown();

  // Render at the panel footprint `rect` (includes the border but NOT the
  // shadow — the shadow extends past `rect` by the theme's popupMenu offsets,
  // so callers must reserve that space themselves).
  //
  // `showSelection`=false suppresses the row highlight without forgetting
  // the stored selection — cascading menus use this on the parent panel
  // while a submenu is open, so popping back restores the user's place.
  //
  // `mutedRow`=-1 disables; otherwise that row gets a dithered light-gray
  // wash (the parent-of-open-submenu hint). Gated by popupMenu.subPanelMutedFill.
  //
  // Labels and glyphs come via callbacks so callers don't need to mirror
  // dynamic state (e.g. the active sort field's arrow direction).
  void render(GfxRenderer& renderer, Rect rect, bool showSelection,
              const std::function<const char*(int index)>& rowLabel,
              const std::function<Glyph(int index)>& rowGlyph = nullptr,
              int mutedRow = -1) const;

 private:
  int itemCount_ = 0;
  int selectedIndex_ = 0;
};
