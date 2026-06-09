#include "CollectionGroupActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "CrossPointSettings.h"
#include "LibraryIndex.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "fontIds.h"

const char* CollectionGroupActivity::headerTitle() const {
  switch (mode) {
    case CrossPointSettings::LIB_VIEW_SERIES:
      return tr(STR_BY_SERIES);
    case CrossPointSettings::LIB_VIEW_AUTHOR:
      return tr(STR_BY_AUTHOR);
    case CrossPointSettings::LIB_VIEW_GENRE:
      return tr(STR_BY_GENRE);
  }
  return tr(STR_COLLECTIONS);
}

void CollectionGroupActivity::buildGroups() {
  groups.clear();

  std::map<std::string, int> counts;  // ordered by name, dedupes
  for (const auto& b : LIBRARY_INDEX.getBooks()) {
    const std::string* field = nullptr;
    switch (mode) {
      case CrossPointSettings::LIB_VIEW_SERIES:
        field = &b.series;
        break;
      case CrossPointSettings::LIB_VIEW_AUTHOR:
        field = &b.author;
        break;
      case CrossPointSettings::LIB_VIEW_GENRE:
        field = &b.genre;
        break;
    }
    if (field == nullptr || field->empty()) continue;  // skip books without this field
    counts[*field]++;
  }

  groups.reserve(counts.size());
  for (auto& kv : counts) {
    groups.emplace_back(kv.first, kv.second);
  }
}

void CollectionGroupActivity::onEnter() {
  Activity::onEnter();
  if (!LIBRARY_INDEX.isLoaded()) {
    LIBRARY_INDEX.loadFromFile();
  }
  buildGroups();
  selectedIndex = 0;
  requestUpdate();
}

void CollectionGroupActivity::onExit() { Activity::onExit(); }

void CollectionGroupActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(groups.size())) {
      SETTINGS.libraryViewKind = mode;
      SETTINGS.libraryViewCollectionId = 0;
      strncpy(SETTINGS.libraryViewName, groups[selectedIndex].first.c_str(), sizeof(SETTINGS.libraryViewName) - 1);
      SETTINGS.libraryViewName[sizeof(SETTINGS.libraryViewName) - 1] = '\0';
      SETTINGS.saveToFile();
      activityManager.goHome();
    }
    return;
  }

  const int itemCount = static_cast<int>(groups.size());
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void CollectionGroupActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& td = *GUI.getData();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, td.layout.topPadding, pageWidth, td.header.height}, headerTitle());

  const int contentTop = td.layout.topPadding + td.header.height + td.layout.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - td.buttonHints.height - td.layout.verticalSpacing * 2;

  if (groups.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LIBRARY_NO_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(groups.size()), selectedIndex,
        [this](int index) { return groups[index].first; }, nullptr, nullptr,
        [this](int index) { return std::to_string(groups[index].second); });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  ButtonHints::render(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
