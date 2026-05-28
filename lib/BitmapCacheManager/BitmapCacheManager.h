#include <cstddef>
#include <memory>
#include <string>
#include "../data-structures/ByteLRUCache/ByteLRUCache.h"

class BitmapCacheManager {
  public:
    struct CachedBitmap {
      std::unique_ptr<uint8_t[]> pixels;
      size_t pixelsBytes = 0;
      int width = 0;
      int height = 0;
      bool topDown = false;

      // Pre-scaled 1-bit pixel data at the most recently requested target
      // dimensions. Built lazily on first drawCachedBitmap call; reused on
      // subsequent paints when target size matches. 1 bit/pixel, MSB first,
      // row-major, stride = (scaledWidth + 7) / 8.
      std::unique_ptr<uint8_t[]> scaledPixels;
      size_t scaledPixelsBytes = 0;
      int scaledWidth = 0;
      int scaledHeight = 0;
    };

  private:
    ByteLRUCache<std::string, CachedBitmap> cache{ std::size_t{ 64 * 1024 } };

    void test() {
    }
};
