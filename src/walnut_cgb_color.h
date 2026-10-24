#pragma once

#include <stdint.h>

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
