#include "CascadingPopupMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"

bool CascadingPopupMenu::isBranch(const PopupMenuEntry& e) { return e.children.has_value() && !e.children->empty(); }

void CascadingPopupMenu::configure(std::function<std::vector<PopupMenuEntry>()> builder,
                                   std::function<void()> requestUpdate) {
  builder_ = std::move(builder);
  requestUpdate_ = std::move(requestUpdate);
}

void CascadingPopupMenu::notifyUpdate() {
  if (requestUpdate_) requestUpdate_();
}

void CascadingPopupMenu::pushLevel(const std::vector<PopupMenuEntry>& entries, std::optional<uint8_t> selection) {
  Level level;
  level.entries = &entries;
  level.menu.setItemCount(static_cast<int>(entries.size()), selection);
  stack_.push_back(std::move(level));
}

void CascadingPopupMenu::open(std::optional<uint8_t> initialSelection) {
  open_ = true;
  stack_.clear();
  entries_ = builder_ ? builder_() : std::vector<PopupMenuEntry>{};
  pushLevel(entries_, initialSelection);
  notifyUpdate();
}

void CascadingPopupMenu::close() {
  open_ = false;
  stack_.clear();
}

const PopupMenuEntry* CascadingPopupMenu::focusedEntry() const {
  if (!open_ || stack_.empty()) return nullptr;
  const Level& level = stack_.back();
  const std::optional<uint8_t> sel = level.menu.selectedIndex();
  if (!sel.has_value() || *sel >= level.entries->size()) return nullptr;
  return &(*level.entries)[*sel];
}

void CascadingPopupMenu::rebuildPreservingPath() {
  // Snapshot the descended path before the tree is replaced.
  std::vector<std::optional<uint8_t>> path;
  path.reserve(stack_.size());
  for (const Level& level : stack_) path.push_back(level.menu.selectedIndex());

  entries_ = builder_ ? builder_() : std::vector<PopupMenuEntry>{};
  stack_.clear();
  if (path.empty()) {
    pushLevel(entries_, std::nullopt);
    return;
  }

  // Root, then re-descend while each saved row still names a branch.
  pushLevel(entries_, path[0]);
  for (size_t d = 1; d < path.size(); ++d) {
    const PopupMenuEntry* entry = focusedEntry();
    if (entry == nullptr || !isBranch(*entry)) break;
    pushLevel(*entry->children, path[d]);
  }
}

void CascadingPopupMenu::moveUp() {
  if (!open_ || stack_.empty()) return;
  if (stack_.back().menu.moveUp()) notifyUpdate();
}

void CascadingPopupMenu::moveDown() {
  if (!open_ || stack_.empty()) return;
  if (stack_.back().menu.moveDown()) notifyUpdate();
}

void CascadingPopupMenu::back() {
  if (!open_) return;
  if (stack_.size() > 1) {
    stack_.pop_back();
  } else {
    close();
  }
  notifyUpdate();
}

void CascadingPopupMenu::activate() {
  if (!open_ || stack_.empty()) return;

  // Activating a panel that has no row selected lands on the first row instead
  // of acting — mirrors how the first Up/Down press behaves from "no selection".
  PopupMenu& focused = stack_.back().menu;
  if (!focused.selectedIndex().has_value()) {
    if (focused.moveDown()) notifyUpdate();
    return;
  }

  const PopupMenuEntry* entry = focusedEntry();
  if (entry == nullptr) return;

  if (isBranch(*entry)) {
    // Entering a submenu lands on its pre-selected row, or row 0 by default.
    pushLevel(*entry->children, entry->initialSelectedChild.value_or(uint8_t{0}));
    notifyUpdate();
    return;
  }

  if (entry->onSelected.has_value()) {
    const bool shouldClose = (*entry->onSelected)();
    if (shouldClose) {
      close();
    } else {
      // The leaf mutated state (e.g. the sort field); rebuild so the tree's
      // glyphs reflect it, keeping the user on the same row.
      rebuildPreservingPath();
    }
    notifyUpdate();
  }
}

int CascadingPopupMenu::panelHeight(int itemCount) {
  const auto& pm = GUI.getData()->popupMenu;
  return itemCount * pm.rowHeight + 2 * pm.borderThickness;
}

int CascadingPopupMenu::panelWidth(GfxRenderer& renderer, const std::vector<PopupMenuEntry>& entries) {
  const auto& pm = GUI.getData()->popupMenu;
  const int font = GUI.getFontForRole(pm.fontRole);
  int maxLabelW = 0;
  bool anyGlyph = false;
  for (const PopupMenuEntry& e : entries) {
    // A row paints a glyph when it's a branch (chevron) or carries one.
    if (isBranch(e) || e.glyph.has_value()) anyGlyph = true;
    if (e.label == nullptr || e.label[0] == '\0') continue;
    // Measure in the same weight PopupMenu renders the label (REGULAR);
    // measuring BOLD over-reserves and leaves slack on the trailing edge.
    maxLabelW = std::max(maxLabelW, renderer.getTextWidth(font, e.label));
  }
  // Layout per row: borderThickness | paddingX | label [… glyph | paddingX] | borderThickness.
  // Only reserve the glyph gutter when some row in this panel actually has one,
  // so glyphless panels (e.g. Book / Files) don't carry dead trailing space.
  // The tree is rebuilt on open, so whether a panel has glyphs is stable for its
  // lifetime — no runtime reflow to guard against. Matches PopupMenu's sizing.
  const int glyphReserve = anyGlyph ? (std::max(6, pm.rowHeight / 3) + pm.paddingX) : 0;
  return 2 * pm.borderThickness + 2 * pm.paddingX + maxLabelW + glyphReserve;
}

void CascadingPopupMenu::render(GfxRenderer& renderer, int leftX, int bottomLimit, int rightLimit) const {
  if (!open_ || stack_.empty()) return;
  const auto& pm = GUI.getData()->popupMenu;
  const int panelBottom = bottomLimit - pm.shadowOffsetY;
  const int screenH = renderer.getScreenHeight();
  const size_t focused = stack_.size() - 1;

  // The focused branch's children render as a preview panel (no highlight) when
  // we haven't entered them yet.
  PopupMenu previewMenu;
  const std::vector<PopupMenuEntry>* previewEntries = nullptr;
  if (const PopupMenuEntry* entry = focusedEntry(); entry != nullptr && isBranch(*entry)) {
    previewEntries = &(*entry->children);
    previewMenu.setItemCount(static_cast<int>(previewEntries->size()), std::nullopt);
  }
  const int panelCount = static_cast<int>(stack_.size()) + (previewEntries != nullptr ? 1 : 0);

  // Walk the panel chain left-to-right. The root anchors just above the footer;
  // every subsequent panel cascades from its parent's selected row.
  int x = leftX;
  int prevTop = 0;                 // top edge of the panel drawn last iteration
  std::optional<uint8_t> prevSel;  // its selected row — the one a child hangs off
  for (int p = 0; p < panelCount; ++p) {
    const bool isPreview = (p == static_cast<int>(stack_.size()));
    const std::vector<PopupMenuEntry>& entries = isPreview ? *previewEntries : *stack_[p].entries;
    const PopupMenu& menu = isPreview ? previewMenu : stack_[p].menu;
    const bool isFocused = (!isPreview && static_cast<size_t>(p) == focused);

    const int count = static_cast<int>(entries.size());
    if (count == 0) break;

    int w = panelWidth(renderer, entries);
    if (x + w + pm.shadowOffsetX > rightLimit) w = rightLimit - pm.shadowOffsetX - x;
    if (w < 2 * pm.borderThickness + pm.paddingX) break;
    const int h = panelHeight(count);

    // Vertical placement: root anchors to the footer baseline; a child aligns
    // its top with the parent's selected-row top when the whole panel still
    // fits on-screen below that row, otherwise aligns its bottom with the row.
    int y;
    if (p == 0 || !prevSel.has_value()) {
      y = panelBottom - h;
    } else {
      const int rowTop = prevTop + pm.borderThickness + static_cast<int>(*prevSel) * pm.rowHeight;
      y = (rowTop + h + pm.shadowOffsetY <= screenH) ? rowTop : (rowTop + pm.rowHeight - h);
      if (y < 0) y = 0;
    }

    // Ancestors get a muted parent-row wash and no highlight; the focused panel
    // shows its selection; the preview shows none.
    int mutedRow = -1;
    if (!isFocused && !isPreview && menu.selectedIndex().has_value()) {
      mutedRow = static_cast<int>(*menu.selectedIndex());
    }

    const std::function<const char*(int)> labelFn = [&entries](int i) -> const char* { return entries[i].label; };
    const std::function<PopupMenu::Glyph(int)> glyphFn = [&entries](int i) -> PopupMenu::Glyph {
      const PopupMenuEntry& e = entries[i];
      if (isBranch(e)) return PopupMenu::Glyph::ChevronRight;
      return e.glyph.value_or(PopupMenu::Glyph::None);
    };
    menu.render(renderer, Rect{x, y, w, h}, /*showSelection=*/isFocused, labelFn, glyphFn, mutedRow);

    prevTop = y;
    prevSel = menu.selectedIndex();
    x += w + pm.panelGap;
  }
}

void CascadingPopupMenu::renderFooterHints(GfxRenderer& renderer, const MappedInputManager& mappedInput) const {
  if (!open_) return;
  const auto labels = getFooterLabels(mappedInput);
  ButtonHints::render(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

MappedInputManager::Labels CascadingPopupMenu::getFooterLabels(const MappedInputManager& mappedInput) const {
  const bool inSubmenu = stack_.size() > 1;
  const PopupMenuEntry* entry = focusedEntry();
  const bool confirmEnters = entry != nullptr && isBranch(*entry);
  const char* back = inSubmenu ? tr(STR_BACK) : tr(STR_CLOSE);
  const char* confirm = confirmEnters ? tr(STR_ENTER) : tr(STR_SELECT);
  return mappedInput.mapLabels(back, confirm, "", "");
}
