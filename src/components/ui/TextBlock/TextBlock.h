#pragma once

#include <EpdFontFamily.h>

#include <cstddef>

struct Rect;
class GfxRenderer;

// Vertically-centered multi-line text block.
//
// Caller supplies pre-wrapped lines (use GfxRenderer::wrappedText to do the
// wrapping). Each line carries its own font + style + optional leading gap,
// so a single block can mix e.g. a bold title with an italic author line.
// Lines are horizontally centered inside `box`; the stack as a whole is
// vertically centered. Lines whose `text` is null or empty are skipped
// entirely (the leading gap is also dropped).
//
// Truncation: if `truncate` is true, the line is shortened with
// GfxRenderer::truncatedText to fit `box.width`; otherwise it's drawn as-is
// and may overflow.
//
// Stateless and self-contained — no theme tokens, no state across calls.
class TextBlock {
 public:
  struct Line {
    const char* text = nullptr;
    int fontId = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    int gapBefore = 0;     // extra px before this line (0 = no gap)
    bool truncate = true;  // shorten to box.width via truncatedText
  };

  static void render(GfxRenderer& renderer, const Rect& box, const Line* lines,
                     std::size_t lineCount, bool textBlack = true);
};
