#pragma once

#if defined(ARDUINO)
#error "FreeInk's ESP-IDF Arduino shim must not be built with arduino-esp32; use the real core's headers."
#endif

// FreeInk SDK — Arduino Wire, declared only (see Arduino.h).
// The board headers that talk I2C (M5Pm1, M5Ioe1, Axp2101) still compile, but
// nothing implements TwoWire on ESP-IDF yet, so a device whose display path
// needs it (Paper Mono, Waveshare 3.97) fails to link.
// ponytail: declaration-only; back TwoWire with driver/i2c_master when an IDF
// consumer targets one of those boards.

#include <Arduino.h>

class TwoWire {
 public:
  bool begin(int sda = -1, int scl = -1, uint32_t frequency = 0);
  void setTimeOut(uint16_t timeoutMs);
  void beginTransmission(uint8_t address);
  size_t write(uint8_t data);
  uint8_t endTransmission(bool sendStop = true);
  uint8_t requestFrom(uint8_t address, uint8_t quantity, uint8_t sendStop = true);
  int read();
};
extern TwoWire Wire;
