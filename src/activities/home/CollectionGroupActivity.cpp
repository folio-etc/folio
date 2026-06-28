#include "CollectionGroupActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "LibraryIndex.h"
#include "MappedInputManager.h"
#include "components/ui/List/List.h"
#include "components/ui/UIPage/UIPage.h"

namespace {
// "1 book" / "N books" — matches the prototype's right-aligned row-meta count.
std::string bookCountLabel(int n) {
  return std::to_string(n) + " " + (n == 1 ? tr(STR_BOOK_SINGULAR) : tr(STR_BOOK_PLURAL));
}
}  // namespace

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
  std::map<std::string, int> counts;  // ordered by name, dedupes
  const int count = LIBRARY_INDEX.getBookCount();
  for (int i = 0; i < count; ++i) {
    const BookView b = LIBRARY_INDEX.getAt(i);
    // Genre holds multiple newline-joined subjects: count the book once per
    // subject so it lists under every one of its genres.
    if (mode == CrossPointSettings::LIB_VIEW_GENRE) {
      forEachGenre(b.genre, [&counts](std::string_view g) { counts[std::string(g)]++; });
      continue;
    }
    std::string_view field;
    switch (mode) {
      case CrossPointSettings::LIB_VIEW_SERIES:
        field = b.series;
        break;
      case CrossPointSettings::LIB_VIEW_AUTHOR:
        field = b.author;
        break;
    }
    if (field.empty()) continue;  // skip books without this field
    counts[std::string(field)]++;
  }

  std::vector<ListItem> items;
  items.reserve(counts.size());
  for (auto& kv : counts) {
    ListItem item;
    item.title = kv.first;
    item.value = bookCountLabel(kv.second);
    // Selecting a group filters the library to it and returns to the Library.
    item.onSelect = [this, name = kv.first] {
      SETTINGS.libraryViewKind = mode;
      SETTINGS.libraryViewCollectionId = 0;
      strncpy(SETTINGS.libraryViewName, name.c_str(), sizeof(SETTINGS.libraryViewName) - 1);
      SETTINGS.libraryViewName[sizeof(SETTINGS.libraryViewName) - 1] = '\0';
      SETTINGS.saveToFile();
      // Default (not-cancelled) result tells the parent a group was chosen, so
      // the return propagates through CollectionsActivity back to the Library.
      finish();
    };
    items.push_back(std::move(item));
  }

  list.valueMetaStyle = true;  // smaller italic meta column (count)
  list.setItems(std::move(items));
}

void CollectionGroupActivity::onEnter() {
  Activity::onEnter();
  if (!LIBRARY_INDEX.isLoaded()) {
    LIBRARY_INDEX.loadFromFile();
  }
  buildGroups();
  requestUpdate();
}

void CollectionGroupActivity::onExit() { Activity::onExit(); }

void CollectionGroupActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Signal "no group chosen" so the parent collections list stays put.
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    list.triggerSelected();
    return;
  }

  if (list.size() > 0) {
    buttonNavigator.onNext([this] {
      list.down();
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      list.up();
      requestUpdate();
    });
  }
}

void CollectionGroupActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const Rect body = UIPage::render(renderer, headerTitle(), nullptr, labels);

  list.render(renderer, body);

  renderer.displayBuffer();
}
