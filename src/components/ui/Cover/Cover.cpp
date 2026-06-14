#include "Cover.h"

#include <BitmapCacheManager.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemeData.generated.h"

void Cover::render(GfxRenderer& renderer, const Rect& box, BitmapCacheManager& cache,
                   const char* thumbPath, bool inverted, int naturalWidth, int naturalHeight,
                   const Fallback* fallback) {
  const auto& lib = GUI.getData()->library;

  // Probe cached dimensions to drive aspect-fit framing. Always a read-only
  // peek — covers are decoded + pre-scaled by the prefetch worker, so the
  // render path never loads from SD; a miss falls through to the natural-size
  // fallback frame while the cover is in flight.
  int bmpW = 0;
  int bmpH = 0;
  const bool haveThumb = thumbPath != nullptr && thumbPath[0] != '\0' &&
                         renderer.peekCachedBitmapDimensions(cache, thumbPath, &bmpW, &bmpH) && bmpW > 0 &&
                         bmpH > 0;

  // Default frame: natural-size, horizontally centered, top-aligned.
  int frameX = box.x + (box.width - naturalWidth) / 2;
  int frameY = box.y;
  int frameW = naturalWidth;
  int frameH = naturalHeight;

  if (haveThumb) {
    // Aspect-fit into `box` (preserves the bitmap's intrinsic aspect, with
    // box dimensions as a hard ceiling).
    float scale = 1.0f;
    if (bmpW > box.width) scale = static_cast<float>(box.width) / static_cast<float>(bmpW);
    if (bmpH > box.height) scale = std::min(scale, static_cast<float>(box.height) / static_cast<float>(bmpH));
    const int drawnW = static_cast<int>(bmpW * scale);
    const int drawnH = static_cast<int>(bmpH * scale);
    // Center horizontally; vertically centered inside box.height. Tall
    // covers naturally bottom-align (drawnH hits box.height first).
    frameX = box.x + (box.width - drawnW) / 2;
    frameY = box.y + (box.height - drawnH) / 2;
    frameW = drawnW;
    frameH = drawnH;
  }

  // 1. Drop shadow — offset rect behind the frame, theme tokens drive
  // offsets and corner radius.
  if (lib.coverDropShadowOffsetX > 0 || lib.coverDropShadowOffsetY > 0) {
    renderer.fillRoundedRect(frameX + lib.coverDropShadowOffsetX,
                             frameY + lib.coverDropShadowOffsetY, frameW, frameH,
                             lib.coverBorderRadius,
                             inverted ? Color::White : Color::Black);
  }

  // 2. Cover bitmap or fallback substrate.
  bool drewBitmap = false;
  if (haveThumb) {
    // Pass box.width × box.height as the aspect-fit envelope so the
    // renderer arrives at the same drawnW × drawnH we computed above.
    // Feeding the truncated frame dims would introduce a fresh binding
    // constraint via int rounding (see the comment on the prior inlined
    // version in LibraryActivity::renderBookTile).
    drewBitmap = renderer.drawCachedBitmap<true>(cache, thumbPath, frameX, frameY, box.width,
                                                 box.height, lib.coverBorderRadius);
  }
  if (!drewBitmap) {
    // Blank cover substrate — always white regardless of inversion, so
    // the fallback reads as an unscanned cover rather than as part of
    // the selection wash.
    if (lib.coverBorderRadius > 0) {
      renderer.fillRoundedRect(frameX, frameY, frameW, frameH, lib.coverBorderRadius,
                               Color::White);
    } else {
      renderer.fillRect(frameX, frameY, frameW, frameH, false);
    }
    if (fallback != nullptr && fallback->text != nullptr && fallback->text[0] != '\0' &&
        fallback->fontId != 0) {
      // 12 px horizontal breathing room on either side, matching the
      // previous inlined fallback.
      const std::string trunc =
          renderer.truncatedText(fallback->fontId, fallback->text, frameW - 12, fallback->style);
      const int tw = renderer.getTextWidth(fallback->fontId, trunc.c_str(), fallback->style);
      const int lineH = renderer.getLineHeight(fallback->fontId);
      renderer.drawText(fallback->fontId, frameX + (frameW - tw) / 2,
                        frameY + (frameH - lineH) / 2, trunc.c_str(), true, fallback->style);
    }
  }

  // 3. Border (skipped when borderWidth == 0). Inverted-selection flips
  // the border ink to stay visible on the dark wash.
  if (lib.coverBorderWidth > 0) {
    renderer.drawRoundedRect(frameX, frameY, frameW, frameH, lib.coverBorderWidth,
                             lib.coverBorderRadius, !inverted);
  }
}
