#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "walnut_cgb_port.h"
#include "walnut_cgb_color.h"

static unsigned lines;

static void draw(void *user, const uint8_t *pixels, uint_fast8_t line) {
  (void)user;
  (void)pixels; /* 8-bit luminance, 0 = black; the value ranges are checked in walnut_cgb_equiv_test.c */
  assert(line < 144);
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
  /* Luminance of the primaries and the extremes, and the DMG shade ramp. */
  assert(walnut_cgb_rgb565_to_luminance(0x0000) == 0);
  assert(walnut_cgb_rgb565_to_luminance(0xFFFF) == 255);
  assert(walnut_cgb_rgb565_to_luminance(0xF800) == 76);  /* red */
  assert(walnut_cgb_rgb565_to_luminance(0x07E0) == 150); /* green */
  assert(walnut_cgb_rgb565_to_luminance(0x001F) == 29);  /* blue */
  assert(walnut_cgb_dmg_shade_to_luminance(0) == 255);
  assert(walnut_cgb_dmg_shade_to_luminance(3) == 0);
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
  printf("Walnut-CGB smoke OK: %u scanlines, save=%zu\n",
         lines, walnut_cgb_save_size(ctx));
  walnut_cgb_destroy(ctx);
  free(rom);
  return 0;
}
