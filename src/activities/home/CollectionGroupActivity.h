#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Group list for one auto-group axis (series / author / genre). Computes the
// distinct group names and book counts on the fly from the library index;
// selecting a group filters the library to it.
class CollectionGroupActivity final : public Activity {
 public:
  // mode is a CrossPointSettings::LIBRARY_VIEW_KIND value: SERIES, AUTHOR or GENRE.
  explicit CollectionGroupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t mode)
      : Activity("CollectionGroup", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  uint8_t mode;
  std::vector<std::pair<std::string, int>> groups;  // name, count (sorted by name)

  void buildGroups();
  const char* headerTitle() const;
};
