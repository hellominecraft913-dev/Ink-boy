#pragma once

#if defined(ARDUINO)
#error "FreeInk's ESP-IDF Arduino shim must not be built with arduino-esp32; use the real core's headers."
#endif

// FreeInk SDK — Arduino-style SPI on ESP-IDF's spi_master (see Arduino.h).
// Chip select stays a plain GPIO the drivers toggle themselves, as on Arduino.
// The bus is SPI2_HOST (Arduino's default FSPI); if the firmware already
// initialized it (e.g. for an SD card), FreeInk adds its device to that bus.

#include <Arduino.h>
#include <driver/spi_master.h>

#define MSBFIRST 1
#define LSBFIRST 0
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3

class SPISettings {
 public:
  SPISettings() = default;
  SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode) : clock(clock), bitOrder(bitOrder), dataMode(dataMode) {}
  uint32_t clock = 1000000;
  uint8_t bitOrder = MSBFIRST;
  uint8_t dataMode = SPI_MODE0;
};

class SPIClass {
 public:
  void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1, int8_t ss = -1);
  void end();
  void beginTransaction(const SPISettings& settings);
  void endTransaction();
  uint8_t transfer(uint8_t data);
  void writeBytes(const uint8_t* data, uint32_t size);

 private:
  spi_device_handle_t _dev = nullptr;
  SPISettings _settings;
  bool _ownsBus = false;
};
extern SPIClass SPI;
