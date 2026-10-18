#include "HomeCoverCache.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>

#include "UITheme.h"

namespace fui = freeink::ui;

void HomeCoverCache::begin() {
  // Cover regions do not overlap. Each may widen by one physical byte per row.
  coverCacheCapacity = renderer.getRegionByteSize(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()) +
                       MAX_COVERS * std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  coverCache = makePsramByteBufferNoThrow(coverCacheCapacity);
  if (!coverCache) {
#if defined(SIMULATOR)
    LOG_DBG("HOME", "PSRAM cover cache unavailable in simulator; rendering uncached");
#else
    LOG_ERR("HOME", "PSRAM cover cache unavailable (%u bytes); rendering uncached", unsigned(coverCacheCapacity));
#endif
    coverCacheCapacity = 0;
  }
}

void HomeCoverCache::invalidate() {
  coverCacheUsed = 0;
  cachedCovers.fill(CachedCover{});
}

void HomeCoverCache::invalidate(size_t index) {
  if (index < cachedCovers.size()) cachedCovers[index].valid = false;
}

void HomeCoverCache::prepare() {
  const int orientation = static_cast<int>(renderer.getOrientation());
  if (coverCacheOrientation != orientation || coverCacheInverted != bool(SETTINGS.screenInverted)) {
    invalidate();
    coverCacheOrientation = orientation;
    coverCacheInverted = SETTINGS.screenInverted;
  }
}

bool HomeCoverCache::paint(fui::Rect rect, size_t index, const std::string& path) {
  if (index >= cachedCovers.size()) return false;
  auto& cached = cachedCovers[index];
  if (coverCache && cached.valid && cached.rect.x == rect.x && cached.rect.y == rect.y &&
      cached.rect.width == rect.width && cached.rect.height == rect.height &&
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, &coverCache[cached.offset], cached.bytes)) {
    return true;
  }
  cached.valid = false;
  bool drawn = false;
  if (!path.empty() && Storage.openFileForRead("HOME", path, coverFile)) {
    if (coverBitmap.parseHeaders() == BmpReaderError::Ok && coverBitmap.rewindToData() == BmpReaderError::Ok &&
        coverBitmap.getWidth() > 0 && coverBitmap.getHeight() > 0) {
      const int width = coverBitmap.getWidth();
      const int height = coverBitmap.getHeight();
      const float cropX = width > rect.width ? 1.0f - static_cast<float>(rect.width) / width : 0.0f;
      const float cropY = height > rect.height ? 1.0f - static_cast<float>(rect.height) / height : 0.0f;
      const int x = rect.x + std::max(0, (rect.width - width) / 2);
      const int y = rect.y + std::max(0, (rect.height - height) / 2);
      drawn = renderer.drawBitmap(coverBitmap, x, y, rect.width, rect.height, cropX, cropY);
    }
    coverFile.close();
  }
  if (!drawn) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    renderer.fillRect(rect.x, rect.y + rect.height / 3, rect.width, rect.height - rect.height / 3, true);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  }
  if (coverCache) {
    const size_t needed = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
    if (needed > cached.bytes && needed <= coverCacheCapacity - coverCacheUsed) {
      cached.offset = coverCacheUsed;
      cached.bytes = needed;
      coverCacheUsed += needed;
    }
    if (needed > 0 && needed <= cached.bytes) {
      cached.rect = rect;
      cached.valid = renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height, &coverCache[cached.offset],
                                                 cached.bytes);
    }
  }
  return true;  // The cover slot is painted, including fallback art.
}
