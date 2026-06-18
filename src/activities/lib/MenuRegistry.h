#pragma once

#include <Bitmap1Bit.h>
#include <functional>
#include <optional>
#include <string>
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"

struct MenuRegistryEntry {
  using Icon = Bitmap1Bit;
  Icon icon;
  std::string name;
  std::optional<std::function<void()>> onPress;
  std::vector<PopupMenuEntry> popupItems;
};
