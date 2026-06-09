#include "CollectionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CollectionStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/home/CollectionGroupActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/ui/ButtonHints/ButtonHints.h"
#include "fontIds.h"

void CollectionsActivity::buildRows() {
  rows.clear();
  rows.push_back({RowKind::GroupSeries, 0, tr(STR_BY_SERIES), 0, false});
  rows.push_back({RowKind::GroupAuthor, 0, tr(STR_BY_AUTHOR), 0, false});
  rows.push_back({RowKind::GroupGenre, 0, tr(STR_BY_GENRE), 0, false});
  rows.push_back({RowKind::NewCollection, 0, tr(STR_NEW_COLLECTION), 0, false});

  const bool collectionViewActive = SETTINGS.libraryViewKind == CrossPointSettings::LIB_VIEW_COLLECTION;
  for (const auto& c : COLLECTION_STORE.getCollections()) {
    const bool active = collectionViewActive && SETTINGS.libraryViewCollectionId == c.id;
    rows.push_back({RowKind::ManualCollection, c.id, c.name, c.memberCount(), active});
  }
}

void CollectionsActivity::onEnter() {
  Activity::onEnter();
  COLLECTION_STORE.loadFromFile();
  buildRows();
  selectedIndex = 0;
  requestUpdate();
}

void CollectionsActivity::onExit() { Activity::onExit(); }

void CollectionsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = static_cast<int>(rows.size());
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

void CollectionsActivity::handleSelection() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(rows.size())) return;
  const Row& row = rows[selectedIndex];

  switch (row.kind) {
    case RowKind::GroupSeries:
      startActivityForResult(
          std::make_unique<CollectionGroupActivity>(renderer, mappedInput, CrossPointSettings::LIB_VIEW_SERIES),
          [](const ActivityResult&) {});
      return;
    case RowKind::GroupAuthor:
      startActivityForResult(
          std::make_unique<CollectionGroupActivity>(renderer, mappedInput, CrossPointSettings::LIB_VIEW_AUTHOR),
          [](const ActivityResult&) {});
      return;
    case RowKind::GroupGenre:
      startActivityForResult(
          std::make_unique<CollectionGroupActivity>(renderer, mappedInput, CrossPointSettings::LIB_VIEW_GENRE),
          [](const ActivityResult&) {});
      return;
    case RowKind::NewCollection:
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NAME_COLLECTION),
                                                                     std::string(), 48, InputType::Text),
                             [this](const ActivityResult& res) {
                               if (!res.isCancelled) {
                                 const auto& kb = std::get<KeyboardResult>(res.data);
                                 COLLECTION_STORE.createCollection(kb.text);
                               }
                               buildRows();
                             });
      return;
    case RowKind::ManualCollection:
      SETTINGS.libraryViewKind = CrossPointSettings::LIB_VIEW_COLLECTION;
      SETTINGS.libraryViewCollectionId = row.collectionId;
      SETTINGS.libraryViewName[0] = '\0';
      SETTINGS.saveToFile();
      activityManager.goHome();
      return;
  }
}

void CollectionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& td = *GUI.getData();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, td.layout.topPadding, pageWidth, td.header.height}, tr(STR_COLLECTIONS));

  const int contentTop = td.layout.topPadding + td.header.height + td.layout.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - td.buttonHints.height - td.layout.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(rows.size()), selectedIndex,
      [this](int index) {
        const Row& r = rows[index];
        // Mark the active manual collection with a leading middle dot.
        if (r.kind == RowKind::ManualCollection && r.active) {
          return std::string("\xC2\xB7 ") + r.label;  // U+00B7 (·)
        }
        return r.label;
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        const Row& r = rows[index];
        switch (r.kind) {
          case RowKind::GroupSeries:
          case RowKind::GroupAuthor:
          case RowKind::GroupGenre:
            return std::string("\xC2\xBB");  // U+00BB (»)
          case RowKind::ManualCollection:
            return std::to_string(r.count);
          case RowKind::NewCollection:
          default:
            return std::string("");
        }
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  ButtonHints::render(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
