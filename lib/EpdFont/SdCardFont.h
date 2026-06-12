#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"

// Thread-safe SD file wrapper (aliased as FsFile downstream via HalStorage.h).
// Forward-declared here so the header doesn't pull in the HAL; the persistent
// handle member is a unique_ptr, completed in SdCardFont.cpp.
class HalFile;

// On-disk binary format version for .cpfont files. Defined as a preprocessor
// macro (rather than a constexpr) so it can be stringified into the SD-fonts
// release URL — see FONT_MANIFEST_URL in FontDownloadActivity.h. No integer
// suffix because stringification would include it (e.g. `4U` → `"4U"`).
//
// The canonical version for the build tooling lives in
// lib/EpdFont/scripts/cpfont_version.py. This firmware-side copy must be
// bumped manually when the firmware is updated to support a new format.
// Reader enforcement: SdCardFont::load().
#define CPFONT_VERSION 4

// Low-level .cpfont (v4) loader and glyph cache. Shared by both the reader
// (one instance, via ReaderFontManager) and the UI/theme system (up to 8
// instances deduped by path, via ThemeFontManager). Each instance owns one
// open file path plus per-style glyph state.
//
// State machine for each style's EpdFontData pointer (see PerStyle below):
//
//   load()        ─►  epdFont.data = &stubData
//                     (header metrics only; glyphs resolved on-demand via the
//                      miss handler → overflow LRU. Used by UI roles that
//                      render arbitrary text without a prewarm pass.)
//
//   prewarm(text) ─►  epdFont.data = &miniData
//                     (mini intervals + glyph table + bitmap blob for the
//                      visible codepoints; miss handler still wired for any
//                      glyph not in the prewarm set. Used by both UI screens
//                      and reader page renders.)
//
//   clearCache()  ─►  epdFont.data = &stubData
//                     (mini buffers freed; persistent advance cache survives)
//
//   freeAll()     ─►  both zeroed; instance is effectively dead until load()
//
// Two distinct paths exercise this object:
//
//   - Shared (UI + reader): load → prewarm → render → clearCache loop. Driven
//     by FontCacheManager::prewarmCache via GfxRenderer.
//
//   - Reader-only (layout): buildAdvanceTable + getAdvance. The advance table
//     is a separate persistent cache (sized for an entire book layout pass)
//     that the UI never touches — UI text has fixed positions and never
//     measures via this path. See ParsedText.cpp + TxtReaderActivity.cpp for
//     the call sites; GfxRenderer::ensureSdCardFontReady forwards into it.
class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;

  SdCardFont() = default;
  ~SdCardFont();
  // Owns raw buffers freed in dtor — no shallow-copy semantics. Make any
  // accidental pass-by-value or move a compile-time error.
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false);

  // --- Reader-only layout API ----------------------------------------------
  // The advance table is exercised exclusively by the reader's layout pass
  // (ParsedText / TxtReaderActivity → GfxRenderer::ensureSdCardFontReady).
  // The UI never builds or queries it because UI text has fixed widths and
  // never re-flows. The table coexists with mini data — it's a separate,
  // longer-lived cache keyed by codepoint, not page.

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from words (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F);
  int buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask = 0x0F);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;
  // --- End reader-only layout API ------------------------------------------

  // Free mini data for all styles, restore stub EpdFontData.
  // Also clears the temporary advance table (built per layout pass) but
  // preserves the persistent advance cache (reused across passes).
  void clearCache();

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes.
  void clearPersistentCache();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;

    // Persistent kern-class + ligature tables (lazy-loaded on first prewarm).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; the matrix is reconstructed per-page as miniKernMatrix.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

    // Per-page mini kern matrix (built by buildMiniKernMatrix on each full
    // prewarm). miniKernLeftClasses/miniKernRightClasses map ONLY the codepoints
    // used on the current page to renumbered class IDs (1..miniKern*ClassCount).
    // miniKernMatrix is a small miniKernLeftClassCount × miniKernRightClassCount
    // flat matrix. Typical Latin page: ~25×25 matrix = ~625 bytes per style vs
    // ~36KB for the full Literata matrix — ~50× reduction.
    EpdKernClassEntry* miniKernLeftClasses = nullptr;
    EpdKernClassEntry* miniKernRightClasses = nullptr;
    uint16_t miniKernLeftEntryCount = 0;
    uint16_t miniKernRightEntryCount = 0;
    uint8_t miniKernLeftClassCount = 0;
    uint8_t miniKernRightClassCount = 0;
    int8_t* miniKernMatrix = nullptr;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Self-warming on-demand glyph cache. Holds glyphs not in the prewarmed mini
  // data — populated by glyphMissHandler on the first drawText that touches a
  // not-yet-cached codepoint, and persisted across paints. Byte-budgeted so a
  // whole screen's (or reader page's) working set stays resident; this is what
  // lets activities render correctly WITHOUT a prewarm pass. LRU-evicted by
  // lastUsedTick when either the slot cap or the byte budget is exceeded.
  //
  // Memory cost: up to OVERFLOW_BUDGET_BYTES of bitmap data plus the entry
  // vector (~32 B/entry). Reads go through the persistent overflowFile_ handle
  // so a miss costs a seek+read, not a directory-walking file open.
  static constexpr uint32_t OVERFLOW_MAX_SLOTS = 512;
  static constexpr uint32_t OVERFLOW_BUDGET_BYTES = 32 * 1024;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
    uint32_t lastUsedTick = 0;  // LRU timestamp; bumped on hit and insertion.
  };
  std::vector<OverflowEntry> overflow_;
  uint32_t overflowBytes_ = 0;
  uint32_t nextLruTick_ = 1;  // Monotonic counter (0 reserved for "unused" sentinel).

  // Self-warming kern-row cache. The full kern matrix is too big to keep
  // resident; instead getKerning() fetches one matrix row (per left class) on
  // demand via onKernRow, which reads it through overflowFile_ and caches it
  // here. A screen touches only ~30-50 distinct left classes, so the cache
  // stays small and warms in a single render pass. Byte-budgeted + LRU.
  static constexpr uint32_t KERN_ROW_MAX_SLOTS = 96;
  static constexpr uint32_t KERN_ROW_BUDGET_BYTES = 12 * 1024;
  struct KernRowEntry {
    uint8_t styleIdx = 0;
    uint8_t leftClass = 0;
    uint32_t lastUsed = 0;
    std::vector<int8_t> row;  // kernRightClassCount values for this left class
  };
  std::vector<KernRowEntry> kernRows_;
  uint32_t kernRowBytes_ = 0;
  uint32_t nextKernTick_ = 1;

  // Persistent read handle for on-demand glyph + kern-row reads. Opened lazily
  // on first miss and reused across paints (closed by clearOverflow/freeAll).
  // Deep-sleep wake is a full chip reset, so no stale handle survives sleep.
  std::unique_ptr<HalFile> overflowFile_;

  // Compact advance-only table for layout measurement (per-style).
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  // Per-style advance table. Sorted by codepoint for binary lookup.
  // Bounded to ADVANCE_CACHE_LIMIT entries; persists across layout passes
  // (across calls to clearCache()) so repeated indexing of the same font
  // amortizes SD reads. Cleared only on font unload or clearPersistentCache().
  static constexpr uint32_t ADVANCE_CACHE_LIMIT = 768;
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  bool advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const;
  // Merge sortedNew (sorted by codepoint, no overlap with existing) into the
  // advance table for styleIdx, preserving sort order; cap-truncates the tail.
  void mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount);
  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Fingerprint of the last successful prewarm — FNV-1a hash of the sorted
  // codepoints + styleMask + metadataOnly flag. Used by prewarm() to skip
  // the destructive rebuild when the same content is re-prewarmed (a common
  // case when activities re-render with stable visible text). Reset by
  // clearCache() / freeAll() so the next prewarm rebuilds unconditionally.
  uint32_t lastPrewarmHash_ = 0;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  void freeStyleMiniKern(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  // Lazy-load fullIntervals for a style. Returns true if intervals are
  // resident after the call. Cheap when already loaded (no-op).
  bool ensureStyleIntervalsLoaded(uint8_t styleIdx);
  bool buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask);
  template <typename Iter>
  int buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask);
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Persistent read handle for the on-demand caches.
  HalFile* ensureOverflowFileOpen();
  void closeOverflowFile();
  // Evict LRU entries until a new item fits the slot cap + byte budget.
  void evictOverflowToFit(uint32_t neededBytes);
  void evictKernRowsToFit(uint32_t neededBytes);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
  // Static callback for EpdFontData::kernRowHandler — returns the kern matrix
  // row for a 1-based left class, reading + caching it on demand.
  static const int8_t* onKernRow(void* ctx, uint8_t leftClass);
};
