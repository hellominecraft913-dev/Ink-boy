#include "Ssd2677Driver.h"

#include <BoardConfig.h>

#if FREEINK_DEVICE_PICCO

#include <cstring>

#include "../lut/Ssd2677Luts.h"
#include "esp_heap_caps.h"

namespace freeink {
namespace {

struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t delayMs;
  uint8_t data[9];
};

// SE0400NQW47 init list (stock table at DROM 0x3c8246b4, run by FUN_4200b868).
constexpr InitCmd kInit[] = {
    {0x00, 2, 10, {0x27, 0x09}},
    {0x83, 9, 0, {0x00, 0x00, 0x03, 0x1F, 0x00, 0xC8, 0x02, 0xA8, 0x01}},
    {0x01, 6, 0, {0x07, 0xF0, 0x22, 0x20, 0x78, 0x22}},
    {0x06, 4, 0, {0x0F, 0x8B, 0x93, 0xC1}},
    {0xE7, 1, 0, {0xC1}},
    {0x50, 1, 0, {0x77}},
    {0x62, 8, 0, {0x98, 0x98, 0x98, 0x75, 0xCA, 0xB2, 0x98, 0x7E}},
    {0xE0, 1, 0, {0x10}},
    {0x65, 4, 0, {0x00, 0x00, 0x00, 0x00}},
    {0x30, 1, 0, {0x04}},
    {0x82, 1, 0, {0x28}},
    {0xE9, 1, 0, {0x01}},
};

constexpr uint8_t CMD_DATA = 0x10;
constexpr uint8_t CMD_LUT = 0x20;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_REFRESH = 0x12;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint16_t LUT_LEN = 535;

// 8 pixels of old + new -> two bytes of 2-bit pixels, old in the high bit,
// MSB-first (stock FUN_422869b4).
inline void packPixels(uint8_t oldByte, uint8_t newByte, uint8_t* out) {
  uint16_t v = 0;
  for (int i = 7; i >= 0; --i) v = static_cast<uint16_t>((v << 2) | (((oldByte >> i) & 1) << 1) | ((newByte >> i) & 1));
  out[0] = static_cast<uint8_t>(v >> 8);
  out[1] = static_cast<uint8_t>(v);
}

}  // namespace

uint32_t Ssd2677Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 20000000;
}

PanelGeometry Ssd2677Driver::geometry() const {
  const uint16_t w = BoardConfig::ACTIVE.displayWidth;
  const uint16_t h = BoardConfig::ACTIVE.displayHeight;
  return {w, h, static_cast<uint16_t>(w / 8), static_cast<uint32_t>(w / 8) * h};
}

void Ssd2677Driver::begin(EpdBus& bus) {
  bus.reset();
  bus.waitBusy(" SSD2677 reset");
  for (const InitCmd& c : kInit) {
    bus.cmd(c.cmd);
    bus.data(c.data, c.len);
    if (c.delayMs) delay(c.delayMs);
  }
  bus.waitBusy(" SSD2677 init");
  _needsPowerOn = true;
  _needsInitialFull = true;
}

void Ssd2677Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  // First paint after init clears the panel with the full waveform (stock forces
  // mode 0 on the first frame).
  if (_needsInitialFull) mode = RefreshMode::Full;
  sendPacked(bus, prev ? prev : fb, fb);
  refreshWith(bus, mode == RefreshMode::Fast ? lut_picco_se0400_fast : lut_picco_se0400_full, turnOff);
  _needsInitialFull = false;
}

void Ssd2677Driver::sendPacked(EpdBus& bus, const uint8_t* hi, const uint8_t* lo) {
  const uint32_t size = geometry().bufferSize;
  static uint8_t chunk[1024];
  bus.cmd(CMD_DATA);
  auto txn = bus.beginTxn();
  for (uint32_t i = 0; i < size;) {
    uint16_t n = 0;
    while (n < sizeof(chunk) && i < size) {
      packPixels(hi[i], lo[i], &chunk[n]);
      n += 2;
      ++i;
    }
    txn.writeBytes(chunk, n);
  }
}

void Ssd2677Driver::refreshWith(EpdBus& bus, const unsigned char* lut, bool turnOff) {
  bus.cmd(CMD_LUT);
  bus.data(lut, LUT_LEN);
  if (_needsPowerOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" SSD2677 power on");
    _needsPowerOn = false;
  }
  bus.cmd(CMD_REFRESH);
  bus.data(0x00);
  bus.waitRefreshComplete("refresh");
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.data(0x00);
    bus.waitBusy(" SSD2677 power off");
    _needsPowerOn = true;
  }
}

void Ssd2677Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (!lsb) return;
  const uint32_t size = geometry().bufferSize;
  if (!_lsb) _lsb = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (_lsb) memcpy(_lsb, lsb, size);
}

void Ssd2677Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  // Stock (FUN_42286a70): MSB mask in the high bit, LSB mask in the low bit.
  if (!msb || !_lsb) return;
  sendPacked(bus, msb, _lsb);
}

void Ssd2677Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)lut;
  (void)factoryMode;
  if (!_lsb) {  // planes never staged: plain B/W
    display(bus, fb, nullptr, RefreshMode::Full, turnOff);
    return;
  }
  refreshWith(bus, lut_picco_se0400_gray, turnOff);
  // The panel now shows gray content the previous B/W frame doesn't describe.
  _needsInitialFull = true;
}

void Ssd2677Driver::deepSleep(EpdBus& bus) {
  // Stock (FUN_4200b608): 0x07 with the 0xA5 check code.
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
  _needsPowerOn = true;
  _needsInitialFull = true;
}

PanelDriver& ssd2677Driver() {
  static Ssd2677Driver instance;
  return instance;
}

}  // namespace freeink

#endif  // FREEINK_DEVICE_PICCO
