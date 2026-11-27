#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WalnutCgbContext WalnutCgbContext;

typedef void (*WalnutCgbLineCallback)(void *user, const uint8_t *grayscale_luminance, uint_fast8_t line);
typedef void (*WalnutCgbFatalCallback)(void *user, int error_code, uint16_t address);

enum {
  WALNUT_CGB_INIT_NO_ERROR = 0,
  WALNUT_CGB_INIT_CARTRIDGE_UNSUPPORTED = 1,
  WALNUT_CGB_INIT_INVALID_CHECKSUM = 2,
  WALNUT_CGB_INIT_OUT_OF_MEMORY = -1,
  WALNUT_CGB_INIT_INVALID_SAVE_SIZE = -2
};

WalnutCgbContext *walnut_cgb_create(const uint8_t *rom, size_t rom_size,
                                     WalnutCgbLineCallback line_callback,
                                     WalnutCgbFatalCallback fatal_callback,
                                     void *user, int *init_result);
void walnut_cgb_destroy(WalnutCgbContext *context);
size_t walnut_cgb_save_size(const WalnutCgbContext *context);
uint8_t *walnut_cgb_save_ram(WalnutCgbContext *context);

/* Emulates one frame and calls the line callback for every scanline. */
void walnut_cgb_run_frame(WalnutCgbContext *context);

/* Emulates one frame. With draw_lines == 0 the CPU, timers, DMA and LCD
 * registers run exactly as usual but the 144 scanlines are not composed and
 * the line callback is not called: for frames nobody will look at. */
void walnut_cgb_run_frame_ex(WalnutCgbContext *context, int draw_lines);

/* Non-zero when the emulator state was placed in internal RAM (faster than
 * PSRAM); zero when it fell back to PSRAM. For logging. */
int walnut_cgb_state_in_internal_ram(const WalnutCgbContext *context);

void walnut_cgb_set_joypad(WalnutCgbContext *context, uint8_t active_low_joypad);
void walnut_cgb_set_rtc(WalnutCgbContext *context, const struct tm *time_info);
void walnut_cgb_get_rom_name(WalnutCgbContext *context, char *title);

#ifdef __cplusplus
}
#endif
