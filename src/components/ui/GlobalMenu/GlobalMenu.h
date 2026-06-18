#pragma once

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/lib/MenuRegistry.h"
#include "components/themes/BaseTheme.h"
#include "components/ui/CascadingPopupMenu/CascadingPopupMenu.h"

class GlobalMenu {
  private:
    bool opened = false;
    GfxRenderer& renderer;
    MappedInputManager& mappedInput;

    uint8_t selectedIndex = 0;

    // The cascading popup for the selected entry's popupItems. Opened (as an
    // unselected preview) whenever the selected entry has popup items; entered
    // when the user presses Confirm. Navigation routes here while entered.
    CascadingPopupMenu popup_;

    void renderNavBody(Rect area);
    void renderNavItems(Rect area);
    // Draw a single nav slot: the selection indicator (when selected) and the icon.
    void renderSlot(const MenuRegistryEntry& entry, Rect slot, bool selected);

    std::function<std::vector<MenuRegistryEntry>()> getTopEntries;
    std::function<std::vector<MenuRegistryEntry>()> getBottomEntries;

    std::optional<MenuRegistryEntry> getSelectedEntry();

    // Open the popup (as a preview) for the selected entry, or close it when the
    // selection has no popup items. Call on open and on every selection change.
    void syncPopupToSelection();
    // The screen rect of the currently-selected nav slot within `nav`.
    Rect selectedSlotRect(Rect nav);
    // Close the whole menu (and its popup), resetting selection.
    void closeMenu();

  public:
    GlobalMenu(
      GfxRenderer& renderer,
      MappedInputManager& mappedInput,
      std::function<std::vector<MenuRegistryEntry>()> getTopEntries,
      std::function<std::vector<MenuRegistryEntry>()> getBottomEntries
    ) : renderer(renderer),
        mappedInput(mappedInput),
        getTopEntries(getTopEntries),
        getBottomEntries(getBottomEntries) {
      // The builder re-reads the selected entry on every (re)build, so state-
      // dependent glyphs (e.g. the active sort arrow) stay fresh. requestUpdate
      // is a no-op: loop() returns true to drive the single refresh.
      popup_.configure(
        [this]() -> std::vector<PopupMenuEntry> {
          auto entry = getSelectedEntry();
          if (entry.has_value()) return entry->popupItems;
          return {};
        },
        [] {});
    }

    bool isOpen() const { return opened; }

    // Handle input for the menu. Returns true if the state changed
    // this frame, so the caller can trigger exactly one e-ink refresh.
    bool loop();
    void render();
};
