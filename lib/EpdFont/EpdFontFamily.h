#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, UNDERLINE = 4 };

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;
  /// Same as getGlyph(cp, style) but reports which EpdFontData supplied the glyph
  /// via outData. Set to either getData(style) (primary hit / replacement substitution)
  /// or to the fallback font's data when the codepoint missed in the primary and was
  /// resolved through `EpdFont::fallback_`.
  const EpdGlyph* getGlyph(uint32_t cp, Style style, const EpdFontData** outData) const;
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;

  /// Sets a single shared fallback EpdFont across all four style slots. Style-level
  /// granularity isn't useful here — the renderer only ever wires one fallback family.
  /// Marked const because EpdFontFamily stores `const EpdFont*` members and
  /// EpdFont::setFallback mutates a `mutable` field.
  void setFallback(const EpdFont* fallback) const;
  const EpdFont* getRegular() const { return regular; }

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;

  const EpdFont* getFont(Style style) const;
};
