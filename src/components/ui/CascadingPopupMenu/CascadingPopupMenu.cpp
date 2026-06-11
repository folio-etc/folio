#include "CascadingPopupMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"

void CascadingPopupMenu::configure(std::function<const char*(int)> topRowLabel,
                                   std::vector<SubmenuConfig> submenus) {
  topLabel_ = std::move(topRowLabel);
  subs_ = std::move(submenus);
  top_.setItemCount(static_cast<int>(subs_.size()), 0);
  subMenus_.clear();
  subMenus_.resize(subs_.size());
  for (size_t i = 0; i < subs_.size(); ++i) {
    if (subs_[i].itemCount > 0) {
      subMenus_[i].setItemCount(subs_[i].itemCount, 0);
    }
  }
}

void CascadingPopupMenu::open() {
  open_ = true;
  activeSub_ = -1;
  top_.setSelectedIndex(0);
}

void CascadingPopupMenu::close() {
  open_ = false;
  activeSub_ = -1;
}

int CascadingPopupMenu::subSelectedIndex() const {
  if (!inSubmenu()) return -1;
  return subMenus_[activeSub_].selectedIndex();
}

bool CascadingPopupMenu::topRowHasSubmenu(int i) const {
  return i >= 0 && i < static_cast<int>(subs_.size()) && subs_[i].itemCount > 0;
}

CascadingPopupMenu::Nav CascadingPopupMenu::moveUp() {
  if (!open_) return Nav::Ignored;
  PopupMenu& m = (activeSub_ >= 0) ? subMenus_[activeSub_] : top_;
  return m.moveUp() ? Nav::Moved : Nav::Ignored;
}

CascadingPopupMenu::Nav CascadingPopupMenu::moveDown() {
  if (!open_) return Nav::Ignored;
  PopupMenu& m = (activeSub_ >= 0) ? subMenus_[activeSub_] : top_;
  return m.moveDown() ? Nav::Moved : Nav::Ignored;
}

CascadingPopupMenu::Nav CascadingPopupMenu::moveLeft() {
  if (!open_) return Nav::Ignored;
  if (activeSub_ >= 0) {
    activeSub_ = -1;
    return Nav::BackToTop;
  }
  open_ = false;
  return Nav::ClosedPopup;
}

CascadingPopupMenu::Nav CascadingPopupMenu::moveRight() {
  if (!open_ || activeSub_ >= 0) return Nav::Ignored;
  const int i = top_.selectedIndex();
  if (topRowHasSubmenu(i)) {
    activeSub_ = i;
    const int init = subs_[i].initialSelection ? subs_[i].initialSelection() : 0;
    subMenus_[i].setSelectedIndex(init);
    return Nav::EnteredSubmenu;
  }
  return Nav::LeafActivated;
}

CascadingPopupMenu::Nav CascadingPopupMenu::activate() {
  if (!open_) return Nav::Ignored;
  if (activeSub_ >= 0) return Nav::SubItemActivated;
  const int i = top_.selectedIndex();
  if (topRowHasSubmenu(i)) {
    activeSub_ = i;
    const int init = subs_[i].initialSelection ? subs_[i].initialSelection() : 0;
    subMenus_[i].setSelectedIndex(init);
    return Nav::EnteredSubmenu;
  }
  return Nav::LeafActivated;
}

int CascadingPopupMenu::panelHeight(int itemCount) const {
  const auto& pm = GUI.getData()->popupMenu;
  return itemCount * pm.rowHeight + 2 * pm.borderThickness;
}

int CascadingPopupMenu::panelWidth(GfxRenderer& renderer, int itemCount,
                                   const std::function<const char*(int)>& rowLabel) const {
  const auto& pm = GUI.getData()->popupMenu;
  const int font = GUI.getFontForRole(pm.fontRole);
  int maxLabelW = 0;
  for (int i = 0; i < itemCount; ++i) {
    const char* s = rowLabel ? rowLabel(i) : nullptr;
    if (s == nullptr || s[0] == '\0') continue;
    const int w = renderer.getTextWidth(font, s, EpdFontFamily::BOLD);
    maxLabelW = std::max(maxLabelW, w);
  }
  // Layout per row: borderThickness | paddingX | label … glyph | paddingX | borderThickness
  // The glyph footprint matches PopupMenu's filled-triangle size (rowHeight/3,
  // floor-clamped at 6). Reserve it on every panel so adding an arrow at runtime
  // (e.g. the active sort field) doesn't visually reflow the panel.
  const int glyphRoom = std::max(6, pm.rowHeight / 3);
  return 2 * pm.borderThickness + 2 * pm.paddingX + maxLabelW + glyphRoom + pm.paddingX;
}

void CascadingPopupMenu::render(GfxRenderer& renderer, int leftX, int bottomLimit,
                                int rightLimit) const {
  if (!open_) return;
  const auto& pm = GUI.getData()->popupMenu;

  // ---- Top panel layout ----------------------------------------------------
  const int topCount = static_cast<int>(subs_.size());
  const int topH = panelHeight(topCount);
  const int topW = panelWidth(renderer, topCount, topLabel_);
  const int topX = leftX;
  // bottomLimit is the lowest pixel the cascade may touch (shadow included).
  // PopupMenu paints its shadow at rect.y + shadowOffsetY, so the panel rect's
  // bottom-edge has to sit at bottomLimit - shadowOffsetY.
  const int topY = bottomLimit - pm.shadowOffsetY - topH;

  // Top row glyphs are derived: chevron for rows that own a submenu, None
  // for leaves. The activity doesn't supply this — it's a cascade concern.
  auto topGlyph = [this](int i) -> PopupMenu::Glyph {
    return topRowHasSubmenu(i) ? PopupMenu::Glyph::ChevronRight : PopupMenu::Glyph::None;
  };

  // When a submenu is open, dim-highlight its owning top row so the user
  // can see which entry the submenu hangs off.
  const int topMuted = (activeSub_ >= 0) ? activeSub_ : -1;

  top_.render(renderer, Rect{topX, topY, topW, topH},
              /*showSelection=*/activeSub_ < 0, topLabel_, topGlyph, topMuted);

  if (activeSub_ < 0) return;

  // ---- Submenu panel layout ------------------------------------------------
  const SubmenuConfig& cfg = subs_[activeSub_];
  const int subH = panelHeight(cfg.itemCount);
  int subW = panelWidth(renderer, cfg.itemCount, cfg.rowLabel);
  const int subX = topX + topW + pm.panelGap;
  // Clamp width so the submenu's shadow doesn't escape rightLimit.
  if (subX + subW + pm.shadowOffsetX > rightLimit) {
    subW = rightLimit - pm.shadowOffsetX - subX;
  }
  // Baseline-align bottoms with the top panel (the prototype anchors both at
  // the same Y so the user sees the cascade open "from" the active row).
  const int subY = topY + topH - subH;

  subMenus_[activeSub_].render(renderer, Rect{subX, subY, subW, subH},
                               /*showSelection=*/true, cfg.rowLabel, cfg.rowGlyph);
}

void CascadingPopupMenu::renderFooterHints(GfxRenderer& renderer,
                                           const MappedInputManager& mappedInput) const {
  if (!open_) return;
  const auto labels = getFooterLabels(mappedInput);
  ButtonHints::render(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

MappedInputManager::Labels CascadingPopupMenu::getFooterLabels(const MappedInputManager& mappedInput) const {
  const bool inSub = inSubmenu();
  const bool confirmEnters = !inSub && topRowHasSubmenu(top_.selectedIndex());
  const char* back = inSub ? tr(STR_BACK) : tr(STR_CLOSE);
  const char* confirm = confirmEnters ? tr(STR_ENTER) : tr(STR_SELECT);
  return mappedInput.mapLabels(back, confirm, "", "");
}
