/*
 * CrossInk Walnut-CGB bridge.
 * Walnut-CGB is vendored from the user-supplied walnut_cgb.h. Its license and
 * upstream/derived-work notices are retained in lib/WalnutCGB/src/walnut_cgb.h.
 */

/* The core is a large switch-based interpreter and this file is its only
 * translation unit. When the project builds for size (-Os, the PlatformIO
 * default for firmware sources) that costs this kind of code a good part of its
 * speed, so this one file is raised to -O2, trading some flash (the app slots
 * have plenty) for speed. A build already at -O2 or -O3 is left alone: GCC only
 * defines __OPTIMIZE_SIZE__ under -Os. Must stay above every #include so all
 * inline helpers share one optimisation level. Define WALNUT_CGB_KEEP_BUILD_OPT
 * to build with the project's flags regardless. */
#if !defined(WALNUT_CGB_KEEP_BUILD_OPT) && defined(__GNUC__) && !defined(__clang__) && defined(__OPTIMIZE_SIZE__)
#pragma GCC optimize("O2")
#endif

#include "walnut_cgb_port.h"
#include "walnut_cgb_color.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>

/* Fixed firmware configuration: CGB emulation enabled; audio omitted; CPU is little endian. */
#define WALNUT_GB_IS_LITTLE_ENDIAN 1
#define WALNUT_GB_RGB565_BIGENDIAN 0
#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define WALNUT_GB_12_COLOUR 1
#define WALNUT_FULL_GBC_SUPPORT 1

/* Peanut-GB and Walnut-CGB share public/internal symbol names. Namespace every
 * non-static Walnut symbol at preprocessing time to prevent duplicate symbols. */
#define __gb_read16 walnut_core_read16
#define __gb_read32 walnut_core_read32
#define __gb_read walnut_core_read
#define __gb_write walnut_core_write
#define __gb_write32 walnut_core_write32
#define __gb_write16 walnut_core_write16
#define __gb_execute_cb walnut_core_execute_cb
#define __gb_draw_line walnut_core_draw_line
#define __gb_step_cpu walnut_core_step_cpu
#define __gb_step_cpu_x walnut_core_step_cpu_x
#define gb_run_frame walnut_core_run_frame
#define gb_run_frame_dualfetch walnut_core_run_frame_dualfetch
#define gb_get_save_size_s walnut_core_get_save_size_s
#define gb_get_save_size walnut_core_get_save_size
#define gb_init_serial walnut_core_init_serial
#define gb_colour_hash walnut_core_colour_hash
#define gb_reset walnut_core_reset
#define gb_init walnut_core_init
#define gb_get_rom_name walnut_core_get_rom_name
#define gb_init_lcd walnut_core_init_lcd
#define gb_set_bootrom walnut_core_set_bootrom
#define gb_tick_rtc walnut_core_tick_rtc
#define gb_set_rtc walnut_core_set_rtc

#include <walnut_cgb.h>

/* Where the emulator state (struct gb_s: CPU registers, 32 KB WRAM, 16 KB VRAM,
 * IO, palettes, about 50 KB in all) lives. Every emulated instruction touches
 * it, and PSRAM is far slower than internal RAM and shares one cache and bus
 * with the ROM and both cores. It goes in internal RAM only while a game runs
 * and only when that leaves at least WALNUT_CGB_INTERNAL_RESERVE_BYTES of
 * internal heap free (and a contiguous block of the state's size plus
 * WALNUT_CGB_INTERNAL_BLOCK_MARGIN_BYTES to spare); otherwise it falls back to
 * PSRAM exactly as before. walnut_cgb_destroy() returns it when the game ends,
 * so nothing is held while idle. Define WALNUT_CGB_STATE_IN_PSRAM to always use
 * PSRAM. */
#ifndef WALNUT_CGB_INTERNAL_RESERVE_BYTES
#define WALNUT_CGB_INTERNAL_RESERVE_BYTES (128U * 1024U)
#endif
#ifndef WALNUT_CGB_INTERNAL_BLOCK_MARGIN_BYTES
#define WALNUT_CGB_INTERNAL_BLOCK_MARGIN_BYTES (16U * 1024U)
#endif

struct WalnutCgbContext {
  struct gb_s gb;
  const uint8_t *rom;
  size_t rom_size;
  uint8_t *cart_ram;
  size_t cart_ram_size;
  WalnutCgbLineCallback line_callback;
  WalnutCgbFatalCallback fatal_callback;
  void *user;
  WalnutCgbLuminanceCache luminance;
  int state_in_internal_ram;
  uint8_t grayscale_line[160];
};

static struct WalnutCgbContext *context_from_gb(struct gb_s *gb) {
  return gb ? (struct WalnutCgbContext *)gb->direct.priv : NULL;
}

/* ROM reads: the core calls these for every opcode and operand fetch. The
 * context is always set (walnut_core_init() stores it before its first ROM
 * read) and walnut_cgb_create() rejects a NULL ROM, so only the bound is
 * checked; anything past the end of the image reads as 0xFF as before. */
static uint8_t walnut_rom_read(struct gb_s *gb, const uint_fast32_t address) {
  const struct WalnutCgbContext *ctx = (const struct WalnutCgbContext *)gb->direct.priv;
  return address < ctx->rom_size ? ctx->rom[address] : 0xFF;
}

static uint16_t walnut_rom_read16(struct gb_s *gb, const uint_fast32_t address) {
  const struct WalnutCgbContext *ctx = (const struct WalnutCgbContext *)gb->direct.priv;
  if (address + 2U <= ctx->rom_size) {
    const uint8_t *p = ctx->rom + address;
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
  }
  return (uint16_t)(walnut_rom_read(gb, address) | (uint16_t)(walnut_rom_read(gb, address + 1U) << 8));
}

static uint32_t walnut_rom_read32(struct gb_s *gb, const uint_fast32_t address) {
  const struct WalnutCgbContext *ctx = (const struct WalnutCgbContext *)gb->direct.priv;
  if (address + 4U <= ctx->rom_size) {
    const uint8_t *p = ctx->rom + address;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }
  return (uint32_t)walnut_rom_read(gb, address) | ((uint32_t)walnut_rom_read(gb, address + 1U) << 8) |
         ((uint32_t)walnut_rom_read(gb, address + 2U) << 16) | ((uint32_t)walnut_rom_read(gb, address + 3U) << 24);
}

static uint8_t walnut_cart_ram_read(struct gb_s *gb, const uint_fast32_t address) {
  struct WalnutCgbContext *ctx = context_from_gb(gb);
  if (!ctx || !ctx->cart_ram || address >= ctx->cart_ram_size) return 0xFF;
  return ctx->cart_ram[address];
}

static void walnut_cart_ram_write(struct gb_s *gb, const uint_fast32_t address, const uint8_t value) {
  struct WalnutCgbContext *ctx = context_from_gb(gb);
  if (!ctx || !ctx->cart_ram || address >= ctx->cart_ram_size) return;
  ctx->cart_ram[address] = value;
}

static void walnut_lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
  struct WalnutCgbContext *ctx = context_from_gb(gb);
  if (!ctx || !pixels || line >= 144U || !ctx->line_callback) return;

  if (gb->cgb.cgbMode) {
    /* CGB pixels index Walnut's 64-entry RGB565 palette (BG then OBJ). Keep
     * continuous luminance here; dithering is applied after scaling. The
     * palette-to-luminance table is refreshed only when a palette changed. */
    walnut_cgb_luminance_cache_sync(&ctx->luminance, gb->cgb.fixPalette);
    walnut_cgb_convert_cgb_line(&ctx->luminance, pixels, ctx->grayscale_line);
  } else {
    /* DMG-compatible shade values use 0=white through 3=black. */
    walnut_cgb_convert_dmg_line(pixels, ctx->grayscale_line);
  }

  ctx->line_callback(ctx->user, ctx->grayscale_line, line);
}

static void walnut_core_error(struct gb_s *gb, const enum gb_error_e error, const uint16_t address) {
  struct WalnutCgbContext *ctx = context_from_gb(gb);
  if (ctx && ctx->fatal_callback) ctx->fatal_callback(ctx->user, (int)error, address);
  esp_restart();
  for (;;) { }
}

/* Allocates the zeroed emulator context: internal RAM when there is plenty to
 * spare (see the comment above the context), else PSRAM. */
static void *walnut_alloc_state(size_t size, int *in_internal_ram) {
  *in_internal_ram = 0;
#ifndef WALNUT_CGB_STATE_IN_PSRAM
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
          size + WALNUT_CGB_INTERNAL_RESERVE_BYTES &&
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
          size + WALNUT_CGB_INTERNAL_BLOCK_MARGIN_BYTES) {
    void *memory = heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (memory) {
      *in_internal_ram = 1;
      return memory;
    }
  }
#endif
  return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

WalnutCgbContext *walnut_cgb_create(const uint8_t *rom, size_t rom_size,
                                     WalnutCgbLineCallback line_callback,
                                     WalnutCgbFatalCallback fatal_callback,
                                     void *user, int *init_result) {
  struct WalnutCgbContext *ctx;
  size_t save_size = 0;
  enum gb_init_error_e result;
  int in_internal_ram = 0;

  if (init_result) *init_result = WALNUT_CGB_INIT_OUT_OF_MEMORY;
  if (!rom || rom_size < 0x150U || !line_callback) return NULL;

  ctx = (struct WalnutCgbContext *)walnut_alloc_state(sizeof(*ctx), &in_internal_ram);
  if (!ctx) return NULL;
  ctx->state_in_internal_ram = in_internal_ram;
  ctx->rom = rom;
  ctx->rom_size = rom_size;
  ctx->line_callback = line_callback;
  ctx->fatal_callback = fatal_callback;
  ctx->user = user;

  result = walnut_core_init(&ctx->gb, walnut_rom_read, walnut_rom_read16, walnut_rom_read32,
                            walnut_cart_ram_read, walnut_cart_ram_write, walnut_core_error, ctx);
  if (result != GB_INIT_NO_ERROR) {
    if (init_result) *init_result = (int)result;
    heap_caps_free(ctx);
    return NULL;
  }

  if (walnut_core_get_save_size_s(&ctx->gb, &save_size) < 0) {
    if (init_result) *init_result = WALNUT_CGB_INIT_INVALID_SAVE_SIZE;
    heap_caps_free(ctx);
    return NULL;
  }
  if (save_size > 0) {
    ctx->cart_ram = (uint8_t *)heap_caps_calloc(1, save_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->cart_ram) {
      if (init_result) *init_result = WALNUT_CGB_INIT_OUT_OF_MEMORY;
      heap_caps_free(ctx);
      return NULL;
    }
    ctx->cart_ram_size = save_size;
  }

  /* gb_reset initializes the CGB palette RAM to white but does not populate
   * the converted RGB565 cache until the game writes a palette register. Prime
   * all 64 cached entries so the first rendered frame is correct as well. */
  {
    size_t i;
    for (i = 0; i < 32U; ++i) {
      const uint16_t bg = (uint16_t)(ctx->gb.cgb.BGPalette[2U * i] |
                                     ((uint16_t)ctx->gb.cgb.BGPalette[2U * i + 1U] << 8));
      const uint16_t obj = (uint16_t)(ctx->gb.cgb.OAMPalette[2U * i] |
                                      ((uint16_t)ctx->gb.cgb.OAMPalette[2U * i + 1U] << 8));
      ctx->gb.cgb.fixPalette[i] = bgr555_to_rgb565_accurate(bg);
      ctx->gb.cgb.fixPalette[0x20U + i] = bgr555_to_rgb565_accurate(obj);
    }
  }

  walnut_core_init_lcd(&ctx->gb, walnut_lcd_draw_line);
  if (init_result) *init_result = WALNUT_CGB_INIT_NO_ERROR;
  return ctx;
}

void walnut_cgb_destroy(WalnutCgbContext *context) {
  if (!context) return;
  if (context->cart_ram) heap_caps_free(context->cart_ram);
  heap_caps_free(context);
}

size_t walnut_cgb_save_size(const WalnutCgbContext *context) {
  return context ? context->cart_ram_size : 0U;
}

uint8_t *walnut_cgb_save_ram(WalnutCgbContext *context) {
  return context ? context->cart_ram : NULL;
}

int walnut_cgb_state_in_internal_ram(const WalnutCgbContext *context) {
  return context ? context->state_in_internal_ram : 0;
}

void walnut_cgb_run_frame_ex(WalnutCgbContext *context, int draw_lines) {
  if (!context) return;
  /* The core's own frame-skip switch: __gb_draw_line() returns before touching
   * anything when frame_skip is set and frame_skip_count is clear. The core
   * flips frame_skip_count itself at every VBLANK, after all 144 lines of the
   * frame were (or were not) drawn, so it is set here for each frame. Drawing
   * has no effect on emulation: the line renderer only reads emulator state,
   * and the window line counter it advances is cleared at the first line of
   * every frame. */
  context->gb.direct.frame_skip = 1;
  context->gb.display.frame_skip_count = draw_lines ? 1 : 0;
  walnut_core_run_frame(&context->gb);
}

void walnut_cgb_run_frame(WalnutCgbContext *context) {
  walnut_cgb_run_frame_ex(context, 1);
}

void walnut_cgb_set_joypad(WalnutCgbContext *context, uint8_t active_low_joypad) {
  if (context) context->gb.direct.joypad = active_low_joypad;
}

void walnut_cgb_set_rtc(WalnutCgbContext *context, const struct tm *time_info) {
  if (context && time_info) walnut_core_set_rtc(&context->gb, time_info);
}

void walnut_cgb_get_rom_name(WalnutCgbContext *context, char *title) {
  if (context && title) walnut_core_get_rom_name(&context->gb, title);
}
