#pragma once

#include <InflateReader.h>

#include <vector>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  // Theoretical maximum: 8 FontRoles × 4 styles (R/B/I/BI). Each slot itself
  // is ~20 bytes; the buffer/glyphs allocations are sized per-prewarm and
  // amortized via the (fontData, textHash) idempotent short-circuit below.
  static constexpr uint8_t MAX_PAGE_SLOTS = 32;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group).
  void clearCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
    // Allocated size of `buffer` in bytes, and the append point for incremental
    // prewarm growth (prewarmAppend grows the slot in place rather than
    // rebuilding, so glyphs decompressed for earlier pages stay resident).
    uint32_t bufferBytes = 0;
    // FNV-1a of the prewarm utf8Text. Lets prewarmCache short-circuit when
    // the same (fontData, text) is requested again — UI activities prewarm
    // on every render, and stable scenes (page navigation in a fixed grid,
    // popup open/close) keep the same text in most slots.
    uint32_t textHash = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Decompressed-group LRU (replaces the former single "hot group" slot).
  // Each entry holds one byte-aligned decompressed group; individual glyphs are
  // compacted on demand into hotGlyphBuf. Keyed by (font, groupIndex), bounded
  // by GROUP_CACHE_BUDGET_BYTES and MAX_GROUP_SLOTS, LRU-evicted via lruClock_.
  // A UI screen only touches the group-0 of each active (size, style); keeping
  // them all resident eliminates the re-inflate thrash that occurred when
  // consecutive drawText calls alternated fonts against the old single slot.
  static constexpr uint32_t GROUP_CACHE_BUDGET_BYTES = 48 * 1024;
  static constexpr uint8_t MAX_GROUP_SLOTS = 16;
  struct GroupCacheEntry {
    const EpdFontData* font = nullptr;
    uint16_t groupIndex = UINT16_MAX;
    std::vector<uint8_t> data;
    uint32_t lastUsed = 0;
  };
  std::vector<GroupCacheEntry> groupCache_;
  uint32_t groupCacheBytes_ = 0;
  uint32_t lruClock_ = 0;

  // Scratch buffer for compacting a single glyph out of a resident group.
  // Valid until the next getBitmap() call.
  std::vector<uint8_t> hotGlyphBuf;

  // Incrementally grow an existing page slot: decompress only the groups of
  // `neededGlyphs` not already resident, appending them to the slot's buffer.
  // `neededGlyphs` is filtered in place. Returns the number of glyphs that
  // couldn't be loaded (0 on full success).
  int prewarmAppend(PageSlot& slot, const EpdFontData* fontData, uint32_t* neededGlyphs, uint16_t neededCount,
                    uint32_t newTextHash);

  void freePageBuffer();
  void freeGroupCache();
  // Look up (font, groupIndex) in the LRU; decompress and insert on miss.
  // Returns the resident byte-aligned group, or nullptr on allocation failure.
  const std::vector<uint8_t>* getDecompressedGroup(const EpdFontData* fontData, uint16_t groupIndex);
  // Evict least-recently-used groups until a new group of neededBytes fits
  // within the byte budget and slot cap.
  void evictGroupsToFit(uint32_t neededBytes);
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
