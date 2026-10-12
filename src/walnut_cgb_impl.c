/*
 * CrossInk Walnut-CGB bridge.
 * Walnut-CGB is vendored from the user-supplied walnut_cgb.h. Its license and
 * upstream/derived-work notices are retained in lib/WalnutCGB/src/walnut_cgb.h.
 */
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

struct WalnutCgbContext {
  struct gb_s gb;
  const uint8_t *rom;
  size_t rom_size;
  uint8_t *cart_ram;
  size_t cart_ram_size;
  WalnutCgbLineCallback line_callback;
  WalnutCgbFatalCallback fatal_callback;
  void *user;
  uint8_t monochrome_line[160];
};

static struct WalnutCgbContext *context_from_gb(struct gb_s *gb) {
  return gb ? (struct WalnutCgbContext *)gb->direct.priv : NULL;
}

static uint8_t walnut_rom_byte(struct WalnutCgbContext *ctx, uint_fast32_t address) {
  if (!ctx || !ctx->rom || address >= ctx->rom_size) return 0xFF;
  return ctx->rom[address];
}

static uint8_t walnut_rom_read(struct gb_s *gb, const uint_fast32_t address) {
  return walnut_rom_byte(context_from_gb(gb), address);
}

static uint16_t walnut_rom_read16(struct gb_s *gb, const uint_fast32_t address) {
  const uint16_t lo = walnut_rom_byte(context_from_gb(gb), address);
  const uint16_t hi = walnut_rom_byte(context_from_gb(gb), address + 1U);
  return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static uint32_t walnut_rom_read32(struct gb_s *gb, const uint_fast32_t address) {
  const uint32_t b0 = walnut_rom_byte(context_from_gb(gb), address);
  const uint32_t b1 = walnut_rom_byte(context_from_gb(gb), address + 1U);
  const uint32_t b2 = walnut_rom_byte(context_from_gb(gb), address + 2U);
  const uint32_t b3 = walnut_rom_byte(context_from_gb(gb), address + 3U);
  return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
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
  uint_fast8_t x;
  if (!ctx || !pixels || line >= 144U || !ctx->line_callback) return;

  for (x = 0; x < 160U; ++x) {
    uint8_t luminance;
    if (gb->cgb.cgbMode) {
      /* In CGB mode Walnut's line pixels index its 64-entry RGB565 palette:
       * 0..31 are background palettes; 32..63 are object palettes. */
      const uint8_t palette_index = (uint8_t)(pixels[x] & 0x3FU);
      ctx->monochrome_line[x] = walnut_cgb_rgb565_to_bw(gb->cgb.fixPalette[palette_index], x, line);
      continue;
    } else {
      /* DMG-compatible carts use four logical palette shades in the low bits. */
      luminance = (uint8_t)(((uint16_t)(pixels[x] & 0x03U) * 255U) / 3U);
    }
    ctx->monochrome_line[x] = walnut_cgb_luminance_to_bw(luminance, x, line);
  }

  ctx->line_callback(ctx->user, ctx->monochrome_line, line);
}

static void walnut_core_error(struct gb_s *gb, const enum gb_error_e error, const uint16_t address) {
  struct WalnutCgbContext *ctx = context_from_gb(gb);
  if (ctx && ctx->fatal_callback) ctx->fatal_callback(ctx->user, (int)error, address);
  esp_restart();
  for (;;) { }
}

WalnutCgbContext *walnut_cgb_create(const uint8_t *rom, size_t rom_size,
                                     WalnutCgbLineCallback line_callback,
                                     WalnutCgbFatalCallback fatal_callback,
                                     void *user, int *init_result) {
  struct WalnutCgbContext *ctx;
  size_t save_size = 0;
  enum gb_init_error_e result;

  if (init_result) *init_result = WALNUT_CGB_INIT_OUT_OF_MEMORY;
  if (!rom || rom_size < 0x150U || !line_callback) return NULL;

  ctx = (struct WalnutCgbContext *)heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ctx) return NULL;
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

void walnut_cgb_run_frame(WalnutCgbContext *context) {
  if (context) walnut_core_run_frame(&context->gb);
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
