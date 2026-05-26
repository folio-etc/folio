#include "PopupMenu.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.h"

void PopupMenu::setItemCount(int count, int initialSelection) {
  itemCount_ = count < 0 ? 0 : count;
  if (itemCount_ == 0) {
    selectedIndex_ = 0;
    return;
  }
  selectedIndex_ = std::clamp(initialSelection, 0, itemCount_ - 1);
}

void PopupMenu::setSelectedIndex(int i) {
  if (itemCount_ == 0) {
    selectedIndex_ = 0;
    return;
  }
  selectedIndex_ = std::clamp(i, 0, itemCount_ - 1);
}

bool PopupMenu::moveUp() {
  if (selectedIndex_ <= 0) return false;
  --selectedIndex_;
  return true;
}

bool PopupMenu::moveDown() {
  if (selectedIndex_ >= itemCount_ - 1) return false;
  ++selectedIndex_;
  return true;
}

void PopupMenu::render(GfxRenderer& renderer, Rect rect, bool showSelection,
                      const std::function<const char*(int)>& rowLabel,
                      const std::function<Glyph(int)>& rowGlyph, int mutedRow) const {
  if (itemCount_ <= 0) return;
  const auto& theme = GUI;
  const auto& pm = theme.getData()->popupMenu;

  // ---- Drop shadow ---------------------------------------------------------
  if (pm.shadowOffsetX != 0 || pm.shadowOffsetY != 0) {
    if (pm.cornerRadius > 0) {
      renderer.fillRoundedRect(rect.x + pm.shadowOffsetX, rect.y + pm.shadowOffsetY, rect.width, rect.height,
                               pm.cornerRadius, Color::Black);
    } else {
      renderer.fillRect(rect.x + pm.shadowOffsetX, rect.y + pm.shadowOffsetY, rect.width, rect.height, true);
    }
  }

  // ---- Panel fill (white) --------------------------------------------------
  if (pm.cornerRadius > 0) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, pm.cornerRadius, Color::White);
  } else {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  }

  // ---- Panel border --------------------------------------------------------
  if (pm.borderThickness > 0) {
    if (pm.cornerRadius > 0) {
      renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, pm.borderThickness, pm.cornerRadius, true);
    } else {
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height, pm.borderThickness, true);
    }
  }

  // ---- Rows ----------------------------------------------------------------
  const int labelFontId = theme.getFontForRole(pm.fontRole);
  const int labelLineHeight = renderer.getLineHeight(labelFontId);
  const int innerX = rect.x + pm.borderThickness;
  const int innerW = rect.width - 2 * pm.borderThickness;

  for (int i = 0; i < itemCount_; ++i) {
    const Rect rowRect{innerX, rect.y + pm.borderThickness + i * pm.rowHeight, innerW, pm.rowHeight};

    const bool isSelected = showSelection && (i == selectedIndex_);
    const bool isMuted = (i == mutedRow) && pm.subPanelMutedFill;

    // Background pass.
    if (isMuted && !isSelected) {
      renderer.fillRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, pm.selectionCornerRadius, Color::LightGray);
    }
    if (isSelected) {
      switch (pm.selectionStyle) {
        case PopupMenuSelectionStyle::SolidFill:
          if (pm.selectionCornerRadius > 0) {
            renderer.fillRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, pm.selectionCornerRadius,
                                     Color::Black);
          } else {
            renderer.fillRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, true);
          }
          break;
        case PopupMenuSelectionStyle::RoundedFill:
          renderer.fillRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height,
                                   pm.selectionCornerRadius > 0 ? pm.selectionCornerRadius : 4, Color::Black);
          break;
        case PopupMenuSelectionStyle::BorderOnly:
          renderer.drawRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, 1, true);
          break;
      }
    }

    const bool textBlack = !(isSelected && pm.selectionTextInverted);

    // Label — vertically centered within the row.
    const char* label = rowLabel ? rowLabel(i) : nullptr;
    if (label != nullptr && label[0] != '\0') {
      const int labelY = rowRect.y + (rowRect.height - labelLineHeight) / 2;
      renderer.drawText(labelFontId, rowRect.x + pm.paddingX, labelY, label, textBlack);
    }

    // Optional right-aligned glyph. Drawn as a filled triangle so it doesn't
    // depend on the font having ↑/↓/▶ in its glyph set.
    if (rowGlyph) {
      const Glyph glyph = rowGlyph(i);
      if (glyph != Glyph::None) {
        // Glyph metrics — sized off the row height so it scales with theme.
        // Body of the glyph hugs the trailing edge with a paddingX gutter.
        const int glyphSize = std::max(6, rowRect.height / 3);
        const int gx = rowRect.x + rowRect.width - pm.paddingX - glyphSize;
        const int gy = rowRect.y + (rowRect.height - glyphSize) / 2;
        switch (glyph) {
          case Glyph::ArrowUp: {
            const int xs[3] = {gx + glyphSize / 2, gx, gx + glyphSize};
            const int ys[3] = {gy, gy + glyphSize, gy + glyphSize};
            renderer.fillPolygon(xs, ys, 3, textBlack);
            break;
          }
          case Glyph::ArrowDown: {
            const int xs[3] = {gx, gx + glyphSize, gx + glyphSize / 2};
            const int ys[3] = {gy, gy, gy + glyphSize};
            renderer.fillPolygon(xs, ys, 3, textBlack);
            break;
          }
          case Glyph::ChevronRight: {
            // Slightly narrower than the up/down arrows to read as "expand"
            // rather than "direction".
            const int w = (glyphSize * 2) / 3;
            const int cx = rowRect.x + rowRect.width - pm.paddingX - w;
            const int xs[3] = {cx, cx + w, cx};
            const int ys[3] = {gy, gy + glyphSize / 2, gy + glyphSize};
            renderer.fillPolygon(xs, ys, 3, textBlack);
            break;
          }
          case Glyph::None:
            break;
        }
      }
    }
  }
}
