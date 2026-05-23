#include "ThemeFontRegistry.h"

#include <EpdFontFamily.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

namespace {
constexpr char LOG_TAG[] = "TFR";

// Two roots — mirrors the existing SD font convention (see
// SdCardFontRegistry::FONTS_DIR_HIDDEN / _VISIBLE). The "themes" subfolder
// keeps role-fonts separate from user-installed reader fonts so the existing
// reader font picker doesn't list them.
constexpr const char* THEMES_DIR_HIDDEN = "/.fonts/themes";
constexpr const char* THEMES_DIR_VISIBLE = "/fonts/themes";

constexpr char CPFONT_EXT[] = ".cpfont";

}  // namespace

ThemeFontRegistry& ThemeFontRegistry::getInstance() {
  static ThemeFontRegistry instance;
  return instance;
}

ThemeFontRegistry::~ThemeFontRegistry() = default;

void ThemeFontRegistry::discover(GfxRenderer& renderer, const char* themeId) {
  if (themeId == nullptr || themeId[0] == '\0') return;
  clear(renderer);
  scanRoot(renderer, THEMES_DIR_HIDDEN, themeId);
  scanRoot(renderer, THEMES_DIR_VISIBLE, themeId);
  activeThemeId_ = themeId;
  LOG_DBG(LOG_TAG, "Discovered %d role font(s) for theme '%s'", static_cast<int>(loaded_.size()), themeId);
}

void ThemeFontRegistry::setActiveTheme(GfxRenderer& renderer, const char* themeId) {
  if (themeId == nullptr) return;
  if (activeThemeId_ == themeId) return;
  discover(renderer, themeId);
}

void ThemeFontRegistry::reloadActive(GfxRenderer& renderer) {
  if (activeThemeId_.empty()) return;
  // Capture by value — discover() clears state, including activeThemeId_,
  // and we'd be reading it through itself otherwise.
  const std::string id = activeThemeId_;
  discover(renderer, id.c_str());
}

void ThemeFontRegistry::scanRoot(GfxRenderer& renderer, const char* rootPath, const char* themeId) {
  // Build the exact directory path for the named theme rather than walking
  // every theme directory under the root — saves a directory enumeration
  // on a constrained device and avoids touching unused themes' files.
  const std::string themeDirPath = std::string(rootPath) + "/" + themeId;
  if (!Storage.exists(themeDirPath.c_str())) return;

  HalFile themeDir = Storage.open(themeDirPath.c_str());
  if (!themeDir || !themeDir.isDirectory()) return;
  themeDir.rewindDirectory();

  char roleFileName[128];
  for (HalFile roleFile = themeDir.openNextFile(); roleFile; roleFile = themeDir.openNextFile()) {
    if (roleFile.isDirectory()) continue;
    roleFile.getName(roleFileName, sizeof(roleFileName));
    if (roleFileName[0] == '.') continue;
    if (!FsHelpers::checkFileExtension(std::string_view{roleFileName}, CPFONT_EXT)) continue;

    // Strip ".cpfont" to recover the role name.
    const size_t nameLen = std::strlen(roleFileName);
    const size_t extLen = std::strlen(CPFONT_EXT);
    if (nameLen <= extLen) continue;
    const std::string roleName{roleFileName, nameLen - extLen};

    const std::string filePath = themeDirPath + "/" + roleFileName;
    loadRoleFile(renderer, themeId, roleName, filePath);
  }
  themeDir.close();
}

bool ThemeFontRegistry::loadRoleFile(GfxRenderer& renderer, const std::string& themeName,
                                     const std::string& roleName, const std::string& filePath) {
  bool ok = false;
  const FontRole role = parseRoleName(roleName.c_str(), ok);
  if (!ok) {
    LOG_DBG(LOG_TAG, "Skipping %s: unrecognized role '%s'", filePath.c_str(), roleName.c_str());
    return false;
  }

  auto font = std::make_unique<SdCardFont>();
  if (!font->load(filePath.c_str())) {
    LOG_ERR(LOG_TAG, "Failed to load theme role font %s", filePath.c_str());
    return false;
  }

  const int fontId = computeFontId(font->contentHash(), themeName.c_str(), roleName.c_str());
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR(LOG_TAG, "Font ID %d collision; skipping %s", fontId, filePath.c_str());
    return false;
  }

  renderer.registerSdCardFont(fontId, font.get());
  EpdFontFamily family(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, family);

  LOG_DBG(LOG_TAG, "Loaded %s -> id=%d styles=%u", filePath.c_str(), fontId, font->styleCount());

  loaded_.push_back({themeName, role, fontId, std::move(font)});
  return true;
}

void ThemeFontRegistry::unloadAll(GfxRenderer& renderer) {
  clear(renderer);
  loaded_.shrink_to_fit();
  // Preserve activeThemeId_ — callers like LibraryActivity unload to reclaim
  // RAM before the reader and then expect reloadActive() to re-discover the
  // same theme on return.
}

int ThemeFontRegistry::getRoleFont(const char* themeName, FontRole role) const {
  if (themeName == nullptr) return 0;
  for (const auto& lf : loaded_) {
    if (lf.role == role && lf.themeName == themeName) return lf.fontId;
  }
  return 0;
}

void ThemeFontRegistry::clear(GfxRenderer& renderer) {
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
  }
  loaded_.clear();
}

FontRole ThemeFontRegistry::parseRoleName(const char* name, bool& ok) {
  ok = true;
  if (std::strcmp(name, "title") == 0) return FontRole::Title;
  if (std::strcmp(name, "heading") == 0) return FontRole::Heading;
  if (std::strcmp(name, "body") == 0) return FontRole::Body;
  if (std::strcmp(name, "caption") == 0) return FontRole::Caption;
  if (std::strcmp(name, "accent") == 0) return FontRole::Accent;
  if (std::strcmp(name, "body-compact") == 0) return FontRole::BodyCompact;
  if (std::strcmp(name, "caption-compact") == 0) return FontRole::CaptionCompact;
  if (std::strcmp(name, "accent-compact") == 0) return FontRole::AccentCompact;
  ok = false;
  return FontRole::Body;
}

int ThemeFontRegistry::computeFontId(uint32_t contentHash, const char* themeName, const char* roleName) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*themeName) {
    hash ^= static_cast<uint8_t>(*themeName++);
    hash *= FNV_PRIME;
  }
  hash ^= 0x2E;  // '.' separator so role-X and themeX-role can't collide
  hash *= FNV_PRIME;
  while (*roleName) {
    hash ^= static_cast<uint8_t>(*roleName++);
    hash *= FNV_PRIME;
  }
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 reserved as "not found" sentinel
}
