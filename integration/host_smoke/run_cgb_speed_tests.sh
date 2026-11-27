#!/usr/bin/env bash
# Host checks for the Game Boy Color speedups (no ESP32 or PlatformIO needed).
#   1. cgb_dither_test:        CgbDither.h == the per-pixel dither it replaced, bit for bit.
#   2. walnut_cgb_equiv_test:  luminance tables, "skip scanline composition" changes nothing about
#                              emulation, and the internal-RAM/PSRAM allocation rules and leaks.
#   3. walnut_cgb_smoke:       the bridge still runs a ROM end to end.
# Everything runs under AddressSanitizer + UndefinedBehaviorSanitizer.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT_DIR/integration/host_smoke"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

c++ -std=c++20 -O1 -g $SAN -Wall -Wextra -I"$ROOT_DIR/src" \
  "$HERE/cgb_dither_test.cpp" -o "$BUILD_DIR/cgb_dither_test"
"$BUILD_DIR/cgb_dither_test"

for t in walnut_cgb_equiv_test; do
  cc -std=gnu17 -O1 -g $SAN -Wall -Wextra -Wno-unused-parameter \
    -I"$HERE/stubs" -I"$ROOT_DIR/src" -I"$ROOT_DIR/lib/WalnutCGB/src" \
    "$HERE/$t.c" "$HERE/esp_stubs.c" -o "$BUILD_DIR/$t"
  "$BUILD_DIR/$t"
done

cc -std=gnu17 -O1 -g $SAN -Wall -Wextra -Wno-unused-parameter \
  -I"$HERE/stubs" -I"$ROOT_DIR/src" -I"$ROOT_DIR/lib/WalnutCGB/src" \
  "$ROOT_DIR/src/walnut_cgb_impl.c" "$HERE/esp_stubs.c" "$HERE/walnut_cgb_smoke.c" \
  -o "$BUILD_DIR/walnut_cgb_smoke"
"$BUILD_DIR/walnut_cgb_smoke"

echo "All Game Boy Color speedup checks passed."
