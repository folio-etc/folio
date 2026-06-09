#pragma once

struct Rect;
class GfxRenderer;

// Horizontal progress bar that fills `box`: 1 px outer border, 1 px inset
// gap, fill rectangle inside, proportional to `percentage` (0..100).
//
// The bar's background is drawn in the opposite ink to whatever `textBlack`
// implies — so a black-on-white tile gets a white bar background with a
// black border and fill, and an inverted (selected) tile gets the opposite.
//
// Caller sizes the box; the widget does no theming and no centering. For
// a narrower-than-slot bar, wrap with a flex::Hstack to center.
class ProgressBar {
 public:
  static void render(GfxRenderer& renderer, const Rect& box, int percentage,
                     bool textBlack = true);
};
