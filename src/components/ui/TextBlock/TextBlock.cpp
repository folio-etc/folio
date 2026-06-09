#include "TextBlock.h"

#include <GfxRenderer.h>

#include <string>

#include "components/themes/BaseTheme.h"  // Rect

void TextBlock::render(GfxRenderer& renderer, const Rect& box, const Line* lines,
                       std::size_t lineCount, bool textBlack) {
  if (lines == nullptr || lineCount == 0) return;

  // Pass 1: compute total block height (sum of line heights + gaps).
  // Skip lines with null/empty text and their gaps so the block recenters
  // when an optional line (e.g. author) is missing.
  int totalHeight = 0;
  for (std::size_t i = 0; i < lineCount; ++i) {
    const Line& l = lines[i];
    if (l.text == nullptr || l.text[0] == '\0' || l.fontId == 0) continue;
    totalHeight += l.gapBefore;
    totalHeight += renderer.getLineHeight(l.fontId);
  }
  if (totalHeight <= 0) return;

  int y = box.y + (box.height - totalHeight) / 2;

  // Pass 2: draw each visible line centered horizontally.
  for (std::size_t i = 0; i < lineCount; ++i) {
    const Line& l = lines[i];
    if (l.text == nullptr || l.text[0] == '\0' || l.fontId == 0) continue;
    y += l.gapBefore;
    const int lineH = renderer.getLineHeight(l.fontId);
    if (l.truncate) {
      const std::string truncated =
          renderer.truncatedText(l.fontId, l.text, box.width, l.style);
      const int w = renderer.getTextWidth(l.fontId, truncated.c_str(), l.style);
      renderer.drawText(l.fontId, box.x + (box.width - w) / 2, y, truncated.c_str(),
                        textBlack, l.style);
    } else {
      const int w = renderer.getTextWidth(l.fontId, l.text, l.style);
      renderer.drawText(l.fontId, box.x + (box.width - w) / 2, y, l.text, textBlack,
                        l.style);
    }
    y += lineH;
  }
}
