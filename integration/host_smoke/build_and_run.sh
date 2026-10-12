#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

# Existing Peanut-GB core smoke test.
cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$ROOT_DIR/lib/PeanutGB/src" \
  "$ROOT_DIR/integration/host_smoke/host_smoke.c" \
  -o "$BUILD_DIR/peanut_gb_host_smoke"
"$BUILD_DIR/peanut_gb_host_smoke"

# Walnut-CGB bridge smoke test; ESP allocator/restart APIs are host stubs.
cc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -O2 \
  -I"$ROOT_DIR/integration/host_smoke/stubs" \
  -I"$ROOT_DIR/src" \
  -I"$ROOT_DIR/lib/WalnutCGB/src" \
  "$ROOT_DIR/src/walnut_cgb_impl.c" \
  "$ROOT_DIR/integration/host_smoke/esp_stubs.c" \
  "$ROOT_DIR/integration/host_smoke/walnut_cgb_smoke.c" \
  -o "$BUILD_DIR/walnut_cgb_host_smoke"
"$BUILD_DIR/walnut_cgb_host_smoke"

# Make sure both cores can coexist in one link without duplicate public symbols.
cc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -O2 \
  -DCROSSINK_GAMEBOY=1 -I"$ROOT_DIR/lib/PeanutGB/src" \
  -c "$ROOT_DIR/src/peanut_gb_impl.c" -o "$BUILD_DIR/peanut_core.o"
cc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -O2 \
  -I"$ROOT_DIR/integration/host_smoke/stubs" \
  -I"$ROOT_DIR/src" -I"$ROOT_DIR/lib/WalnutCGB/src" \
  -c "$ROOT_DIR/src/walnut_cgb_impl.c" -o "$BUILD_DIR/walnut_core.o"
cc -r "$BUILD_DIR/peanut_core.o" "$BUILD_DIR/walnut_core.o" -o "$BUILD_DIR/both_cores.o"
echo "Dual-core relocatable link passed (no duplicate symbols)."
