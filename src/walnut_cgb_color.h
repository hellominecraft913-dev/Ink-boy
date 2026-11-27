#pragma once

#include <stdint.h>
#include <string.h>

/* Convert Walnut-CGB's RGB565 palette to 8-bit luminance: 0=black, 255=white. */
static inline uint8_t walnut_cgb_rgb565_to_luminance(uint16_t color) {
  const uint8_t red5 = (uint8_t)((color >> 11) & 0x1FU);
  const uint8_t green6 = (uint8_t)((color >> 5) & 0x3FU);
  const uint8_t blue5 = (uint8_t)(color & 0x1FU);
  const uint16_t red = (uint16_t)((red5 << 3) | (red5 >> 2));
  const uint16_t green = (uint16_t)((green6 << 2) | (green6 >> 4));
  const uint16_t blue = (uint16_t)((blue5 << 3) | (blue5 >> 2));
  return (uint8_t)((299U * red + 587U * green + 114U * blue + 500U) / 1000U);
}

/* Walnut's DMG-compatible palette values follow GameBoyActivity's shade order:
 * 0=white, 1=light grey, 2=dark grey, 3=black. */
static inline uint8_t walnut_cgb_dmg_shade_to_luminance(uint8_t shade) {
  return (uint8_t)(255U - ((uint16_t)(shade & 0x03U) * 85U));
}

/* Luminance of Walnut's 64-entry RGB565 palette cache (32 BG then 32 OBJ).
 *
 * A scanline has 160 pixels but the palette has only 64 colours, and it changes
 * rarely, so converting once per palette entry replaces three multiplies and a
 * divide per pixel with one table load. `source` remembers which RGB565 value
 * each `luminance` entry was computed from; a zero-initialised cache is already
 * consistent because RGB565 0x0000 converts to luminance 0. */
typedef struct WalnutCgbLuminanceCache {
  uint16_t source[64];
  uint8_t luminance[64];
} WalnutCgbLuminanceCache;

/* Brings the cache up to date with `palette` (64 RGB565 values). The common
 * case, an unchanged palette, is one 128-byte compare. */
static inline void walnut_cgb_luminance_cache_sync(WalnutCgbLuminanceCache *cache, const uint16_t *palette) {
  unsigned i;
  if (memcmp(palette, cache->source, sizeof(cache->source)) == 0) return;
  for (i = 0; i < 64U; ++i) {
    if (palette[i] != cache->source[i]) {
      cache->source[i] = palette[i];
      cache->luminance[i] = walnut_cgb_rgb565_to_luminance(palette[i]);
    }
  }
}

/* CGB scanline: each pixel is an index (low 6 bits) into the palette cache. */
static inline void walnut_cgb_convert_cgb_line(const WalnutCgbLuminanceCache *cache, const uint8_t *__restrict pixels,
                                               uint8_t *__restrict out) {
  const uint8_t *__restrict luminance = cache->luminance;
  unsigned x;
  for (x = 0; x < 160U; ++x) out[x] = luminance[pixels[x] & 0x3FU];
}

/* DMG-compatible scanline (a DMG cartridge run by the CGB core). */
static inline void walnut_cgb_convert_dmg_line(const uint8_t *__restrict pixels, uint8_t *__restrict out) {
  static const uint8_t kShadeLuminance[4] = {255U, 170U, 85U, 0U};
  unsigned x;
  for (x = 0; x < 160U; ++x) out[x] = kShadeLuminance[pixels[x] & 0x03U];
}
