#include "FontInstaller.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFontSystem.h>
#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#endif

#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

namespace {
bool isSafeFontPathChar(const char c) {
  return static_cast<unsigned char>(c) >= 0x80 || std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
         c == ' ' || c == '.' || c == '(' || c == ')';
}
#if CROSSINK_SCALABLE_FONTS
// Match discovery names: folder names for grouped TTFs, metadata for loose files.
// Scan skipped duplicate styles too, so they cannot reappear after deletion.
bool deleteTtfFamily(const char* path, const char* family, const char* folderName = nullptr) {
  HalFile dir = Storage.open(path);
  if (!dir) return !Storage.exists(path);
  char name[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) {
      const bool ok = !FsHelpers::directoryIterationFailed(dir);
      dir.close();
      return ok;
    }
    const bool directory = entry.isDirectory();
    entry.getName(name, sizeof(name));
    entry.close();
    if (name[0] == '.' || name[0] == '_') continue;
    const std::string full = std::string(path) + "/" + name;
    if (directory) {
      if (!folderName && strcmp(name, family) == 0 && !deleteTtfFamily(full.c_str(), family, name)) {
        dir.close();
        return false;
      }
      continue;
    }
    const size_t length = strlen(name);
    if (length < 5 || strcasecmp(name + length - 4, ".ttf") != 0) continue;
    HalScalableFont::Info info;
    bool unavailable = false;
    const bool valid = HalScalableFont::inspectFile(full.c_str(), info, &unavailable);
    const char* discoveredName = folderName ? folderName : info.family;
    if (unavailable || (valid && strcmp(discoveredName, family) == 0 && !Storage.remove(full.c_str()))) {
      LOG_ERR("FONT", "Cannot inspect/remove TTF: %s", full.c_str());
      dir.close();
      return false;
    }
  }
}
#endif
}  // namespace

bool FontInstaller::isValidFamilyName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Reject path traversal
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  for (const char* p = name; *p != '\0'; ++p) {
    char c = *p;
    if (!isSafeFontPathChar(c)) {
      return false;
    }
  }
  return true;
}

bool FontInstaller::isValidCpfontFilename(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Reject path separators / traversal up front. Anything that could escape
  // the family directory or refer to a different one is a hard reject.
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

#if CROSSINK_SCALABLE_FONTS
  const size_t n = strlen(name);
  if (n > 4 && strcasecmp(name + n - 4, ".ttf") == 0) {
    for (const char* p = name; *p; ++p)
      if (!isSafeFontPathChar(*p)) return false;
    return true;
  }
#endif
  // Must end with ".cpfont" exactly.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  size_t nameLen = strlen(name);
  if (nameLen <= kExtLen) return false;
  if (strcmp(name + nameLen - kExtLen, kExt) != 0) return false;

  // Basename (before .cpfont) must stay within safe single-path-component
  // characters. Additional dots are allowed because GitHub release assets
  // expose spaces as dots.
  size_t baseLen = nameLen - kExtLen;
  for (size_t i = 0; i < baseLen; ++i) {
    char c = name[i];
    if (!isSafeFontPathChar(c)) {
      return false;
    }
  }
  return true;
}

bool FontInstaller::ensureFamilyDir(const char* familyName) {
  // Reuse the family's existing root if installed; otherwise pick the
  // default-write root (hidden if no roots exist yet).
  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();

  if (!Storage.exists(root)) {
    if (!Storage.mkdir(root)) {
      LOG_ERR("FONT", "Failed to create fonts dir: %s", root);
      return false;
    }
  }

  char dirPath[192];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);

  if (!Storage.exists(dirPath)) {
    if (!Storage.mkdir(dirPath)) {
      LOG_ERR("FONT", "Failed to create family dir: %s", dirPath);
      return false;
    }
  }
  return true;
}

bool FontInstaller::validateCpfontFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file)) {
    LOG_ERR("FONT", "Cannot open for validation: %s", path);
    return false;
  }

  const size_t pathLength = strlen(path);
  const bool ttf = pathLength > 4 && strcasecmp(path + pathLength - 4, ".ttf") == 0;
  uint8_t magic[CPFONT_MAGIC_LEN];
  size_t bytesRead = file.read(magic, CPFONT_MAGIC_LEN);
  file.close();

  if (bytesRead < CPFONT_MAGIC_LEN) {
    LOG_ERR("FONT", "File too small: %s (%zu bytes)", path, bytesRead);
    return false;
  }

#if CROSSINK_SCALABLE_FONTS
  const bool staticTtf = memcmp(magic, "\0\1\0\0", 4) == 0 || memcmp(magic, "true", 4) == 0;
  if (ttf && staticTtf) {
    HalScalableFont::Info info;
    return HalScalableFont::inspectFile(path, info);
  }
#endif
  if (ttf || memcmp(magic, "CPFONT\0\0", CPFONT_MAGIC_LEN) != 0) {
    LOG_ERR("FONT", "Bad magic in: %s", path);
    return false;
  }

  return true;
}

void FontInstaller::buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize) {
  // Use the same root selection as ensureFamilyDir: existing install dir wins,
  // otherwise the default-write root.
  const char* root = SdCardFontRegistry::findFamilyRoot(family);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();
  snprintf(outBuf, outBufSize, "%s/%s/%s", root, family, filename);
}

FontInstaller::Error FontInstaller::deleteFamily(const char* familyName) {
  if (!isValidFamilyName(familyName)) return Error::INVALID_FAMILY_NAME;
  sdFontSystem.markRegistryDirty();
#if CROSSINK_SCALABLE_FONTS
  if (registry_.lastDiscoveryFailed()) return Error::SD_WRITE_ERROR;
  const auto* scalable = registry_.findSummary(familyName);
  if (scalable && scalable->isScalable()) {
    const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
    for (const char* root : roots) {
      char resolved[32];
      if (FsHelpers::resolveRootDirectoryIgnoreCase(root, resolved, sizeof(resolved)) &&
          !deleteTtfFamily(resolved, familyName))
        return Error::SD_WRITE_ERROR;
    }
    if (strcmp(SETTINGS.sdFontFamilyName, familyName) == 0) {
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
    }
    return Error::OK;
  }
#endif
  // A family may exist in either root (or, edge case, both). Remove from both.
  const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
  bool removedAny = false;
  bool sawAny = false;
  for (const char* root : roots) {
    char dirPath[192];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);
    if (!Storage.exists(dirPath)) continue;
    sawAny = true;
    if (!Storage.removeDir(dirPath)) {
      LOG_ERR("FONT", "Failed to remove family dir: %s", dirPath);
      return Error::SD_WRITE_ERROR;
    }
    removedAny = true;
  }

  if (!sawAny) {
    LOG_DBG("FONT", "Family not found in any fonts root: %s", familyName);
    return Error::OK;  // Already gone
  }
  (void)removedAny;

  // If this was the active font, clear the setting
  if (strcmp(SETTINGS.sdFontFamilyName, familyName) == 0) {
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("FONT", "Cleared active SD font (deleted family: %s)", familyName);
  }

  return Error::OK;
}

bool FontInstaller::refreshRegistry() {
  registry_.discover();
  return !registry_.lastDiscoveryFailed();
}
