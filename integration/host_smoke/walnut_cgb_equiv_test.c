/*
 * Host checks for the Walnut-CGB bridge speedups. Included as one translation
 * unit with the bridge so it can see the emulator state.
 *
 *  1. The palette-to-luminance table and the DMG shade table give exactly the
 *     values of the per-pixel conversions they replaced, for every colour.
 *  2. Skipping scanline composition changes nothing about emulation: a run
 *     that composes only some frames ends every frame with byte-identical
 *     emulator state (apart from the frame-skip switch itself) to a run that
 *     composes all of them, and the frames it does compose match pixel for
 *     pixel.
 *  3. Where the state is allocated follows the free-memory rules, falls back
 *     to PSRAM, and nothing leaks on success, init failure or destroy.
 */
#include "../../src/walnut_cgb_impl.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "cgb_test_rom.h"

static uint8_t frame_a[144][160];
static uint8_t frame_b[144][160];
static int lines_b;
static int drew_a;

static void draw_a(void *user, const uint8_t *px, uint_fast8_t line) {
  (void)user;
  memcpy(frame_a[line], px, 160);
  ++drew_a;
}
static void draw_b(void *user, const uint8_t *px, uint_fast8_t line) {
  (void)user;
  memcpy(frame_b[line], px, 160);
  ++lines_b;
}
static void die(void *user, int code, uint16_t address) {
  (void)user;
  fprintf(stderr, "fatal core error %d at 0x%04X\n", code, address);
  abort();
}

static void test_luminance_tables(void) {
  uint32_t color;
  unsigned i, shade;
  WalnutCgbLuminanceCache cache;
  uint16_t palette[64];
  uint8_t out[160], in[160];
  uint32_t seed = 1;

  memset(&cache, 0, sizeof(cache));
  /* Every RGB565 value through every palette slot, sixty-four at a time. */
  for (color = 0; color < 0x10000U; color += 64U) {
    for (i = 0; i < 64U; ++i) palette[i] = (uint16_t)(color + i);
    walnut_cgb_luminance_cache_sync(&cache, palette);
    for (i = 0; i < 64U; ++i) {
      assert(cache.luminance[i] == walnut_cgb_rgb565_to_luminance(palette[i]));
      assert(cache.source[i] == palette[i]);
    }
  }
  /* A palette that changes a single entry between frames. */
  for (i = 0; i < 64U; ++i) palette[i] = (uint16_t)(i * 1013U);
  walnut_cgb_luminance_cache_sync(&cache, palette);
  palette[17] = 0xF81F;
  walnut_cgb_luminance_cache_sync(&cache, palette);
  for (i = 0; i < 64U; ++i) assert(cache.luminance[i] == walnut_cgb_rgb565_to_luminance(palette[i]));
  /* A zeroed cache is already consistent for an all-black palette. */
  memset(&cache, 0, sizeof(cache));
  memset(palette, 0, sizeof(palette));
  walnut_cgb_luminance_cache_sync(&cache, palette);
  for (i = 0; i < 64U; ++i) assert(cache.luminance[i] == 0 && walnut_cgb_rgb565_to_luminance(0) == 0);

  /* Line converters, with the upper two bits of the index set as the core can. */
  for (i = 0; i < 160U; ++i) {
    seed = seed * 1664525U + 1013904223U;
    in[i] = (uint8_t)(seed >> 24);
  }
  walnut_cgb_convert_cgb_line(&cache, in, out);
  for (i = 0; i < 160U; ++i) assert(out[i] == walnut_cgb_rgb565_to_luminance(palette[in[i] & 0x3FU]));
  walnut_cgb_convert_dmg_line(in, out);
  for (i = 0; i < 160U; ++i) assert(out[i] == walnut_cgb_dmg_shade_to_luminance(in[i]));
  for (shade = 0; shade < 4U; ++shade) assert(walnut_cgb_dmg_shade_to_luminance((uint8_t)shade) == 255U - shade * 85U);
}

/* Everything except the parts that legitimately differ between two contexts:
 * the draw callback, the frame-skip switches, the window line counter (it only
 * feeds the scanline renderer and is cleared at line 0 of every frame) and the
 * pointer back to the context. */
static void normalise(struct gb_s *dst, const struct gb_s *src) {
  memcpy(dst, src, sizeof(*dst));
  memset(&dst->display, 0, sizeof(dst->display));
  dst->direct.frame_skip = 0;
  dst->direct.priv = NULL;
}

static void test_skip_equivalence(void) {
  const int frames = 900;
  int f, compared = 0;
  uint32_t seed = 7;
  WalnutCgbContext *a, *b;
  int r1 = -1, r2 = -1;
  struct gb_s na, nb;

  memset(frame_a, 0, sizeof(frame_a));  /* line 0 is not composed on frame 0: start both from blank */
  memset(frame_b, 0, sizeof(frame_b));
  a = walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), draw_a, die, NULL, &r1);
  b = walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), draw_b, die, NULL, &r2);
  assert(a && b && r1 == 0 && r2 == 0);
  assert(a->gb.cgb.cgbMode == 1);

  for (f = 0; f < frames; ++f) {
    int draw;
    seed = seed * 1664525U + 1013904223U;
    /* Mostly skip; draw about one frame in eight, and always the first few. */
    draw = (f < 3) || ((seed >> 24) % 8U == 0U);
    drew_a = 0;
    lines_b = 0;
    walnut_cgb_set_joypad(a, 0xFF);
    walnut_cgb_set_joypad(b, 0xFF);
    walnut_cgb_run_frame_ex(a, 1);
    walnut_cgb_run_frame_ex(b, draw);

    normalise(&na, &a->gb);
    normalise(&nb, &b->gb);
    if (memcmp(&na, &nb, sizeof(na)) != 0) {
      size_t i = 0;
      while (i < sizeof(na) && ((uint8_t *)&na)[i] == ((uint8_t *)&nb)[i]) ++i;
      fprintf(stderr, "emulator state diverged at frame %d, byte offset %zu of %zu\n", f, i, sizeof(na));
      abort();
    }
    /* The core itself composes 143 lines on its very first frame after power-on
     * (line 0 is not reached); every later frame has 144. Unchanged by this work. */
    assert(drew_a == (f == 0 ? 143 : 144));
    if (draw) {
      assert(lines_b == drew_a);
      if (memcmp(frame_a, frame_b, sizeof(frame_a)) != 0) {
        fprintf(stderr, "composed frame %d differs from the always-composed run\n", f);
        abort();
      }
      ++compared;
    } else {
      assert(lines_b == 0);
    }
  }
  /* The ROM must really be doing things: moving scroll, palettes, double speed. */
  assert(a->gb.cgb.doubleSpeed == 1);
  assert(a->gb.hram_io[IO_IE] == 0x01 && a->gb.gb_ime);  /* still servicing VBlank interrupts */
  printf("skip equivalence OK: %d frames, %d composed and identical, state identical every frame\n", frames, compared);
  walnut_cgb_destroy(a);
  walnut_cgb_destroy(b);
}

static void test_screen_is_not_blank(void) {
  /* Guard against a test ROM that silently does nothing: the picture must
   * contain several different luminances and change between frames. */
  WalnutCgbContext *a;
  int r = -1, f, differ = 0;
  uint8_t seen[256] = {0};
  int distinct = 0, i;
  uint8_t prev[144][160];
  a = walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), draw_a, die, NULL, &r);
  assert(a);
  memset(prev, 0, sizeof(prev));
  for (f = 0; f < 200; ++f) {
    walnut_cgb_run_frame(a);
    if (memcmp(prev, frame_a, sizeof(prev)) != 0) ++differ;
    memcpy(prev, frame_a, sizeof(prev));
  }
  for (i = 0; i < 144 * 160; ++i) seen[((uint8_t *)frame_a)[i]] = 1;
  for (i = 0; i < 256; ++i) distinct += seen[i];
  assert(differ > 150);
  assert(distinct > 8);
  printf("test ROM sanity OK: %d/200 frames changed, %d distinct luminances in the last one\n", differ, distinct);
  walnut_cgb_destroy(a);
}

static void test_allocation(void) {
  WalnutCgbContext *c;
  int r = -1;
  const size_t need = sizeof(struct WalnutCgbContext);
  uint8_t bad_rom[0x8000];

  stub_live_allocs = stub_internal_allocs = stub_psram_allocs = 0;

  /* Plenty of internal RAM: state lands there. */
  stub_internal_free = need + WALNUT_CGB_INTERNAL_RESERVE_BYTES;
  stub_internal_largest = need + WALNUT_CGB_INTERNAL_BLOCK_MARGIN_BYTES;
  stub_internal_alloc_fails = 0;
  c = walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), draw_a, die, NULL, &r);
  assert(c && walnut_cgb_state_in_internal_ram(c) == 1 && stub_internal_allocs == 1);
  walnut_cgb_destroy(c);
  assert(stub_live_allocs == 0);

  /* One byte short on total free heap: PSRAM. */
  stub_internal_free = need + WALNUT_CGB_INTERNAL_RESERVE_BYTES - 1U;
  c = walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), draw_a, die, NULL, &r);
  assert(c && walnut_cgb_state_in_internal_ram(c) == 0 && stub_psram_allocs >= 1);
  walnut_cgb_destroy(c);
  assert(stub_live_allocs == 0);

  /* Plenty free overall but fragmented (no big enough block): PSRAM. */
  stub_internal_free = 1024U * 1024U;
  stub_internal_largest = need + WALNUT_CGB_INTERNAL_BLOCK_MARGIN_BYTES - 1U;
  c = walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), draw_a, die, NULL, &r);
  assert(c && walnut_cgb_state_in_internal_ram(c) == 0);
  walnut_cgb_destroy(c);

  /* The check passes but the allocation itself fails: PSRAM, still works. */
  stub_internal_largest = 512U * 1024U;
  stub_internal_alloc_fails = 1;
  c = walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), draw_a, die, NULL, &r);
  assert(c && walnut_cgb_state_in_internal_ram(c) == 0 && r == WALNUT_CGB_INIT_NO_ERROR);
  walnut_cgb_run_frame(c);
  walnut_cgb_destroy(c);
  stub_internal_alloc_fails = 0;
  assert(stub_live_allocs == 0);

  /* Init failure (bad checksum) frees what it allocated, in either memory. */
  memcpy(bad_rom, kCgbTestRom, sizeof(bad_rom));
  bad_rom[0x14D] ^= 0xFF;
  c = walnut_cgb_create(bad_rom, sizeof(bad_rom), draw_a, die, NULL, &r);
  assert(!c && r == WALNUT_CGB_INIT_INVALID_CHECKSUM && stub_live_allocs == 0);
  stub_internal_alloc_fails = 1;
  c = walnut_cgb_create(bad_rom, sizeof(bad_rom), draw_a, die, NULL, &r);
  assert(!c && stub_live_allocs == 0);
  stub_internal_alloc_fails = 0;

  /* Argument checks are unchanged. */
  assert(!walnut_cgb_create(NULL, 0x8000, draw_a, die, NULL, &r));
  assert(!walnut_cgb_create(kCgbTestRom, 0x10, draw_a, die, NULL, &r));
  assert(!walnut_cgb_create(kCgbTestRom, sizeof(kCgbTestRom), NULL, die, NULL, &r));
  assert(stub_live_allocs == 0);
  walnut_cgb_destroy(NULL);
  assert(walnut_cgb_state_in_internal_ram(NULL) == 0);
  printf("allocation rules OK (state is %zu bytes; internal RAM needs %zu free + %zu reserve)\n", need, need,
         (size_t)WALNUT_CGB_INTERNAL_RESERVE_BYTES);
}

int main(void) {
  test_luminance_tables();
  printf("luminance tables OK (all 65536 colours, line converters)\n");
  test_screen_is_not_blank();
  test_skip_equivalence();
  test_allocation();
  return 0;
}
