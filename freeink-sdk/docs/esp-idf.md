# Using FreeInk from ESP-IDF

FreeInk's display (`FreeInkDisplay` + `BoardConfig`), `FreeInkUI` and
`FreeInkFont`'s TTF engine build as a plain ESP-IDF component, without arduino-esp32. The repo root's
`CMakeLists.txt` registers the component. `idf/` supplies the slice of the
Arduino API those sources call (`Arduino.h`, `SPI.h`, `Wire.h`), backed by
`driver/gpio`, `spi_master` and `esp_timer`. The SDK sources are not modified.

Arduino and PlatformIO-Arduino builds are not affected: they pull `libs/*`
through `lib_deps` and never see `idf/` or the root `CMakeLists.txt`. The shim
headers refuse to compile if `ARDUINO` is defined.

## Adding it to a project

Point `EXTRA_COMPONENT_DIRS` at the checkout (or vendor it under
`components/`), pick the device before `project()`, and require the component
(named after the checkout's directory, `freeink-sdk` by default):

```cmake
# top-level CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS path/to/freeink-sdk)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_DEFINITIONS "FREEINK_DEVICE_STICKY=1" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++2a" APPEND)
project(my_app)
```

```cmake
# main/CMakeLists.txt
idf_component_register(SRCS "main.cpp" REQUIRES freeink-sdk)
```

Then use the same API as on Arduino; see [FreeInkUI](freeink-ui.md#firmware-loop):

```cpp
#include <EInkDisplay.h>
#include <FreeInkApp.h>
#include <FreeInkUIDisplayTarget.h>

EInkDisplay display(0, 0, 0, 0, 0, 0);  // pins come from BoardConfig::ACTIVE

extern "C" void app_main() {
  BoardConfig::holdPowerRails();
  display.begin();
  // ... DisplayTarget + FreeInkApp, render, display.displayBuffer(...)
}
```

## sdkconfig

- PSRAM boards need PSRAM on (`CONFIG_SPIRAM=y`; `CONFIG_SPIRAM_MODE_OCT=y` on
  the Sticky's ESP32-S3R8): `display.begin()` puts the framebuffer there.
- Build with `CONFIG_COMPILER_OPTIMIZATION_PERF=y` and at 240 MHz. ESP-IDF's
  default `-Og` at 160 MHz makes a FreeInkUI render about twice as slow
  (Sticky, full 800×480 screen: 421 ms at `-Og`/160 MHz, 193 ms at `-O2`/240 MHz).

## What the shim does and doesn't do

- **SPI**: the panel drives `SPI2_HOST` (Arduino's default bus). If the
  firmware already initialized that bus (an SD card on the panel's bus, say),
  FreeInk adds its device to it, and holds the bus for each panel
  transaction. Chip select stays a GPIO the drivers toggle, as on Arduino.
  Plane writes go through a 4 KB DMA bounce buffer, so the framebuffer can
  live in PSRAM.
- **GPIO / interrupts**: `pinMode`, `digitalRead/Write`, and
  `attachInterrupt` (BUSY-edge refresh waits) via the GPIO ISR service, which
  FreeInk installs unless the firmware already has.
- **Serial**: `Serial` and `Serial0` print to the ESP-IDF console.
- **Wire is declared, not implemented.** Boards whose display path talks I2C
  (Paper Mono's PMIC, the Waveshare 3.97's AXP2101) compile but fail to link
  on ESP-IDF until `TwoWire` is backed by `driver/i2c_master`.
- Only the display, FreeInkUI and FreeInkFont's stb_truetype engine
  (`TtfFont`, `FontChain`) are in the component; `FreeInkUIRasterFont.h` draws
  a `FontChain` in a `DisplayTarget` font slot. FreeInkFont's FreeType backend,
  input, battery, SD, audio and the other managers are still Arduino-only.

## Validated

Seeed reTerminal Sticky on ESP-IDF 5.5.2 (`-O2`, 240 MHz): full refresh in
1.65 s, fast refresh in 610 ms of waveform plus about 40 ms of SPI.
