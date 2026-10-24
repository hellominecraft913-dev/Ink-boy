# Game Boy / Game Boy Color integration

## ROM routing

- `.gb` routes to Peanut-GB (`src/peanut_gb_impl.c`).
- `.gbc` routes to Walnut-CGB (`src/walnut_cgb_impl.c`) via `GameBoyActivity` and its opaque C API in `src/walnut_cgb_port.h`.
- The file browser accepts both extensions. Extension matching is case-insensitive.

## Monochrome conversion for the e-ink panel

An X4 Pro's monochrome framebuffer cannot display the CGB's original colors, so Walnut-CGB color pixels are converted to 8-bit luminance with `Y = (299R + 587G + 114B) / 1000`. The bridge retains that luminance per 160x144 source pixel rather than converting each source pixel to black/white first. DMG-compatible games routed through Walnut-CGB convert their four shade values into the same luminance range.

When drawing a CGB frame, the renderer expands the luminance image to the final panel pixel grid and uses serpentine Floyd-Steinberg error diffusion to write the 1-bit framebuffer. Dithering after scaling avoids turning a 2x2 source-pixel pattern into enlarged dot clusters. Some grain remains unavoidable on a 1-bit panel; this aims for less regular, finer-grained tones. The normal Peanut-GB `.gb` renderer is unchanged.

## Why a C bridge is used

Peanut-GB and Walnut-CGB share C types and public symbol names (`struct gb_s`, `gb_init`, `gb_run_frame`, etc.) but have incompatible context layouts/API signatures. Walnut-CGB is compiled once in `src/walnut_cgb_impl.c` with its exported symbols renamed to `walnut_core_*`. CrossInk C++ code only sees the opaque `WalnutCgbContext` declared in `src/walnut_cgb_port.h`; do not include `walnut_cgb.h` in C++ activity files alongside `peanut_gb.h`.

## Source and manifests

`lib/WalnutCGB/src/walnut_cgb.h` is the user-supplied header, with a forward declaration of `struct gb_s` added before early callback prototypes so the implementation compiles as C. Both `lib/PeanutGB` and `lib/WalnutCGB` have `library.json` manifests for PlatformIO's library discovery.

## Testing

Check that RGB565 white maps to luminance 255, RGB565 black maps to luminance 0, and DMG shade 0 (white) maps to 255 while shade 3 (black) maps to 0. Then run `pio run -e x4-pro` and test several CGB titles in portrait and landscape. The framebuffer should still contain only black/white bits, while the frame input for Walnut-CGB is grayscale. Physical-panel comparisons are still required to tune the threshold and tone balance.
