#pragma once

#include <EpdFontFamily.h>

struct Rect;
class BitmapCacheManager;
class GfxRenderer;

// Renders a thumbnail cover image into a rect with theme-driven styling.
//
// Pulls cover radius, border width, and drop-shadow offsets from
// GUI.getData()->library at call time, so theme changes propagate
// automatically.
//
// Behavior:
//   - If the thumbnail is cached, aspect-fit it inside `box`, centered.
//     Drop shadow + border hug the aspect-fit frame, not the full box.
//   - If the thumbnail is missing, draw a {naturalWidth × naturalHeight}
//     rounded rect centered in `box` and optionally print `fallback.text`
//     centered inside.
//
// Concurrency: the caller is responsible for synchronizing access to
// `cache` (e.g., holding the LibraryActivity cache mutex around the call).
// Stateless — no member state across calls.
class Cover {
 public:
  // Natural fallback frame for books without a cached thumbnail. The widget
  // centers this box inside its slot and (optionally) prints the title text
  // inside it. Tunes the visual character of the fallback badge — narrower
  // than the cover slot so it reads as a small placeholder rather than a
  // full-width tile.
  static constexpr int kFallbackWidth = 80;
  static constexpr int kFallbackHeight = 120;

  struct Fallback {
    const char* text = nullptr;
    int fontId = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  };

  static void render(GfxRenderer& renderer, const Rect& box, BitmapCacheManager& cache,
                     const char* thumbPath, bool useReadOnlyLookup, bool inverted,
                     int naturalWidth, int naturalHeight, const Fallback* fallback = nullptr);
};
