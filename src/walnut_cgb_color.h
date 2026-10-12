#pragma once

#include <stdint.h>

/* Convert the RGB565 palette values used by Walnut-CGB to display luminance. */
static inline uint8_t walnut_cgb_rgb565_to_luminance(uint16_t color) {
  const uint8_t red5 = (uint8_t)((color >> 11) & 0x1FU);
  const uint8_t green6 = (uint8_t)((color >> 5) & 0x3FU);
  const uint8_t blue5 = (uint8_t)(color & 0x1FU);
  const uint16_t red = (uint16_t)((red5 << 3) | (red5 >> 2));
  const uint16_t green = (uint16_t)((green6 << 2) | (green6 >> 4));
  const uint16_t blue = (uint16_t)((blue5 << 3) | (blue5 >> 2));
  return (uint8_t)((299U * red + 587U * green + 114U * blue + 500U) / 1000U);
}

/*
 * Ordered 2x2 Bayer dithering. The only possible outputs are 0 (black) and
 * 3 (white), matching GameBoyActivity's bit-packed monochrome framebuffer.
 */
static inline uint8_t walnut_cgb_luminance_to_bw(uint8_t luminance, uint_fast8_t x, uint_fast8_t y) {
  static const uint8_t bayer[2][2] = {{0, 2}, {3, 1}};
  const uint16_t threshold = (uint16_t)(((uint16_t)(2U * bayer[y & 1U][x & 1U] + 1U) * 255U) / 8U);
  return luminance > threshold ? 3U : 0U;
}

static inline uint8_t walnut_cgb_rgb565_to_bw(uint16_t color, uint_fast8_t x, uint_fast8_t y) {
  return walnut_cgb_luminance_to_bw(walnut_cgb_rgb565_to_luminance(color), x, y);
}
