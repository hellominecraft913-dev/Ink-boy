#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "walnut_cgb_port.h"
#include "walnut_cgb_color.h"

static unsigned lines;
static unsigned invalid_values;

static void draw(void *user, const uint8_t *pixels, uint_fast8_t line) {
  unsigned i;
  (void)user;
  assert(line < 144);
  for (i = 0; i < 160; ++i) {
    if (pixels[i] != 0 && pixels[i] != 3) ++invalid_values;
  }
  ++lines;
}

static void die(void *user, int code, uint16_t address) {
  (void)user;
  fprintf(stderr, "Walnut-CGB fatal error=%d at 0x%04X\n", code, address);
  abort();
}

static uint8_t checksum(const uint8_t *rom) {
  uint8_t sum = 0;
  size_t i;
  for (i = 0x134; i <= 0x14C; ++i) sum = (uint8_t)(sum - rom[i] - 1U);
  return sum;
}

static void test_color_conversion(void) {
  static const uint16_t samples[] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x8410, 0x7BEF};
  size_t i;
  unsigned y;
  unsigned x;
  for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    for (y = 0; y < 2; ++y) {
      for (x = 0; x < 2; ++x) {
        uint8_t pixel = walnut_cgb_rgb565_to_bw(samples[i], x, y);
        assert(pixel == 0 || pixel == 3);
      }
    }
  }
  assert(walnut_cgb_rgb565_to_luminance(0xF800) == 76);  /* red */
  assert(walnut_cgb_rgb565_to_luminance(0x07E0) == 150); /* green */
  assert(walnut_cgb_rgb565_to_luminance(0x001F) == 29);  /* blue */
  /* Exhaust every RGB565 input and each threshold phase: output is always BW. */
  {
    uint32_t color;
    for (color = 0; color <= 0xFFFFU; ++color) {
      for (y = 0; y < 2; ++y) {
        for (x = 0; x < 2; ++x) {
          const uint8_t pixel = walnut_cgb_rgb565_to_bw((uint16_t)color, x, y);
          assert(pixel == 0 || pixel == 3);
        }
      }
    }
  }
  for (y = 0; y < 2; ++y) {
    for (x = 0; x < 2; ++x) {
      assert(walnut_cgb_rgb565_to_bw(0x0000, x, y) == 0);
      assert(walnut_cgb_rgb565_to_bw(0xFFFF, x, y) == 3);
    }
  }
  /* Mid-gray should use the ordered pattern, not pass gray through. */
  assert(walnut_cgb_rgb565_to_bw(0x8410, 0, 0) == 3);
  assert(walnut_cgb_rgb565_to_bw(0x8410, 1, 0) == 0);
  assert(walnut_cgb_rgb565_to_bw(0x8410, 0, 1) == 0);
  assert(walnut_cgb_rgb565_to_bw(0x8410, 1, 1) == 3);
}

int main(void) {
  const size_t rom_size = 0x8000;
  uint8_t *rom = (uint8_t *)calloc(1, rom_size);
  WalnutCgbContext *ctx;
  int init_result = -99;
  size_t i;

  assert(rom);
  test_color_conversion();

  /* A synthetic valid 32 KiB CGB ROM that loops at the entry point. */
  rom[0x100] = 0x18;
  rom[0x101] = 0xFE;
  memset(rom + 0x134, ' ', 0x10);
  memcpy(rom + 0x134, "TEST COLOR", 10);
  rom[0x143] = 0x80; /* CGB-supported cartridge */
  rom[0x147] = 0x00; /* ROM only */
  rom[0x148] = 0x00; /* 32 KiB */
  rom[0x149] = 0x00; /* no external RAM */
  rom[0x14D] = checksum(rom);

  ctx = walnut_cgb_create(rom, rom_size, draw, die, NULL, &init_result);
  if (!ctx) {
    fprintf(stderr, "Walnut-CGB create failed: %d\n", init_result);
    free(rom);
    return 2;
  }

  walnut_cgb_set_joypad(ctx, 0xFF);
  for (i = 0; i < 3; ++i) walnut_cgb_run_frame(ctx);
  assert(lines > 0);
  assert(invalid_values == 0);
  printf("Walnut-CGB smoke OK: %u scanlines, output strictly black/white, save=%zu\n",
         lines, walnut_cgb_save_size(ctx));
  walnut_cgb_destroy(ctx);
  free(rom);
  return 0;
}
