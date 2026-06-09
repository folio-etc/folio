#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Full-screen Collections list: auto groups (By Series / By Author / By Genre)
// followed by the user's manual collections, with a "New Collection…" action.
// Selecting an auto group opens its group list; selecting a manual collection
// filters the library to it; "New Collection…" names a new (empty) collection.
class CollectionsActivity final : public Activity {
 public:
  explicit CollectionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Collections", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class RowKind { GroupSeries, GroupAuthor, GroupGenre, NewCollection, ManualCollection };

  struct Row {
    RowKind kind;
    uint32_t collectionId;  // ManualCollection only
    std::string label;
    int count;    // ManualCollection member count
    bool active;  // ManualCollection: matches the active library view
  };

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<Row> rows;

  void buildRows();
  void handleSelection();
};
