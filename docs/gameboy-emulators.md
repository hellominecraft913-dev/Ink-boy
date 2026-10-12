# Game Boy / Game Boy Color integration

## ROM routing

- `.gb` routes to Peanut-GB (`src/peanut_gb_impl.c`).
- `.gbc` routes to Walnut-CGB (`src/walnut_cgb_impl.c`) via `GameBoyActivity` and its opaque C API in `src/walnut_cgb_port.h`.
- The file browser accepts both extensions. Extension matching is case-insensitive.

## Monochrome conversion for the e-ink panel

Walnut-CGB writes palette-index pixels on its LCD callback. In CGB mode, the low six bits select one of Walnut-CGB's 64 RGB565 palette entries (`gb->cgb.fixPalette`). The bridge converts RGB565 to 8-bit luminance using `Y = (299R + 587G + 114B) / 1000`, then applies a 2x2 Bayer threshold. Every emitted pixel is exactly `0` (black) or `3` (white), matching the GameBoyActivity framebuffer convention and ensuring color data never reaches the 1-bit CrossInk panel framebuffer. The 2x2 pattern preserves approximate average brightness at the cost of some visible stippling. DMG-compatible ROMs opened with the CGB core use their four shade values as luminance input to the same monochrome dither.

The Game Boy render path scales each source pixel by nearest-neighbor expansion; at the native scale of 3x the 2x2 dither pattern is visible as a small ordered pattern.

## Why a C bridge is used

Peanut-GB and Walnut-CGB share C types and public symbol names (`struct gb_s`, `gb_init`, `gb_run_frame`, etc.) but have incompatible context layouts/API signatures. Walnut-CGB is compiled once in `src/walnut_cgb_impl.c` with its exported symbols renamed to `walnut_core_*`. CrossInk C++ code only sees the opaque `WalnutCgbContext` declared in `src/walnut_cgb_port.h`; do not include `walnut_cgb.h` in C++ activity files alongside `peanut_gb.h`.

## Source and manifests

`lib/WalnutCGB/src/walnut_cgb.h` is the user-supplied header, with a forward declaration of `struct gb_s` added before early callback prototypes so the implementation compiles as C. Both `lib/PeanutGB` and `lib/WalnutCGB` have `library.json` manifests for PlatformIO's library discovery.

## Testing

A host-side smoke test should validate that a synthetic CGB ROM initializes, frame callbacks arrive, and every callback pixel is either `0` or `3`. The ESP32-S3 PlatformIO build and physical panel/touch test still need to be performed in a Windows/PlatformIO environment with CrossInk's `freeink-sdk` submodule initialized.
