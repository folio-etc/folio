#include "components/themes/BaseTheme.h"
#include "MappedInputManager.h"
#include "util/Flex.h"

class UIPage {
  public:
    static Rect render(
        GfxRenderer& renderer,
        const char* title,
        const char* subtitle,
        const MappedInputManager::Labels btnLabels,
        const flex::Padding bodyPadding = flex::Padding{}
    );
};
