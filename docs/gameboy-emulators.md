# Game Boy / Game Boy Color integration

## ROM routing

- `.gb` routes to Peanut-GB (`src/peanut_gb_impl.c`).
- `.gbc` routes to Walnut-CGB (`src/walnut_cgb_impl.c`) via `GameBoyActivity` and its opaque C API in `src/walnut_cgb_port.h`.
- The file browser accepts both extensions. Extension matching is case-insensitive.

## Monochrome conversion for the e-ink panel

Walnut-CGB writes palette-index pixels on its LCD callback. In CGB mode, the low six bits select one of Walnut-CGB's 64 RGB565 palette entries (`gb->cgb.fixPalette`). The bridge converts RGB565 to 8-bit luminance using `Y = (299R + 587G + 114B) / 1000`, then applies a 2x2 Bayer threshold. The emitted shades follow the shared renderer's convention: `0` is white and `3` is black. Bright colors therefore map to white, dark colors to black, and midtones become an ordered black/white pattern. The same convention applies when DMG-compatible ROMs use their four shade values as luminance input.

The previous conversion returned `3` for bright pixels and `0` for dark pixels, reversing black and white in the renderer. The conversion helper and host smoke test now assert the correct polarity. The Game Boy render path scales each source pixel by nearest-neighbor expansion; at the native scale of 3x the 2x2 dither pattern is visible as a small ordered pattern.

## Why a C bridge is used

Peanut-GB and Walnut-CGB share C types and public symbol names (`struct gb_s`, `gb_init`, `gb_run_frame`, etc.) but have incompatible context layouts/API signatures. Walnut-CGB is compiled once in `src/walnut_cgb_impl.c` with its exported symbols renamed to `walnut_core_*`. CrossInk C++ code only sees the opaque `WalnutCgbContext` declared in `src/walnut_cgb_port.h`; do not include `walnut_cgb.h` in C++ activity files alongside `peanut_gb.h`.

## Source and manifests

`lib/WalnutCGB/src/walnut_cgb.h` is the user-supplied header, with a forward declaration of `struct gb_s` added before early callback prototypes so the implementation compiles as C. Both `lib/PeanutGB` and `lib/WalnutCGB` have `library.json` manifests for PlatformIO's library discovery.

## Testing

The host-side smoke test checks that a synthetic CGB ROM initializes, emits scanlines containing only valid shade values, and maps RGB565 black to shade `3` and white to shade `0`. The ESP32-S3 PlatformIO build and physical panel/touch test still need to be performed in a Windows/PlatformIO environment with CrossInk's `freeink-sdk` submodule initialized.
