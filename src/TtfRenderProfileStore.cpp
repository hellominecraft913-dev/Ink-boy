#include "TtfRenderProfileStore.h"

#if CROSSINK_SCALABLE_FONTS

#include <algorithm>
#include <cstring>

namespace {
constexpr int32_t kWeights26_6[] = {-32, -16, 0, 16, 32, 48, 64};
// Fixed-point shear values roughly corresponding to 0, 4, 8, and 14 degrees.
constexpr int32_t kSlants16_16[] = {0, 4583, 9209, 16384};

TtfRenderProfile sanitized(TtfRenderProfile profile) {
  profile.hinting = std::min<uint8_t>(profile.hinting, 3);
  profile.raster = std::min<uint8_t>(profile.raster, 1);
  profile.interpreter = std::min<uint8_t>(profile.interpreter, 2);
  profile.weight = std::min<uint8_t>(profile.weight, 6);
  profile.slant = std::min<uint8_t>(profile.slant, 3);
  if (profile.hinting != 1) profile.interpreter = 0;
  return profile;
}
}  // namespace

freeink::font::FtFont::RenderOptions ttfRenderOptions(const TtfRenderProfile& rawProfile) {
  const TtfRenderProfile profile = sanitized(rawProfile);
  freeink::font::FtFont::RenderOptions options;
  switch (profile.hinting) {
    case 0:
      options.hinting = freeink::font::FtFont::HintingMode::None;
      break;
    case 1:
      options.hinting = freeink::font::FtFont::HintingMode::Native;
      break;
    case 3:
      options.hinting = freeink::font::FtFont::HintingMode::Light;
      break;
    case 2:
    default:
      options.hinting = freeink::font::FtFont::HintingMode::Auto;
      break;
  }
  options.monochrome = profile.raster != 0;
  options.interpreterVersion = profile.interpreter == 1 ? 35 : 40;
  options.embolden26_6 = kWeights26_6[profile.weight];
  options.slant16_16 = kSlants16_16[profile.slant];
  options.stemDarkening = profile.stemDarkening;
  return options;
}

void TtfRenderProfileStore::toJson(JsonDocument& doc) const {
  JsonArray profiles = doc["profiles"].to<JsonArray>();
  for (const auto& entry : profiles_) {
    JsonObject value = profiles.add<JsonObject>();
    value["family"] = entry.family;
    value["hinting"] = entry.profile.hinting;
    value["raster"] = entry.profile.raster;
    value["interpreter"] = entry.profile.interpreter;
    value["weight"] = entry.profile.weight;
    value["slant"] = entry.profile.slant;
    value["stemDarkening"] = entry.profile.stemDarkening;
  }
}

bool TtfRenderProfileStore::fromJson(JsonVariantConst doc) {
  profiles_.clear();
  const JsonArrayConst profiles = doc["profiles"].as<JsonArrayConst>();
  profiles_.reserve(std::min(profiles.size(), MAX_PROFILES));
  for (const JsonObjectConst value : profiles) {
    if (profiles_.size() >= MAX_PROFILES) break;
    const char* family = value["family"] | "";
    if (!family[0]) continue;
    TtfRenderProfile profile;
    profile.hinting = value["hinting"] | profile.hinting;
    profile.raster = value["raster"] | profile.raster;
    profile.interpreter = value["interpreter"] | profile.interpreter;
    profile.weight = value["weight"] | profile.weight;
    profile.slant = value["slant"] | profile.slant;
    profile.stemDarkening = value["stemDarkening"] | profile.stemDarkening;
    profiles_.push_back({family, sanitized(profile)});
  }
  return true;
}

TtfRenderProfile TtfRenderProfileStore::profileFor(const char* family) const {
  ensureLoaded();
  if (!family || !family[0]) return {};
  const auto found =
      std::find_if(profiles_.begin(), profiles_.end(), [family](const Entry& entry) { return entry.family == family; });
  return found == profiles_.end() ? TtfRenderProfile{} : found->profile;
}

bool TtfRenderProfileStore::setProfile(const char* family, const TtfRenderProfile& rawProfile) {
  ensureLoaded();
  if (!family || !family[0]) return false;
  const TtfRenderProfile profile = sanitized(rawProfile);
  const auto found =
      std::find_if(profiles_.begin(), profiles_.end(), [family](const Entry& entry) { return entry.family == family; });
  if (profile == TtfRenderProfile{}) {
    if (found == profiles_.end()) return true;
    const size_t index = static_cast<size_t>(found - profiles_.begin());
    Entry removed = std::move(*found);
    profiles_.erase(found);
    if (saveToFile()) return true;
    profiles_.insert(profiles_.begin() + index, std::move(removed));
    return false;
  } else if (found != profiles_.end()) {
    if (found->profile == profile) return true;
    const TtfRenderProfile previous = found->profile;
    found->profile = profile;
    if (saveToFile()) return true;
    found->profile = previous;
    return false;
  } else {
    if (profiles_.size() >= MAX_PROFILES) return false;
    profiles_.push_back({family, profile});
    if (saveToFile()) return true;
    profiles_.pop_back();
    return false;
  }
}

bool TtfRenderProfileStore::resetProfile(const char* family) { return setProfile(family, {}); }

#endif
