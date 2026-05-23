#pragma once

#include <memory>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"  // for FontRole

class GfxRenderer;
class SdCardFont;

// Theme font role progressive enhancement.
//
// Themes ask for fonts by semantic role (Title/Heading/Body/Caption/Accent).
// At boot, this registry walks the SD card looking for role-specific
// `.cpfont` files at
//
//   /.fonts/themes/<themeName>/<roleName>.cpfont
//
// Each file found is loaded, registered with the renderer (so drawText sees
// it as a normal font ID), and indexed under (themeName, role) for lookup.
//
// Themes that want SD progressive enhancement consult `getRoleFont()` in
// their `getFontForRole()` override and fall back to embedded faces when no
// SD override is installed.
//
// Each loaded SD role font carries the usual SdCardFont cost (~10–30 KB of
// kern/intervals/cache per face). With at most one face per role and
// typically 1–2 roles overridden per theme, the steady-state cost stays
// within budget. The registry never auto-loads — it only loads files the
// user has explicitly placed on the SD card.
class ThemeFontRegistry {
 public:
  static ThemeFontRegistry& getInstance();

  // Scan `/.fonts/themes/<themeId>/*.cpfont`, load any role fonts found,
  // and register them with the renderer. Only the named theme's directory
  // is touched — fonts for other themes stay on disk untouched. Safe to
  // call after Storage.begin(). Subsequent calls reload the same theme's
  // role fonts from disk (useful after the web UI installs/removes one).
  void discover(GfxRenderer& renderer, const char* themeId);

  // Switch the active theme. If `themeId` differs from the currently-active
  // theme, unload the previous theme's fonts entirely (frees ~50 KB
  // persistent state + ~30 KB glyph cache per font) and discover the new
  // theme's fonts. No-op if `themeId` matches the active theme.
  void setActiveTheme(GfxRenderer& renderer, const char* themeId);

  // Re-discover the currently-active theme's fonts. Used by activities like
  // Library that unload before transitioning into the reader and want their
  // theme fonts back on return.
  void reloadActive(GfxRenderer& renderer);

  // Returns the active theme id (empty string if no theme has been set).
  const char* getActiveThemeId() const { return activeThemeId_.c_str(); }

  // Returns the renderer font ID registered for (themeName, role), or 0
  // when no SD override is installed. The 0 sentinel matches the renderer's
  // "font not found" convention.
  int getRoleFont(const char* themeName, FontRole role) const;

  // Unregister every loaded role font from the renderer and drop the
  // backing SdCardFont instances. Releases the ~50 KB of resident font
  // intervals + kerning tables that load() keeps in RAM per face. After
  // calling, getRoleFont() returns 0 for every role until discover() is
  // called again. Used by activities that want to claim that RAM on exit.
  void unloadAll(GfxRenderer& renderer);

 private:
  struct LoadedRoleFont {
    std::string themeName;
    FontRole role;
    int fontId = 0;
    std::unique_ptr<SdCardFont> font;
  };

  std::vector<LoadedRoleFont> loaded_;
  std::string activeThemeId_;

  ThemeFontRegistry() = default;
  ~ThemeFontRegistry();
  ThemeFontRegistry(const ThemeFontRegistry&) = delete;
  ThemeFontRegistry& operator=(const ThemeFontRegistry&) = delete;

  // Internal helper shared by discover() (which calls it before scanning)
  // and the public unloadAll() (used by activities on exit).
  void clear(GfxRenderer& renderer);

  // Try to load one file under (themeName, roleName). Returns true on success.
  bool loadRoleFile(GfxRenderer& renderer, const std::string& themeName, const std::string& roleName,
                    const std::string& filePath);

  // Walk one root (`/.fonts/themes` or `/fonts/themes`) looking only for
  // the named theme's subdirectory.
  void scanRoot(GfxRenderer& renderer, const char* rootPath, const char* themeId);

  // Parse "roleName" out of a filename like "caption.cpfont". Returns
  // FontRole and sets ok=true on success.
  static FontRole parseRoleName(const char* filename, bool& ok);

  // Generate a deterministic font ID for (themeName, role, contentHash).
  // Same FNV-1a continuation pattern used by SdCardFontManager.
  static int computeFontId(uint32_t contentHash, const char* themeName, const char* roleName);
};

#define THEME_FONTS ThemeFontRegistry::getInstance()
