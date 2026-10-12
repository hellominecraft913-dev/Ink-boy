# Peanut-GB host smoke test

This is a toolchain-independent smoke check for the vendored emulator header. It does not emulate the X4 Pro HAL.

From the CrossInk repository root:

```bash
./integration/host_smoke/build_and_run.sh
```

The same command also tests Walnut-CGB frame callbacks, checks RGB565-to-monochrome conversion for black/white/primary colors/mid-gray, and verifies that the two core objects link together without duplicate symbols.
