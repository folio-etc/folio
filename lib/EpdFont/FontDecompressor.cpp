#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdlib>

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::freePageBuffer() {
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    free(pageSlots[s].buffer);
    free(pageSlots[s].glyphs);
    pageSlots[s] = {};
  }
  pageSlotCount = 0;
}

void FontDecompressor::freeHotGroup() {
  hotGroup.clear();
  hotGroup.shrink_to_fit();
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  hotGlyphBuf.clear();
  hotGlyphBuf.shrink_to_fit();
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: linear scan
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height) {
  if (width == 0 || height == 0) return;
  const uint32_t rowStride = (width + 3) / 4;
  if (width % 4 == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << 2) | ((alignedSrc[y * rowStride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3);
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: page buffer → hot group → decompress ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per font style)
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    const auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphCount == 0) continue;

    int left = 0, right = slot.glyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &slot.buffer[slot.glyphs[mid].bufferOffset];
        }
        break;  // Not extracted during prewarm; fall through to hot-group path
      }
      if (slot.glyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
    break;  // Found the right slot but glyph wasn't in it; don't check other slots
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Check if hot group already has this group decompressed — if not, decompress it
  if (!(!hotGroup.empty() && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    hotGroup.resize(group.uncompressedSize);
    if (hotGroup.empty()) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroup.data(), group.uncompressedSize)) {
      hotGroup.clear();
      hotGroup.shrink_to_fit();
      hotGroupFont = nullptr;
      hotGroupIndex = UINT16_MAX;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (glyph->dataLength > hotGlyphBuf.size()) {
    hotGlyphBuf.resize(glyph->dataLength);
  }
  if (hotGlyphBuf.empty()) {
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  compactSingleGlyph(&hotGroup[alignedOff], hotGlyphBuf.data(), glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf.data();
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  if (!fontData || !fontData->groups || !utf8Text) return 0;

  // Idempotent short-circuit: hash the page text. An exact (fontData, hash)
  // match means this page was already prewarmed — nothing to do.
  static constexpr uint32_t FNV_OFFSET = 2166136261u;
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t textHash = FNV_OFFSET;
  for (const unsigned char* h = reinterpret_cast<const unsigned char*>(utf8Text); *h; h++) {
    textHash ^= *h;
    textHash *= FNV_PRIME;
  }
  if (textHash == 0) textHash = 1;  // 0 is the not-yet-warmed sentinel

  // Locate an existing slot for this font WITHOUT freeing it. A page change
  // reuses the slot incrementally so glyphs decompressed for earlier pages
  // stay resident — consecutive pages sharing an alphabet then skip
  // decompression entirely (see prewarmAppend).
  uint8_t slotIdx = MAX_PAGE_SLOTS;  // sentinel: no existing slot for this font
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    if (pageSlots[s].fontData != fontData) continue;
    if (pageSlots[s].textHash == textHash) return 0;  // exact page hit
    slotIdx = s;
    break;
  }

  // Step 1: Collect unique glyph indices needed for this page
  uint32_t neededGlyphs[MAX_PAGE_GLYPHS];
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0) continue;

    // Deduplicate
    bool found = false;
    for (uint16_t i = 0; i < glyphCount; i++) {
      if (neededGlyphs[i] == static_cast<uint32_t>(glyphIdx)) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (glyphCount < MAX_PAGE_GLYPHS) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(glyphIdx);
      } else if (!glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
    }
  }

  // Add ligature output glyphs: if both input codepoints of a ligature pair are
  // in the needed set, the output glyph will be queried during rendering.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      // Check if both inputs are in neededGlyphs
      bool hasLeft = false, hasRight = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(leftIdx)) hasLeft = true;
        if (neededGlyphs[i] == static_cast<uint32_t>(rightIdx)) hasRight = true;
        if (hasLeft && hasRight) break;
      }
      if (!hasLeft || !hasRight) continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;

      // Deduplicate
      bool found = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(outIdx)) {
          found = true;
          break;
        }
      }
      if (!found) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(outIdx);
      }
    }
  }

  if (glyphCount == 0) return 0;

  // Incremental append: grow an existing slot in place, decompressing only the
  // groups of glyphs not already resident. The union must fit MAX_PAGE_GLYPHS;
  // otherwise fall through to a full rebuild for the current page (which evicts
  // all stale glyphs). MAX_PAGE_GLYPHS (512) is far above a typical working
  // set, so the rebuild is a cold path.
  if (slotIdx < MAX_PAGE_SLOTS && pageSlots[slotIdx].glyphCount + glyphCount <= MAX_PAGE_GLYPHS) {
    return prewarmAppend(pageSlots[slotIdx], fontData, neededGlyphs, glyphCount, textHash);
  }

  // Full build: a new font's slot, or an eviction rebuild for the current page.
  bool isNewSlot = (slotIdx == MAX_PAGE_SLOTS);
  if (isNewSlot) {
    if (pageSlotCount >= MAX_PAGE_SLOTS) {
      LOG_ERR("FDC", "All %u page buffer slots full, cannot prewarm fontData=%p", MAX_PAGE_SLOTS, (void*)fontData);
      return -1;
    }
    slotIdx = pageSlotCount;
  } else {
    free(pageSlots[slotIdx].buffer);
    free(pageSlots[slotIdx].glyphs);
    pageSlots[slotIdx] = {};
  }
  PageSlot& slot = pageSlots[slotIdx];

  // Step 2: Compute total buffer size and collect unique groups
  uint32_t totalBytes = 0;
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += fontData->glyph[neededGlyphs[i]].dataLength;
    uint16_t gi = getGroupIndex(fontData, neededGlyphs[i]);
    bool found = false;
    for (uint8_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == gi) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (groupCount < 128) {
        neededGroups[groupCount++] = gi;
      } else if (!groupCapWarned) {
        LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use hot-group fallback");
        groupCapWarned = true;
      }
    }
  }

  stats.uniqueGroupsAccessed = groupCount;

  // Step 3: Allocate page buffer and lookup table for this slot
  slot.buffer = static_cast<uint8_t*>(malloc(totalBytes));
  slot.glyphs = static_cast<PageGlyphEntry*>(malloc(glyphCount * sizeof(PageGlyphEntry)));
  if (!slot.buffer || !slot.glyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    free(slot.buffer);
    free(slot.glyphs);
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  slot.textHash = textHash;
  slot.bufferBytes = totalBytes;
  if (isNewSlot) pageSlotCount++;

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  }

  // Sort by glyphIndex for binary search in getBitmap()
  for (uint16_t i = 1; i < glyphCount; i++) {
    PageGlyphEntry key = slot.glyphs[i];
    int j = i - 1;
    while (j >= 0 && slot.glyphs[j].glyphIndex > key.glyphIndex) {
      slot.glyphs[j + 1] = slot.glyphs[j];
      j--;
    }
    slot.glyphs[j + 1] = key;
  }

  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  uint32_t groupAlignedTracker[128] = {};  // running byte-aligned offset for each needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup
    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      // Find this glyph's group position in neededGroups
      uint8_t gpPos = groupCount;
      for (uint8_t j = 0; j < groupCount; j++) {
        if (neededGroups[j] == gi) {
          gpPos = j;
          break;
        }
      }
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted slot.glyphs to find if glyph i is needed
      int left = 0, right = (int)slot.glyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (slot.glyphs[mid].glyphIndex == i) {
          slot.glyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (slot.glyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + 3) / 4) * glyph.height;
      }
    }
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
    for (uint8_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyph& glyph = fontData->glyph[glyphI];

        int left = 0, right = (int)slot.glyphCount - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (slot.glyphs[mid].glyphIndex == glyphI) {
            slot.glyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (slot.glyphs[mid].glyphIndex < glyphI)
            left = mid + 1;
          else
            right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + 3) / 4) * glyph.height;
        }
      }
    }
  }

  // Step 4: For each unique group, decompress to temp buffer and extract needed glyphs
  uint32_t writeOffset = 0;
  int missed = 0;

  for (uint8_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    auto* tempBuf = static_cast<uint8_t*>(malloc(group.uncompressedSize));
    if (!tempBuf) {
      LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes) for group %u", group.uncompressedSize, groupIdx);
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) {
      stats.peakTempBytes = group.uncompressedSize;
    }

    if (!decompressGroup(fontData, groupIdx, tempBuf, group.uncompressedSize)) {
      free(tempBuf);
      missed++;
      continue;
    }

    // Extract needed glyphs directly from the byte-aligned temp buffer, compacting on the fly.
    // alignedOffset was pre-computed in step 3b — no full-group compact scan needed.
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[writeOffset], glyph.width, glyph.height);
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += glyph.dataLength;
    }

    free(tempBuf);
  }

  LOG_DBG("FDC", "Prewarm: %u glyphs in %u bytes from %u groups (%d missed)", glyphCount, writeOffset, groupCount,
          missed);

  return missed;
}

int FontDecompressor::prewarmAppend(PageSlot& slot, const EpdFontData* fontData, uint32_t* neededGlyphs,
                                    uint16_t neededCount, uint32_t newTextHash) {
  // Filter neededGlyphs in place down to glyphs not already present in the slot.
  // A glyph already in the slot (extracted or not) is left alone — re-adding it
  // would create a duplicate entry and break getBitmap's binary search; an
  // unextracted entry harmlessly falls back to the hot-group path.
  uint16_t missingCount = 0;
  for (uint16_t i = 0; i < neededCount; i++) {
    const uint32_t gi = neededGlyphs[i];
    bool present = false;
    int left = 0, right = static_cast<int>(slot.glyphCount) - 1;
    while (left <= right) {
      const int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == gi) {
        present = true;
        break;
      }
      if (slot.glyphs[mid].glyphIndex < gi)
        left = mid + 1;
      else
        right = mid - 1;
    }
    if (!present) neededGlyphs[missingCount++] = gi;
  }

  if (missingCount == 0) {
    // Every glyph this page needs is already resident — just record the new
    // page hash so an identical re-render hits the exact short-circuit.
    slot.textHash = newTextHash;
    LOG_DBG("FDC", "Prewarm incremental: 0 new, %u resident", slot.glyphCount);
    return 0;
  }

  const uint16_t oldCount = slot.glyphCount;
  const uint16_t newCount = oldCount + missingCount;
  const uint32_t oldBytes = slot.bufferBytes;

  uint32_t addBytes = 0;
  for (uint16_t i = 0; i < missingCount; i++) addBytes += fontData->glyph[neededGlyphs[i]].dataLength;

  // Grow the lookup table and packed buffer. realloc preserves existing
  // content; all bufferOffsets are relative to the buffer base, so resident
  // glyphs stay valid even if the block moves.
  auto* ng = static_cast<PageGlyphEntry*>(realloc(slot.glyphs, newCount * sizeof(PageGlyphEntry)));
  if (!ng) {
    LOG_ERR("FDC", "Incremental realloc of %u glyph entries failed", newCount);
    return missingCount;  // slot left intact (realloc kept the old block)
  }
  slot.glyphs = ng;
  auto* nb = static_cast<uint8_t*>(realloc(slot.buffer, oldBytes + addBytes));
  if (!nb) {
    LOG_ERR("FDC", "Incremental realloc of %u buffer bytes failed", oldBytes + addBytes);
    return missingCount;  // glyph array is larger but glyphCount unchanged — still consistent
  }
  slot.buffer = nb;

  // Append entries for the missing glyphs into the unsorted tail
  // [oldCount, newCount). alignedOffset is the glyph's byte-aligned offset
  // within its decompressed group — same convention as the hot-group path.
  uint32_t writeOffset = oldBytes;
  for (uint16_t i = 0; i < missingCount; i++) {
    const uint32_t gi = neededGlyphs[i];
    const uint16_t grp = getGroupIndex(fontData, gi);
    slot.glyphs[oldCount + i] = {gi, writeOffset, getAlignedOffset(fontData, grp, gi)};
    writeOffset += fontData->glyph[gi].dataLength;
  }

  // Collect the unique groups the missing glyphs live in.
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  for (uint16_t i = 0; i < missingCount; i++) {
    const uint16_t grp = getGroupIndex(fontData, neededGlyphs[i]);
    bool seen = false;
    for (uint8_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == grp) {
        seen = true;
        break;
      }
    }
    if (!seen && groupCount < 128) neededGroups[groupCount++] = grp;
  }

  // Decompress each needed group once; compact its missing glyphs into the
  // freshly grown buffer region.
  int missed = 0;
  for (uint8_t g = 0; g < groupCount; g++) {
    const uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    auto* tempBuf = static_cast<uint8_t*>(malloc(group.uncompressedSize));
    if (!tempBuf) {
      LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes) for group %u", group.uncompressedSize, groupIdx);
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) stats.peakTempBytes = group.uncompressedSize;

    if (!decompressGroup(fontData, groupIdx, tempBuf, group.uncompressedSize)) {
      free(tempBuf);
      missed++;
      continue;
    }

    for (uint16_t i = oldCount; i < newCount; i++) {
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;
      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[slot.glyphs[i].bufferOffset], glyph.width,
                         glyph.height);
    }

    free(tempBuf);
  }

  // Re-sort the full entry array by glyphIndex for getBitmap's binary search.
  for (uint16_t i = 1; i < newCount; i++) {
    PageGlyphEntry key = slot.glyphs[i];
    int j = static_cast<int>(i) - 1;
    while (j >= 0 && slot.glyphs[j].glyphIndex > key.glyphIndex) {
      slot.glyphs[j + 1] = slot.glyphs[j];
      j--;
    }
    slot.glyphs[j + 1] = key;
  }

  slot.glyphCount = newCount;
  slot.bufferBytes = oldBytes + addBytes;
  slot.textHash = newTextHash;
  stats.pageBufferBytes += addBytes;
  stats.pageGlyphsBytes += missingCount * sizeof(PageGlyphEntry);

  LOG_DBG("FDC", "Prewarm incremental: %u new, %u resident (%d missed)", missingCount, newCount, missed);

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
