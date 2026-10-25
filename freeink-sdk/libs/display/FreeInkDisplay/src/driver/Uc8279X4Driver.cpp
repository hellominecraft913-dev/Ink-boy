#include "Uc8279X4Driver.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "../lut/Uc8279X3Luts.h"

// Orientation of the visible-row stream, switchable per build for field A/B.
// Defaults (0/0) = the stock convention, HARDWARE-CONFIRMED upright on a field
// UC8279 unit: rows forward, no byte/bit mirroring — orientation comes from the
// PSR SHL bit (0x37), which must be written AFTER PON to latch (PON reloads the
// MTP defaults).
#ifndef FREEINK_UC8279X4_ROWREV
#define FREEINK_UC8279X4_ROWREV 0
#endif
#ifndef FREEINK_UC8279X4_XMIRROR
#define FREEINK_UC8279X4_XMIRROR 0
#endif

namespace freeink {
namespace {
// UC8279 (800x480 X4 Pro variant) command set — UC81xx KW family, per the vendor
// X4 Pro display hardware reference.
constexpr uint8_t CMD_PANEL_SETTING = 0x00;       // PSR
constexpr uint8_t CMD_POWER_OFF = 0x02;           // POF
constexpr uint8_t CMD_PFS = 0x03;                 // PFS (power-off sequence)
constexpr uint8_t CMD_POWER_ON = 0x04;            // PON
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;          // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM1 = 0x10;                // OLD plane in KW mode / AA plane0
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode / AA plane1
constexpr uint8_t CMD_PLL = 0x30;                 // PLL frame rate
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // CDI — 1 data byte on this part
constexpr uint8_t CMD_RESOLUTION = 0x61;          // TRES
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;   // GSST (4 data bytes)
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;      // PTL (window; stock re-issues per partial)
constexpr uint8_t CMD_PARTIAL_IN = 0x91;          // PTIN
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;         // PTOUT
constexpr uint8_t CMD_CCSET = 0xE0;               // CCSET (cascade/output enable)
constexpr uint8_t CMD_GATE_SCAN = 0xE1;           // gate-scan selection
constexpr uint8_t CMD_TSSET = 0xE5;               // TSSET (forced temperature)

constexpr uint8_t GRAY_LUT_LEN = 49;
struct GrayLut {
  uint8_t cmd;
  uint8_t data[GRAY_LUT_LEN];
};
constexpr GrayLut kXtfAa02[5] = {
    {0x20, {0x01, 0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // VCOM
    {0x21, {0x01, 0x02, 0x02, 0x41, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WW
    {0x22, {0x01, 0x02, 0x82, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BW (dark gray)
    {0x23, {0x01, 0x02, 0x82, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WB (dark gray)
    {0x24, {0x01, 0x02, 0x02, 0x81, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BB
};
constexpr GrayLut kXtfAa68[5] = {
    {0x20, {0x01, 0x02, 0x03, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // VCOM
    {0x21, {0x01, 0x02, 0x03, 0x41, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WW
    {0x22, {0x01, 0x02, 0x83, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BW (dark gray)
    {0x23, {0x01, 0x02, 0x83, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // WB (dark gray)
    {0x24, {0x01, 0x02, 0x03, 0x81, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}},  // BB
};

// UC8279_aa_prebw_mid — the stock non-flashing previous->current transition
// waveform (Factory.bin DROM 0x3c5d826f, uploaded by FUN_4214d3a0). 5 tables,
// command-prefixed, 42 data bytes each (distinct from the 49-byte display bank).
// Same layout as the UC8179 sibling's kGrayPreBwMid.
constexpr uint8_t PREBW_LUT_LEN = 42;
const uint8_t kXtfPreBwMid[5][PREBW_LUT_LEN + 1] = {
    {0x20, 0x01, 0x06, 0x01, 0x06, 0x06, 0x01, 0x01, 0x01, 0x02, 0x04, 0x00, 0x00, 0x01, 0x01},
    {0x21, 0x01, 0x06, 0x81, 0x06, 0x06, 0x01, 0x01, 0x01, 0x02, 0x04, 0x00, 0x00, 0x01, 0x01},
    {0x22, 0x01, 0x86, 0x81, 0x86, 0x86, 0x01, 0x01, 0x01, 0x82, 0x84, 0x00, 0x00, 0x01, 0x01},
    {0x23, 0x01, 0x46, 0x41, 0x46, 0x46, 0x01, 0x01, 0x01, 0x42, 0x44, 0x00, 0x00, 0x01, 0x01},
    {0x24, 0x01, 0x06, 0x01, 0x06, 0x06, 0x01, 0x01, 0x01, 0x02, 0x44, 0x00, 0x00, 0x01, 0x01},
    // remaining bytes of each 42-byte table are zero (aggregate init).
};

const GrayLut* selectAaLuts() {
  // LUT_VER stored by the boot probe. Stock's panel LUT registry (X4 Pro
  // 260917 build, table @0x3c1adbd4 + hardcoded bank selects in its UC8279
  // refresh) keys 0x02/0x03 to the QY bank and 0x68/0x69 to the ZHX bank; X4C
  // V7.1.21 points BOE 4.28 D/E (0x41/0x42) at the ZHX bank as well. Anything
  // else unknown keeps the existing fallback to the ZHX bytes. 0x67
  // never reaches this: grayscaleCapabilities() reports unsupported for it
  // (stock ships no external-LUT tables for that id).
  const uint8_t v = BoardConfig::ACTIVE.displayControllerVariant;
  return (v == 0x02 || v == 0x03) ? kXtfAa02 : kXtfAa68;
}

// Four-tone grayscale bank, built at compile time by time-scaling the X3
// XTH4 four-grey waveform (ported from the inx-pro downstream, which ships
// this for its UC8279 sleep/comic images; HARDWARE-VALIDATED on an X4C).
// Phase durations are scaled to FREEINK_UC8279X4_GRAY_SPEED percent, then
// repaired: net charge per table is restored to zero (a naive timing cut
// breaks DC balance) and all tables are padded to equal total frame count.
// The bank consumes the ordinary inverted fold planes and is uploaded with
// the 0x22/0x23 registers EXCHANGED (their empirically found plane-code
// assignment for this panel).
#ifndef FREEINK_UC8279X4_GRAY_SPEED
#define FREEINK_UC8279X4_GRAY_SPEED 60
#endif
struct GrayBank {
  uint8_t data[5][GRAY_LUT_LEN]{};
};

constexpr GrayBank makeQualityBank() {
  GrayBank out{};

  constexpr uint8_t kGroups = 7;
  constexpr uint8_t kPhases = 4;
  struct Phase {
    uint8_t rail;
    uint8_t frames;
  };
  Phase ph[5][kGroups][kPhases] = {};
  uint8_t used[5] = {};

  auto netCharge = [&](uint8_t t) {
    int n = 0;
    for (uint8_t g = 0; g < used[t]; g++)
      for (uint8_t i = 0; i < kPhases; i++) {
        if (ph[t][g][i].rail == 1)
          n += ph[t][g][i].frames;
        else if (ph[t][g][i].rail == 2)
          n -= ph[t][g][i].frames;
      }
    return n;
  };
  auto totalFrames = [&](uint8_t t) {
    int total = 0;
    for (uint8_t g = 0; g < used[t]; g++)
      for (uint8_t i = 0; i < kPhases; i++) total += ph[t][g][i].frames;
    return total;
  };

  for (uint8_t t = 0; t < 5; t++) {
    const uint8_t* src = kUc8279X3_Xth4[t];
    uint8_t g = 0;
    for (; g < kGroups; g++) {
      const uint8_t* grp = src + g * 7;
      bool empty = true;
      for (uint8_t i = 0; i < kPhases; i++)
        if (grp[1 + i] != 0) empty = false;
      if (empty) break;
      for (uint8_t i = 0; i < kPhases; i++) {
        const uint8_t b = grp[1 + i];
        const uint8_t frames = static_cast<uint8_t>(b & 0x3F);
        uint16_t scaled = (static_cast<uint16_t>(frames) * FREEINK_UC8279X4_GRAY_SPEED + 50u) / 100u;
        if (frames != 0 && scaled == 0) scaled = 1;
        if (scaled > 63) scaled = 63;
        ph[t][g][i] = {static_cast<uint8_t>(b >> 6), static_cast<uint8_t>(scaled)};
      }
    }
    used[t] = g;

    // Restore DC balance: nudge the largest opposing-rail phase until the
    // VSH/VSL frame counts cancel again.
    int n = netCharge(t);
    while (n != 0) {
      const uint8_t want = n > 0 ? 2 : 1;
      uint8_t bg = 0xFF, bi = 0, best = 0;
      for (uint8_t g2 = 0; g2 < used[t]; g2++)
        for (uint8_t i = 0; i < kPhases; i++)
          if (ph[t][g2][i].rail == want && ph[t][g2][i].frames > best && ph[t][g2][i].frames < 63) {
            best = ph[t][g2][i].frames;
            bg = g2;
            bi = i;
          }
      if (bg == 0xFF) break;
      ph[t][bg][bi].frames++;
      n = netCharge(t);
    }
  }

  // Equalize table lengths with idle (VSS) padding groups so all five LUTs
  // finish on the same frame.
  int longest = 0;
  for (uint8_t t = 0; t < 5; t++)
    if (totalFrames(t) > longest) longest = totalFrames(t);
  for (uint8_t t = 0; t < 5; t++) {
    int deficit = longest - totalFrames(t);
    while (deficit > 0 && used[t] < kGroups - 1) {
      const uint8_t chunk = static_cast<uint8_t>(deficit > 63 ? 63 : deficit);
      ph[t][used[t]][0] = {0, chunk};
      used[t]++;
      deficit -= chunk;
    }
  }

  for (uint8_t t = 0; t < 5; t++) {
    for (uint8_t g = 0; g < used[t]; g++) {
      uint8_t* dst = out.data[t] + g * 7;
      dst[0] = 0x01;
      for (uint8_t i = 0; i < kPhases; i++)
        dst[1 + i] = static_cast<uint8_t>((ph[t][g][i].rail << 6) | (ph[t][g][i].frames & 0x3F));
      dst[5] = 0x01;
      dst[6] = 0x01;
    }
  }

  return out;
}

constexpr GrayBank kQualityBank = makeQualityBank();

// Register order for the quality bank: 0x22 and 0x23 exchanged relative to
// table order (inx-pro's empirically found assignment for this panel).
constexpr uint8_t kQualityLutReg[5] = {0x20, 0x21, 0x23, 0x22, 0x24};

}  // namespace

const Uc8279X4Config& uc8279X4DefaultConfig() {
  static const Uc8279X4Config cfg = {
      0x37,  // psr0: stock X4C UC8279 vtable value, SHL set (rows forward, no RAM
             // mirroring and writes PSR between PON and DRF — PON reloads MTP
             // settings, so only post-PON PSR writes latch). REG=1 (external LUT)
             // at init and for AA; built-in refreshes re-assert psr0 & 0xDF = 0x17
      0x4D,  // psr1
      0x20,  // pfs (0x03)
      0x0E,  // pll (0x30) — X4 Pro only; stock X4C omits this command
      0x02,  // gateScan (0xE1)
      0x02,  // ccset (0xE0)
      0x1E,  // tsset (0xE5) full refresh
      0x5A,  // tssetFast (0xE5) fast/partial refresh
      0x97,  // cdiAa (0x50, 1 byte): constant on EVERY AA refresh, per stock
      0x97,  // cdiBwFull (0x50): stock writes it before every GC/full refresh
      0xD7,  // cdiBwFast (0x50): stock value for the windowed partial
      600,   // tresHeight — addressed 800x600 (480 visible)
      120,   // gateOffset — visible gates start at 120 on this variant
  };
  return cfg;
}

// Visible geometry comes from the active BoardProfile (X4 Pro, 800x480).
Uc8279X4Driver::Uc8279X4Driver(const Uc8279X4Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _tresH(cfg.tresHeight),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8279X4Driver::spiHz() const {
  // UC8279 serial write timing is rated to 20 MHz, same as the rest of the family.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8279X4Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

// Vendor runtime init: PSR, TRES (800x600), GSST (4x0), PFS, PLL, gate scan.
// PWR/VDCS/BTST stay panel-programmed (OTP/MTP) and are not written here.
void Uc8279X4Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  bus.cmd(CMD_RESOLUTION);
  bus.data(static_cast<uint8_t>((_w >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_w & 0xFF));
  bus.data(static_cast<uint8_t>((_tresH >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_tresH & 0xFF));

  bus.cmd(CMD_GATE_SOURCE_START);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);

  bus.cmd(CMD_PFS);
  bus.data(_cfg.pfs);

  // The stock X4C UC8279 vtable's PLL hook is a no-op. X4 Pro programs PLL.
  if (!BoardConfig::isX4Classic()) {
    bus.cmd(CMD_PLL);
    bus.data(_cfg.pll);
    _pllWritten = _cfg.pll;  // xteink-pokemon: see startBwRefresh
  }

  bus.cmd(CMD_GATE_SCAN);
  bus.data(_cfg.gateScan);

  _isScreenOn = false;
  _grayRefreshedOnce = false;
}

void Uc8279X4Driver::begin(EpdBus& bus) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  _directGrayOnPanel = false;
  _grayImagePass = false;
  _absoluteInput = false;
  _directGrayPass = false;
  _oldPlaneValid = false;
  _needFullClear = true;
  _redriveAfterGray = false;
  bus.reset(50);
  initController(bus);
  // Framebuffer-sized scratch for the grayscale absolute-plane fold (SPIRAM;
  // falls back to a plain malloc). If it can't be had, copyGrayscale* degrade to
  // the raw-plane fallback path.
  if (_grayBase == nullptr) {
    _grayBase = static_cast<uint8_t*>(heap_caps_malloc(_bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_grayBase == nullptr) _grayBase = static_cast<uint8_t*>(malloc(_bufferSize));
  }
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
}

void Uc8279X4Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  // CrossPoint's AA path draws its B/W base with an ordinary Fast paint. Right
  // after an AA page, route that base through stock's non-flashing prev->current
  // transition instead of a plain DU — this keeps the gray edge charge in check
  // continuously (like the UC8179 sibling), so the periodic Half scrub works
  // without a full flash. Same guard as UC8179.
  if (mode == RefreshMode::Fast && _redriveAfterGray && _grayRefreshedOnce && _oldPlaneValid && !_needFullClear) {
    transitionGrayscaleBase(bus, fb, turnOff);
    return;
  }
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

void Uc8279X4Driver::streamPlane(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb, bool invert) {
  uint8_t row[128];
  const uint16_t wb = _wb <= sizeof(row) ? _wb : sizeof(row);
  bus.cmd(ramCmd);
  // The OEM driver keeps CS asserted across a plane upload. Preserve our validated
  // gate offset/orientation, but avoid a new SPI transaction for every row.
  bus.beginTxn();
  // Gates before the visible window (the 120-gate offset): white.
  memset(row, 0xFF, wb);
  for (uint16_t y = 0; y < _cfg.gateOffset; y++) bus.rawWriteBytes(row, wb);
  // Visible rows, stock convention (hardware-confirmed upright): forward order,
  // bytes as-is. The ROWREV/XMIRROR switches (row reversal / reversed byte order
  // + reversed bits) exist for future panel sub-variants whose scan differs.
  // AA planes are sent bitwise-inverted per the vendor reference.
  static const uint8_t kBitRev[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
  for (uint16_t n = 0; n < _h; n++) {
    const uint16_t y = FREEINK_UC8279X4_ROWREV ? static_cast<uint16_t>(_h - 1 - n) : n;
    const uint8_t* src = fb + static_cast<uint32_t>(y) * _wb;
    for (uint16_t i = 0; i < wb; i++) {
      uint8_t b;
      if (FREEINK_UC8279X4_XMIRROR) {
        const uint8_t m = src[wb - 1 - i];
        b = static_cast<uint8_t>((kBitRev[m & 0x0F] << 4) | kBitRev[m >> 4]);
      } else {
        b = src[i];
      }
      row[i] = invert ? static_cast<uint8_t>(~b) : b;
    }
    bus.rawWriteBytes(row, wb);
  }
  // Gates after the visible window: white, up to the addressed gate count.
  memset(row, 0xFF, wb);
  for (uint16_t y = _cfg.gateOffset + _h; y < _tresH; y++) bus.rawWriteBytes(row, wb);
  bus.endTxn();
}

// Same geometry/mirroring as streamPlane, but each visible byte is lhs ^ rhs.
void Uc8279X4Driver::streamPlaneXor(EpdBus& bus, uint8_t ramCmd, const uint8_t* lhs, const uint8_t* rhs, bool invert) {
  uint8_t row[128];
  const uint16_t wb = _wb <= sizeof(row) ? _wb : sizeof(row);
  bus.cmd(ramCmd);
  bus.beginTxn();
  memset(row, 0xFF, wb);
  for (uint16_t y = 0; y < _cfg.gateOffset; y++) bus.rawWriteBytes(row, wb);
  static const uint8_t kBitRev[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
  for (uint16_t n = 0; n < _h; n++) {
    const uint16_t y = FREEINK_UC8279X4_ROWREV ? static_cast<uint16_t>(_h - 1 - n) : n;
    const uint8_t* a = lhs + static_cast<uint32_t>(y) * _wb;
    const uint8_t* c = rhs + static_cast<uint32_t>(y) * _wb;
    for (uint16_t i = 0; i < wb; i++) {
      uint8_t b;
      if (FREEINK_UC8279X4_XMIRROR) {
        const uint8_t m = static_cast<uint8_t>(a[wb - 1 - i] ^ c[wb - 1 - i]);
        b = static_cast<uint8_t>((kBitRev[m & 0x0F] << 4) | kBitRev[m >> 4]);
      } else {
        b = static_cast<uint8_t>(a[i] ^ c[i]);
      }
      row[i] = invert ? static_cast<uint8_t>(~b) : b;
    }
    bus.rawWriteBytes(row, wb);
  }
  memset(row, 0xFF, wb);
  for (uint16_t y = _cfg.gateOffset + _h; y < _tresH; y++) bus.rawWriteBytes(row, wb);
  bus.endTxn();
}

void Uc8279X4Driver::powerOnIfNeeded(EpdBus& bus, const char* tag) {
  if (_isScreenOn) return;  // vendor _powerOn(): no second PON while powered
  bus.cmd(CMD_POWER_ON);
  bus.waitBusy(tag);
  _isScreenOn = true;
}

bool Uc8279X4Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  const bool paintDestination = _directGrayOnPanel;
  _directGrayOnPanel = false;
  _grayImagePass = false;
  _absoluteInput = false;
  _directGrayPass = false;
  (void)prev;
  // Snapshot the B/W base for a grayscale overlay that may follow (the reader
  // draws this base, then folds it into the absolute AA planes). Harmless for
  // pure B/W paints — it's just a memcpy that the next AA page consumes.
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
  pulseFinish(bus);          // xteink-pokemon: a pulse scan may still run
  _phaseStartUs = micros();  // xteink-pokemon: refresh phase timings
  _timings = {};
  // xteink-pokemon: a game's Fast refresh sends and drives only the rectangle
  // that changed, or nothing at all when the frame is the same.
  if (_diffWindow && _diffRefValid && mode == RefreshMode::Fast && !_needFullClear && _oldPlaneValid &&
      !paintDestination && !_redriveAfterGray && fb != nullptr && !FREEINK_UC8279X4_ROWREV &&
      !FREEINK_UC8279X4_XMIRROR) {
    if (!findChangedWindow(fb)) {
      _grayBaseValid = true;  // the snapshot is this frame already
      _timings.copyUs = phaseUs();
      ++_lastTimings.skipped;
      _pendingRefresh = false;
      return false;
    }
    const uint16_t bytes = _winByte1 - _winByte0 + 1;
    for (uint16_t r = _winRow0; r <= _winRow1; ++r) {
      const uint32_t at = static_cast<uint32_t>(r) * _wb + _winByte0;
      memcpy(_grayBase + at, fb + at, bytes);
    }
    _grayBaseValid = true;
    _timings.copyUs = phaseUs();
    _windowActive = true;
    bus.cmd(CMD_PARTIAL_IN);
    sendPartialWindow(bus);
    streamWindow(bus, CMD_DTM2, fb);
    bus.cmd(CMD_PARTIAL_OUT);
    _timings.newPlaneUs = phaseUs();
    _timings.planeBytes = static_cast<uint32_t>(_winRow1 - _winRow0 + 1) * bytes;
    startBwRefresh(bus, true);
    _timings.setupUs = phaseUs();
    _pendingPartial = true;
    _pendingTurnOff = turnOff;
    _pendingRefresh = true;
    return true;
  }
  _timings.planeBytes = static_cast<uint32_t>(_tresH) * _wb;
  if (_grayBase != nullptr && fb != nullptr) {
    memcpy(_grayBase, fb, _bufferSize);
    _grayBaseValid = true;
  }
  _timings.copyUs = phaseUs();  // xteink-pokemon
  // Same differential model as the UC8179 sibling: only an EXPLICIT Fast request
  // uses the PTIN/PTOUT DU partial (OLD plane = previous displayed frame). Full
  // AND Half both run the clearing OTP GC waveform — but they seed the OLD plane
  // DIFFERENTLY (this is the load-bearing distinction, copied from UC8179):
  //   * Half = charge SCRUB: OLD = complement of the target, so EVERY pixel
  //     (including white background) is forced through a transition cell and no
  //     WW/BB pixel idles with stale AA charge. A white-seed GC only redraws
  //     black-target pixels and leaves background ghost parked in WW — that was
  //     the residual ghosting seen after fix9's white-seed Half.
  //   * Full = absolute-from-white seed (the known clean full flash).
  // Half is CrossPoint's periodic ghost-cleanup (every getRefreshFrequency()
  // pages) AND the manual force-refresh; it MUST scrub, exactly like UC8179.
  const bool scrub = (mode == RefreshMode::Half);
  const bool fast = (mode == RefreshMode::Fast) && !_needFullClear && _oldPlaneValid;

  if (paintDestination) {
    streamPlane(bus, CMD_DTM1, fb, true);
    streamPlane(bus, CMD_DTM2, fb);
    startBwRefresh(bus, true);
    bus.waitRefreshComplete(" 8279x4_BW_TARGET_DRF");
    bus.cmd(CMD_PARTIAL_OUT);
  }

  streamPlane(bus, CMD_DTM2, fb);
  if (!fast) {
    if (scrub) {
      // Half scrub: OLD = ~target -> every pixel transitions, purging idle charge.
      streamPlane(bus, CMD_DTM1, fb, /*invert=*/true);
    } else {
      // Full flash: seed the OLD plane white across the whole 600-gate scan for
      // the absolute GC-from-white waveform.
      bus.fillPlane(CMD_DTM1, 0xFF, _tresH, _wb);
    }
  } else if (_redriveAfterGray) {
    // Re-drive every pixel once after grayscale so the B/W transition scrubs
    // residual edge charge before restoring the ordinary differential baseline.
    streamPlane(bus, CMD_DTM1, fb, /*invert=*/true);
  }
  // Consumed: the white-seed (!fast) or the re-drive above already scrubbed any
  // post-AA gray residue for this frame.
  _redriveAfterGray = false;

  _timings.newPlaneUs = phaseUs();  // xteink-pokemon: DTM2 (and DTM1 seeds)
  startBwRefresh(bus, fast);
  _timings.setupUs = phaseUs();  // xteink-pokemon: through the waveform's start
  _pendingPartial = fast;
  _pendingTurnOff = turnOff;
  _pendingRefresh = true;
  return true;
}

void Uc8279X4Driver::startBwRefresh(EpdBus& bus, bool fast) {
  // Built-in refresh setup, byte-for-byte the stock FW trigger order (RE of
  // Factory.bin FUN_4214d050 partial / FUN_4214cfe8 full): CDI first — stock
  // writes the 1-byte CDI on EVERY refresh, 0x97 full / 0xD7 partial — then
  // CCSET/TSSET (+ PFS/gate-scan on partial), PON, the partial window, and a
  // PSR rewrite between PON and DRF (same latch behavior as the AA path).
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(fast ? _cfg.cdiBwFast : _cfg.cdiBwFull);
  bus.cmd(CMD_CCSET);
  bus.data(_cfg.ccset);  // 0x02
  bus.cmd(CMD_TSSET);
  // xteink-pokemon: a game may force another temperature for Fast refreshes.
  bus.data(fast ? (_tuneTsset ? _tuneTsset : _cfg.tssetFast) : _cfg.tsset);  // DU 0x5A / GC 0x1E
  // xteink-pokemon: and another frame rate, restored for every other refresh.
  if (!BoardConfig::isX4Classic()) {
    const uint8_t pll = (fast && _tunePll) ? _tunePll : _cfg.pll;
    if (pll != _pllWritten) {
      bus.cmd(CMD_PLL);
      bus.data(pll);
      _pllWritten = pll;
    }
  }
  if (fast) {
    bus.cmd(CMD_PFS);
    bus.data(_cfg.pfs);  // 0x03 <- 0x20
    bus.cmd(CMD_GATE_SCAN);
    bus.data(_cfg.gateScan);  // 0xE1 <- 0x02
  }

  // xteink-pokemon: a game may run Fast refreshes on a host-loaded, shortened
  // DU waveform (REG=1 below) instead of the panel's built-in one.
  // xteink-pokemon: or the one-pulse LUT of a pulse scan.
  const bool externalDu = fast && (_duScalePct != 0 || _pulseFrames != 0);
  if (fast && _pulseFrames != 0) {
    loadPulseLut(bus, _pulseFrames);
  } else if (externalDu) {
    loadScaledDuLut(bus, _duScalePct);
  }

  powerOnIfNeeded(bus, " 8279x4_PON");

  if (fast) {
    // Stock NEVER issues PTIN without a PTL (0x90) window — with PTL unset the
    // DU scans but develops nothing (first field unit: 443 ms DRF, no image).
    bus.cmd(CMD_PARTIAL_IN);
    sendPartialWindow(bus);
  }
  bus.cmd(CMD_PANEL_SETTING);
  // REG cleared -> OTP (0x13); xteink-pokemon: REG kept for an external DU.
  bus.data(externalDu ? _cfg.psr0 : static_cast<uint8_t>(_cfg.psr0 & 0xDF));
  bus.data(_cfg.psr1);
  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform started (BUSY dropped) before returning, so
  // displayFinish() only rides out the completion edge.
  {
    const int8_t busyPin = bus.pins().busy;
    const unsigned long t0 = millis();
    while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) delay(1);
  }
}

// xteink-pokemon: loads the stock X3 UC8279 B/W DU bank (kUc8279X3_BwDu, the
// same controller family) into the LUT registers with the frame count of every
// phase scaled to `percent`. A table is groups of 7 bytes - repeat, four phase
// bytes (bits 7-6 the drive level, bits 5-0 the frames), two trailing bytes -
// and only phase bytes longer than one frame are scaled; the drive levels and
// so the voltages stay as they are.
void Uc8279X4Driver::loadScaledDuLut(EpdBus& bus, const uint8_t percent) {
  constexpr size_t kTableLen = 42;
  for (size_t t = 0; t < 5; ++t) {
    uint8_t table[kTableLen];
    memcpy(table, &kUc8279X3_BwDu[t][1], kTableLen);
    for (size_t i = 0; i < kTableLen; ++i) {
      const size_t pos = i % 7;
      if (pos < 1 || pos > 4) continue;
      const uint8_t frames = table[i] & 0x3F;
      if (frames <= 1) continue;
      int scaled = (frames * percent + 50) / 100;
      if (scaled < 1) scaled = 1;
      if (scaled > 0x3F) scaled = 0x3F;
      table[i] = static_cast<uint8_t>((table[i] & 0xC0) | scaled);
    }
    bus.cmd(kUc8279X3_BwDu[t][0]);
    bus.data(table, kTableLen);
  }
}

// xteink-pokemon: the one-pulse LUT of a pulse scan (see pulseScan). The two
// transition tables of the DU bank (0x22, 0x23: a pixel whose OLD and NEW bits
// differ) keep the drive level the bank gives them, for `frames` frames; the
// other three (VCOM, and 0x21/0x24 where the bits agree) stay at level 0 for
// as long, so a held pixel gets no drive. Copying the bank's levels keeps the
// mapping the DU refresh already proved: whichever table turns a pixel black
// there does here too.
void Uc8279X4Driver::loadPulseLut(EpdBus& bus, const uint8_t frames) {
  constexpr size_t kTableLen = 42;
  const uint8_t n = static_cast<uint8_t>(frames < 1 ? 1 : (frames > 0x3F ? 0x3F : frames));
  for (size_t t = 0; t < 5; ++t) {
    const uint8_t reg = kUc8279X3_BwDu[t][0];
    uint8_t level = 0;
    if (reg == 0x22 || reg == 0x23) {
      // The level the bank drives longest in this table.
      uint16_t byLevel[4] = {};
      for (size_t i = 0; i < kTableLen; ++i) {
        const size_t pos = i % 7;
        if (pos < 1 || pos > 4) continue;
        const uint8_t b = kUc8279X3_BwDu[t][1 + i];
        byLevel[b >> 6] += b & 0x3F;
      }
      for (uint8_t l = 1; l < 4; ++l)
        if (byLevel[l] > byLevel[level]) level = l;
    }
    uint8_t table[kTableLen] = {};
    table[0] = 0x01;
    table[1] = static_cast<uint8_t>((level << 6) | n);
    // The group's last two bytes are repeat counts (0 runs nothing at all).
    table[5] = 0x01;
    table[6] = 0x01;
    bus.cmd(reg);
    bus.data(table, kTableLen);
  }
}

// xteink-pokemon: see PanelDriver::pulseScan. Both planes of the window go
// out (NEW = target, OLD = target with the driven pixels flipped), then a Fast
// refresh runs the one-pulse LUT; power stays on between scans.
bool Uc8279X4Driver::pulseScan(EpdBus& bus, const uint8_t* target, const uint8_t* drive, const uint16_t row0,
                               const uint16_t row1, const uint16_t byte0, const uint16_t byte1, const uint8_t frames,
                               const bool wait) {
  if (FREEINK_UC8279X4_ROWREV || FREEINK_UC8279X4_XMIRROR) return false;
  if (target == nullptr || drive == nullptr || row0 > row1 || row1 >= _h || byte0 > byte1 || byte1 >= _wb) {
    return false;
  }
  // Both planes of the window are built first, into contiguous buffers, while
  // the previous scan's waveform may still run; then they go out in a few
  // long writes once it is done (sending during a waveform froze the screen
  // in 1.8.6-beta.3).
  const uint32_t prepStart = micros();
  if (!preparePulsePlanes(target, drive, row0, row1, byte0, byte1)) return false;
  const uint32_t prepUs = micros() - prepStart;
  const uint32_t size = static_cast<uint32_t>(row1 - row0 + 1) * (byte1 - byte0 + 1);
  pulseFinish(bus);
  if (_pendingRefresh) displayFinish(bus, target);
  _phaseStartUs = micros();
  _timings = {};
  _timings.copyUs = prepUs;
  sendPulsePlanes(bus, row0, row1, byte0, byte1, size);
  const uint32_t sendUs = phaseUs();
  _winRow0 = row0;
  _winRow1 = row1;
  _winByte0 = byte0;
  _winByte1 = byte1;
  _windowActive = true;
  _timings.newPlaneUs = sendUs;
  _timings.planeBytes = size;
  _pulseFrames = frames;
  startBwRefresh(bus, true);
  _pulseFrames = 0;
  _timings.setupUs = phaseUs();
  _pulseDirty = true;
  _diffRefValid = false;
  _pulsePending = true;
  if (wait) pulseFinish(bus);
  return true;
}

// xteink-pokemon: the window's NEW plane (the target) and OLD plane (the
// target with the driven pixels flipped), row after row in two PSRAM buffers
// kept for the session.
bool Uc8279X4Driver::preparePulsePlanes(const uint8_t* target, const uint8_t* drive, const uint16_t row0,
                                        const uint16_t row1, const uint16_t byte0, const uint16_t byte1) {
  if (_pulseNew == nullptr) {
    _pulseNew = static_cast<uint8_t*>(heap_caps_malloc(_bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    _pulseOld = static_cast<uint8_t*>(heap_caps_malloc(_bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_pulseNew == nullptr || _pulseOld == nullptr) {
      heap_caps_free(_pulseNew);
      heap_caps_free(_pulseOld);
      _pulseNew = _pulseOld = nullptr;
      return false;
    }
  }
  const uint16_t bytes = byte1 - byte0 + 1;
  uint8_t* n = _pulseNew;
  uint8_t* o = _pulseOld;
  for (uint16_t r = row0; r <= row1; ++r) {
    const uint32_t at = static_cast<uint32_t>(r) * _wb + byte0;
    memcpy(n, target + at, bytes);
    for (uint16_t i = 0; i < bytes; ++i) o[i] = static_cast<uint8_t>(n[i] ^ drive[at + i]);
    n += bytes;
    o += bytes;
  }
  return true;
}

// xteink-pokemon: PTIN, the window, both prepared planes in long writes, PTOUT.
void Uc8279X4Driver::sendPulsePlanes(EpdBus& bus, const uint16_t row0, const uint16_t row1, const uint16_t byte0,
                                     const uint16_t byte1, const uint32_t size) {
  _winRow0 = row0;
  _winRow1 = row1;
  _winByte0 = byte0;
  _winByte1 = byte1;
  _windowActive = true;
  bus.cmd(CMD_PARTIAL_IN);
  sendPartialWindow(bus);
  const uint8_t* planes[2] = {_pulseNew, _pulseOld};
  const uint8_t cmds[2] = {CMD_DTM2, CMD_DTM1};
  for (int p = 0; p < 2; ++p) {
    bus.cmd(cmds[p]);
    bus.beginTxn();
    for (uint32_t done = 0; done < size;) {
      const uint32_t n = size - done < 32768 ? size - done : 32768;
      bus.rawWriteBytes(planes[p] + done, static_cast<uint16_t>(n));
      done += n;
    }
    bus.endTxn();
  }
  bus.cmd(CMD_PARTIAL_OUT);
}

// xteink-pokemon: waits out a pulse scan started without waiting.
void Uc8279X4Driver::pulseFinish(EpdBus& bus) {
  if (!_pulsePending) return;
  _pulsePending = false;
  bus.waitRefreshComplete(" 8279x4_PULSE");
  _timings.waveformUs = phaseUs();
  bus.cmd(CMD_PARTIAL_OUT);
  _windowActive = false;
  _timings.refreshes = _lastTimings.refreshes + 1;
  _timings.skipped = _lastTimings.skipped;
  _lastTimings = _timings;
}

// xteink-pokemon: the RAM planes hold drive masks after pulse scans; the next
// refresh is a full one that redraws every pixel from the framebuffer.
void Uc8279X4Driver::endPulseDrive(EpdBus& bus) {
  pulseFinish(bus);
  heap_caps_free(_pulseNew);
  heap_caps_free(_pulseOld);
  _pulseNew = _pulseOld = nullptr;
  if (!_pulseDirty) return;
  _pulseDirty = false;
  _diffRefValid = false;
  _needFullClear = true;
}

// PTL (0x90): the full screen, or a game's changed rectangle while
// `_windowActive`. Gate coords carry the 120-gate visible offset; x is
// byte-aligned.
void Uc8279X4Driver::sendPartialWindow(EpdBus& bus) {
  uint16_t xStart = 0;
  uint16_t xEnd = _w - 1;
  uint16_t yStart = _cfg.gateOffset;
  uint16_t yEnd = _cfg.gateOffset + _h - 1;
  if (_windowActive) {  // xteink-pokemon
    xStart = _winByte0 * 8;
    xEnd = _winByte1 * 8 + 7;
    yStart = _cfg.gateOffset + _winRow0;
    yEnd = _cfg.gateOffset + _winRow1;
  }
  bus.cmd(CMD_PARTIAL_WINDOW);
  bus.data(static_cast<uint8_t>(xStart >> 8));
  bus.data(static_cast<uint8_t>(xStart & 0xF8));
  bus.data(static_cast<uint8_t>(xEnd >> 8));
  bus.data(static_cast<uint8_t>(xEnd | 0x07));
  bus.data(static_cast<uint8_t>(yStart >> 8));
  bus.data(static_cast<uint8_t>(yStart & 0xFF));
  bus.data(static_cast<uint8_t>(yEnd >> 8));
  bus.data(static_cast<uint8_t>(yEnd & 0xFF));
  bus.data(0x01);
}

// xteink-pokemon: the bounding rectangle (visible rows, bytes) where `fb`
// differs from `_grayBase`, the frame both RAM planes hold; false if none.
bool Uc8279X4Driver::findChangedWindow(const uint8_t* fb) {
  int row0 = -1;
  int row1 = -1;
  int byte0 = _wb;
  int byte1 = -1;
  for (uint16_t r = 0; r < _h; ++r) {
    const uint8_t* a = fb + static_cast<uint32_t>(r) * _wb;
    const uint8_t* b = _grayBase + static_cast<uint32_t>(r) * _wb;
    if (memcmp(a, b, _wb) == 0) continue;
    if (row0 < 0) row0 = r;
    row1 = r;
    for (int i = 0; i < byte0; ++i) {
      if (a[i] != b[i]) {
        byte0 = i;
        break;
      }
    }
    for (int i = _wb - 1; i > byte1; --i) {
      if (a[i] != b[i]) {
        byte1 = i;
        break;
      }
    }
  }
  if (row0 < 0) return false;
  _winRow0 = static_cast<uint16_t>(row0);
  _winRow1 = static_cast<uint16_t>(row1);
  _winByte0 = static_cast<uint16_t>(byte0);
  _winByte1 = static_cast<uint16_t>(byte1);
  return true;
}

// xteink-pokemon: streams the changed rectangle of `fb` into a RAM plane; the
// controller must be in partial mode with that window set. Rows go out
// gathered into 1 KB writes.
void Uc8279X4Driver::streamWindow(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb) {
  uint8_t buf[1024];
  uint16_t used = 0;
  const uint16_t bytes = _winByte1 - _winByte0 + 1;
  bus.cmd(ramCmd);
  bus.beginTxn();
  for (uint16_t r = _winRow0; r <= _winRow1; ++r) {
    if (used + bytes > sizeof(buf)) {
      bus.rawWriteBytes(buf, used);
      used = 0;
    }
    memcpy(buf + used, fb + static_cast<uint32_t>(r) * _wb + _winByte0, bytes);
    used = static_cast<uint16_t>(used + bytes);
  }
  if (used) bus.rawWriteBytes(buf, used);
  bus.endTxn();
}

void Uc8279X4Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8279x4_DRF");
  _timings.waveformUs = phaseUs();  // xteink-pokemon
  if (_windowActive) {
    // xteink-pokemon: a changed-rectangle refresh syncs only that rectangle,
    // still in partial mode.
    sendPartialWindow(bus);
    streamWindow(bus, CMD_DTM1, _grayBase);
    bus.cmd(CMD_PARTIAL_OUT);
    _windowActive = false;
  } else {
    if (_pendingPartial) bus.cmd(CMD_PARTIAL_OUT);

    // Sync the OLD plane (0x10) with the just-displayed frame so the NEXT partial
    // diffs against it (same ghosting management as the UC8179 sibling).
    // xteink-pokemon: in game mode from the frame's snapshot, which DTM2 got too,
    // so that both planes match `_grayBase` for the next changed rectangle.
    const bool keepRef = _diffWindow && _grayBaseValid && _grayBase != nullptr;
    streamPlane(bus, CMD_DTM1, keepRef ? _grayBase : fb);
    _diffRefValid = keepRef;
  }
  _timings.oldPlaneUs = phaseUs();  // xteink-pokemon
  _timings.refreshes = _lastTimings.refreshes + 1;
  _timings.skipped = _lastTimings.skipped;
  _lastTimings = _timings;
  _oldPlaneValid = true;
  _needFullClear = false;

  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4_POF");
    _isScreenOn = false;
  }
}

void Uc8279X4Driver::requestResync(uint8_t settlePasses) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  _grayImagePass = false;
  _absoluteInput = false;
  _directGrayPass = false;
  (void)settlePasses;
  _needFullClear = true;
}

void Uc8279X4Driver::skipInitialResync() { _needFullClear = false; }

void Uc8279X4Driver::deepSleep(EpdBus& bus) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  _directGrayOnPanel = false;
  _grayImagePass = false;
  _absoluteInput = false;
  _directGrayPass = false;
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// --- 4-level grayscale (anti-aliasing) --------------------------------------
// Absolute-plane scheme ported from the UC8179 sibling (see the header). The
// delta masks are folded into stock's absolute selectors and sent INVERTED, so
// black=(0,0) and white=(1,1) are distinct buckets (the earlier raw-delta path
// conflated them → white-text ghosting). plane0/LSB -> DTM1 (0x10),
// plane1/MSB -> DTM2 (0x13).
GrayscaleCapabilities Uc8279X4Driver::grayscaleCapabilities(GrayscaleMode mode) const {
  // LUT_VER 0x67: stock's panel LUT registry (X4 Pro 260917 build) recognizes
  // the id but carries no external-LUT tables for it and excludes it from the
  // ZHX fallback — that panel refreshes from OTP only, with no grayscale path.
  // Report unsupported so callers render B/W instead of driving it with
  // another vendor's waveform.
  if (BoardConfig::ACTIVE.displayControllerVariant == 0x67) return {};
  if (mode == GrayscaleMode::Absolute || mode == GrayscaleMode::Direct)
    return {GrayscaleEncoding::AbsolutePlanes,
            mode == GrayscaleMode::Direct ? GrayscaleBase::Combined : GrayscaleBase::Separate, false, false, false};
  if (mode != GrayscaleMode::Overlay) return {};
  return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, false, false, false};
}

void Uc8279X4Driver::beginGrayscale(EpdBus& bus, const uint8_t* fb, GrayscaleMode mode, RefreshMode fallback,
                                    bool turnOff) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  _grayImagePass = false;
  _absoluteInput = false;
  _directGrayPass = false;
  if (mode == GrayscaleMode::Direct) {
    _absoluteInput = true;
    _directGrayPass = true;
    _grayBaseValid = false;
    _absoluteGrayPlanes = false;
    _oldPlaneValid = false;
    _needFullClear = true;
    _redriveAfterGray = false;
    return;
  }
  displayGrayscaleBase(bus, fb, fallback, turnOff);
  _absoluteInput = mode == GrayscaleMode::Absolute;
}

void Uc8279X4Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  if (!lsb) return;
  if (_absoluteInput) {
    bus.waitBusy(" absolute plane");
    streamPlane(bus, CMD_DTM1, lsb, true);
    return;
  }
  _absoluteGrayPlanes = false;
  if (_grayBaseValid && _grayBase != nullptr) {
    // plane0 = base | maskLsb  (base bit = 1 for white, 0 for non-white).
    for (uint32_t i = 0; i < _bufferSize; i++) _grayBase[i] = static_cast<uint8_t>(_grayBase[i] | lsb[i]);
    streamPlane(bus, CMD_DTM1, _grayBase, /*invert=*/true);
    _absoluteGrayPlanes = true;
  } else {
    streamPlane(bus, CMD_DTM1, lsb, /*invert=*/true);  // fallback: no base snapshot
  }
  _grayBaseValid = false;  // _grayBase now holds absolute plane0, not the B/W base
}

// Grey-mask coverage (percent of panel pixels) above which a gray pass is
// treated as an image and refreshed with the scaled four-tone quality bank.
// Deliberately high: only image-DOMINANT surfaces (sleep screens, standalone
// image pages, full covers — typically 30%+) should cross it. Glyph AA marks
// 1-2%; book pages with modest inline images stay under it and keep stock AA.
#ifndef FREEINK_UC8279X4_QUALITY_COVERAGE_PCT
#define FREEINK_UC8279X4_QUALITY_COVERAGE_PCT 25
#endif

void Uc8279X4Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  if (!msb) return;
  if (_absoluteInput) {
    bus.waitBusy(" absolute plane");
    streamPlane(bus, CMD_DTM2, msb, true);
    return;
  }
  // The MSB mask marks every grey pixel (dark=(1,1), light=(0,1)); its
  // coverage separates image passes from glyph anti-aliasing without a
  // caller-side hint (see displayGray).
  {
    uint32_t grayPx = 0;
    for (uint32_t i = 0; i < _bufferSize; i++) grayPx += __builtin_popcount(msb[i]);
    _grayImagePass = grayPx * 100u > _bufferSize * 8u * FREEINK_UC8279X4_QUALITY_COVERAGE_PCT;
  }
  if (_absoluteGrayPlanes && _grayBase != nullptr) {
    // plane1 = plane0 ^ maskMsb (streamed inverted). Then recover the B/W base
    // for the post-DRF restore: base = plane0 & plane1 = plane0 & (plane0 ^ msb).
    streamPlaneXor(bus, CMD_DTM2, _grayBase, msb, /*invert=*/true);
    for (uint32_t i = 0; i < _bufferSize; i++)
      _grayBase[i] = static_cast<uint8_t>(_grayBase[i] & (_grayBase[i] ^ msb[i]));
    _grayBaseValid = true;
  } else {
    streamPlane(bus, CMD_DTM2, msb, /*invert=*/true);  // fallback
  }
}

void Uc8279X4Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                 bool factoryMode) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  (void)lut;

  // Vendor AA sequence: PSR (REG=1) -> [planes already in RAM via
  // copyGrayscale*] -> 5x49 LUTs -> CDI (constant 0x97) -> PON -> PSR rewrite ->
  // DRF. No CCSET/TSSET writes on this path.
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);  // 0x33: REG=1, external LUT
  bus.data(_cfg.psr1);
  // Complete image planes need four tones; sparse overlay AA keeps the short stock bank.
  if (_directGrayPass || _absoluteInput || factoryMode || _grayImagePass) {
    for (int i = 0; i < 5; i++) {
      bus.cmd(kQualityLutReg[i]);
      bus.data(kQualityBank.data[i], GRAY_LUT_LEN);
    }
  } else {
    for (int i = 0; i < 5; i++) {
      const auto& lut = selectAaLuts()[i];
      bus.cmd(lut.cmd);
      bus.data(lut.data, GRAY_LUT_LEN);
    }
  }
  _grayImagePass = false;
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiAa);  // constant 0x97 every AA refresh (stock; no first/later split)
  _grayRefreshedOnce = true;

  powerOnIfNeeded(bus, " 8279x4_gray_PON");

  // The reference rewrites PSR once more between PON and DRF.
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy((_directGrayPass ? " 8279x4_DIRECT_GRAY_DRF" : " 8279x4_gray"));
  // Vendor production behavior: the UC8279 AA path leaves analog power enabled
  // between page refreshes (the UC8179 sibling does the same — its gray_aa sends
  // no POF). Honor only an explicit turnOff.
  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4_gray_POF");
    _isScreenOn = false;
  }

  // Restore the clean B/W base to BOTH controller planes (stock gray_aa does the
  // same). `_grayBase` was rebuilt to the base = plane0 & plane1 in
  // copyGrayscaleMsb. This keeps the next Fast B/W diff's OLD plane coherent AND
  // stops a stale gray selector plane from being reused by a later refresh — the
  // core of why absolute planes de-ghost where raw delta planes did not.
  (void)fb;
  if (_grayBaseValid && _grayBase != nullptr) {
    streamPlane(bus, CMD_DTM1, _grayBase);
    streamPlane(bus, CMD_DTM2, _grayBase);
    _oldPlaneValid = true;
    _needFullClear = false;
  }
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;

  // AA still leaves intermediate edge charge a plain DU diff can't neutralize;
  // flag the next B/W page to re-drive every pixel to its target (see
  // displayStart), scrubbing residue with a cheap DU — an explicit Half GC
  // remains the strong purge.
  _directGrayOnPanel = _directGrayPass;
  _redriveAfterGray = !_directGrayPass;
  if (_directGrayPass) _needFullClear = true;
  _grayImagePass = false;
  _absoluteInput = false;
  _directGrayPass = false;
}

void Uc8279X4Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  if (!fb) return;
  // Match the UC8179 sibling: an explicit Full/Half clean, or a
  // state that can't run the non-flashing transition (first AA page / no valid
  // previous / pending full clear), takes a real B/W activation via display().
  // Otherwise use stock's non-flashing prev->current transition.
  if (fallback != RefreshMode::Fast || !_grayRefreshedOnce || !_oldPlaneValid || _needFullClear) {
    display(bus, fb, nullptr, fallback, turnOff);
    return;
  }
  transitionGrayscaleBase(bus, fb, turnOff);
}

void Uc8279X4Driver::transitionGrayscaleBase(EpdBus& bus, const uint8_t* fb, bool turnOff) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  if (!fb) return;
  // Snapshot the new B/W base for the AA fold that follows.
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
  if (_grayBase != nullptr) {
    memcpy(_grayBase, fb, _bufferSize);
    _grayBaseValid = true;
  }
  bus.waitBusy(" 8279x4_gray_base_ready");
  // DTM1 retains the previous page's clean B/W base; load the new base into DTM2.
  // The prebw settle waveform drives that transition without an OTP GC flash.
  streamPlane(bus, CMD_DTM2, fb);
  runGrayscalePrecondition(bus);
  // Restore the B/W baseline to DTM1 (an AA upload may overwrite these; the
  // cached snapshot above survives for the fold in copyGrayscale*).
  streamPlane(bus, CMD_DTM1, fb);
  _oldPlaneValid = true;
  _redriveAfterGray = false;
  _needFullClear = false;
  if (turnOff && _isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8279x4_gray_base_POF");
    _isScreenOn = false;
  }
}

// Stock's UC8279_aa_prebw_mid pre-conditioning pass (byte-exact order from
// Factory.bin FUN_4214d3a0): PTIN -> PTL(full window, +120 gate offset) ->
// PSR(REG=1) -> PFS -> gate scan -> CDI 0xD7 -> CCSET -> TSSET(fast) -> upload
// the 5 settle LUTs -> PON -> DRF -> PTOUT. Requires a valid previous page in
// DTM1 and the new base in DTM2.
void Uc8279X4Driver::runGrayscalePrecondition(EpdBus& bus) {
  if (!_oldPlaneValid || !_grayRefreshedOnce) return;
  bus.waitBusy(" 8279x4_gray_pre_ready");

  const uint16_t xEnd = static_cast<uint16_t>(_w - 1);
  const uint16_t yStart = _cfg.gateOffset;
  const uint16_t yEnd = static_cast<uint16_t>(_cfg.gateOffset + _h - 1);
  bus.cmd(CMD_PARTIAL_IN);
  bus.cmd(CMD_PARTIAL_WINDOW);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(static_cast<uint8_t>(xEnd >> 8));
  bus.data(static_cast<uint8_t>(xEnd | 0x07));
  bus.data(static_cast<uint8_t>(yStart >> 8));
  bus.data(static_cast<uint8_t>(yStart & 0xFF));
  bus.data(static_cast<uint8_t>(yEnd >> 8));
  bus.data(static_cast<uint8_t>(yEnd & 0xFF));
  bus.data(0x01);

  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);  // REG=1: run the external settle tables
  bus.data(_cfg.psr1);
  bus.cmd(CMD_PFS);
  bus.data(_cfg.pfs);
  bus.cmd(CMD_GATE_SCAN);
  bus.data(_cfg.gateScan);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiBwFast);  // 0xD7, per FUN_4214d3a0
  bus.cmd(CMD_CCSET);
  bus.data(_cfg.ccset);
  bus.cmd(CMD_TSSET);
  bus.data(_cfg.tssetFast);  // 0x5A
  for (const auto& l : kXtfPreBwMid) {
    bus.cmd(l[0]);
    bus.data(&l[1], PREBW_LUT_LEN);
  }

  powerOnIfNeeded(bus, " 8279x4_gray_pre_PON");
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8279x4_gray_pre_DRF");
  bus.cmd(CMD_PARTIAL_OUT);
}

void Uc8279X4Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  _diffRefValid = false;  // xteink-pokemon: the planes stop matching _grayBase
  _grayImagePass = false;
  _absoluteInput = false;
  _directGrayPass = false;
  _grayBaseValid = false;
  _absoluteGrayPlanes = false;
  if (!bw) {
    _needFullClear = true;
    _oldPlaneValid = false;
    return;
  }
  // Re-seed the OLD plane (0x10) with the clean B/W frame the reader restored —
  // same rationale as the UC8179 sibling. (displayGray already restored the base
  // to both planes; this covers callers that reach cleanup by another route.)
  streamPlane(bus, CMD_DTM1, bw);
  _oldPlaneValid = true;
  // RAM restoration does not cancel a requested physical clean.
}

// Per-board config injection, same idiom as the other drivers: define
// `const Uc8279X4Config& yourConfig();` in namespace freeink and build with
// -DFREEINK_UC8279_X4_CONFIG=yourConfig.
#ifdef FREEINK_UC8279_X4_CONFIG
const Uc8279X4Config& FREEINK_UC8279_X4_CONFIG();
static const Uc8279X4Config& uc8279X4ActiveConfig() { return FREEINK_UC8279_X4_CONFIG(); }
#else
static const Uc8279X4Config& uc8279X4ActiveConfig() {
  // Stock X4C screenType=2 uses the same 800x600/120-gate UC8279 configuration.
  return uc8279X4DefaultConfig();
}
#endif

PanelDriver& uc8279X4Driver() {
  static Uc8279X4Driver instance(uc8279X4ActiveConfig());
  return instance;
}

}  // namespace freeink
