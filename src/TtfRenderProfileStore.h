#pragma once

#if CROSSINK_SCALABLE_FONTS

#include <ArduinoJson.h>
#include <FtFont.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

struct TtfRenderProfile {
  uint8_t hinting = 2;      // None, Native, Auto, Light
  uint8_t raster = 0;       // Grayscale, Monochrome
  uint8_t interpreter = 0;  // Default, FreeType 35, FreeType 40
  uint8_t weight = 2;       // -0.5 px through +1 px; 2 is unchanged
  uint8_t slant = 0;        // None, Gentle, Medium, Strong
  bool stemDarkening = false;

  bool operator==(const TtfRenderProfile& other) const {
    return hinting == other.hinting && raster == other.raster && interpreter == other.interpreter &&
           weight == other.weight && slant == other.slant && stemDarkening == other.stemDarkening;
  }
  bool operator!=(const TtfRenderProfile& other) const { return !(*this == other); }
};

freeink::font::FtFont::RenderOptions ttfRenderOptions(const TtfRenderProfile& profile);

class TtfRenderProfileStore : public PersistableStore<TtfRenderProfileStore> {
 private:
  struct Entry {
    std::string family;
    TtfRenderProfile profile;
  };

  static constexpr size_t MAX_PROFILES = 24;
  std::vector<Entry> profiles_;

  TtfRenderProfileStore() = default;
  friend class PersistableStore<TtfRenderProfileStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/ttf-rendering.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  TtfRenderProfile profileFor(const char* family) const;
  bool setProfile(const char* family, const TtfRenderProfile& profile);
  bool resetProfile(const char* family);
};

#define TTF_RENDER_PROFILES TtfRenderProfileStore::getInstance()

#endif
