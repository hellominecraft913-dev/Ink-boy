# Walnut-CGB vendored core

This folder contains the `walnut_cgb.h` file supplied for the CrossInk integration. Its upstream MIT license and SameBoy-derived-code notice are preserved at the top of the header. Keep those notices with redistribution and modifications.

CrossInk compiles this header once through `src/walnut_cgb_impl.c`; it must not be included directly by the C++ activity. `src/walnut_cgb_port.h` provides the opaque C interface and `src/walnut_cgb_color.h` provides the shared RGB565-to-monochrome conversion helpers used both by the core bridge and host-side tests.

The CGB LCD callback converts every palette entry to luminance and then ordered-dithers it to the two values used by CrossInk's bit-packed framebuffer: `0` for black and `3` for white. No RGB565 or intermediate gray pixel is passed to CrossInk's panel framebuffer.

The vendored source is a single-header emulator core, not a full cycle-accuracy or compatibility guarantee. ROM/mapper and timing compatibility is limited to the implementation in the supplied header.
