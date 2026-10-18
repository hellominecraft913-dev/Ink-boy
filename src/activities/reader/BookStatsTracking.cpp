#include "BookStatsTracking.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"

namespace {
constexpr char MARKER_NAME[] = "/reading_stats_off";

std::string markerPath(const std::string& cachePath) { return cachePath + MARKER_NAME; }
}  // namespace

namespace BookStatsTracking {
bool isBookEnabled(const std::string& cachePath) {
  return cachePath.empty() || !Storage.exists(markerPath(cachePath).c_str());
}

bool isEnabled(const std::string& cachePath) { return SETTINGS.shouldTrackReadingStats() && isBookEnabled(cachePath); }

bool setBookEnabled(const std::string& cachePath, const bool enabled) {
  if (cachePath.empty()) {
    LOG_ERR("BSTAT", "Missing cache path for book tracking setting");
    return false;
  }
  const std::string path = markerPath(cachePath);
  if (enabled) {
    if (!Storage.exists(path.c_str())) return true;
    if (Storage.remove(path.c_str())) return true;
    LOG_ERR("BSTAT", "Could not enable book stats: %s", path.c_str());
    return false;
  }
  if (Storage.exists(path.c_str())) return true;
  if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("BSTAT", "Could not create book cache: %s", cachePath.c_str());
    return false;
  }
  const std::string tempPath = path + ".tmp";
  FsFile file;
  if (!Storage.openFileForWrite("BSTAT", tempPath, file)) {
    LOG_ERR("BSTAT", "Could not disable book stats: %s", path.c_str());
    return false;
  }
  const uint8_t marker = 1;
  const bool written = file.write(&marker, sizeof(marker)) == sizeof(marker) && file.sync();
  const bool closed = file.close();
  if (!written || !closed || !Storage.rename(tempPath.c_str(), path.c_str())) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("BSTAT", "Could not finish book tracking setting: %s", path.c_str());
    return false;
  }
  return true;
}
}  // namespace BookStatsTracking
