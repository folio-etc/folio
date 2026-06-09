#include "ProgressBar.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "components/themes/BaseTheme.h"  // Rect

void ProgressBar::render(GfxRenderer& renderer, const Rect& box, int percentage,
                         bool textBlack) {
  if (box.width <= 0 || box.height <= 0) return;

  // Background: opposite ink so the border + fill read against any
  // selection wash the activity has painted underneath.
  renderer.fillRect(box.x, box.y, box.width, box.height, !textBlack);

  // 1-px border tracing the box. Single line width — drawRect's default.
  renderer.drawRect(box.x, box.y, box.width, box.height, textBlack);

  // Inner fill: inset 2 px from every edge (1 px border + 1 px gap), with
  // a hard minimum of 1 px tall so very-short slots still show something.
  const int pct = std::clamp(percentage, 0, 100);
  const int fillW = (box.width - 4) * pct / 100;
  const int fillH = std::max(1, box.height - 4);
  if (fillW > 0) {
    renderer.fillRect(box.x + 2, box.y + 2, fillW, fillH, textBlack);
  }
}
