// Onyx Picco: panel-ID probe picks the stock per-panel SSD1677 tables.
// Run once per panel (the driver is a process-wide singleton): yrd | depg | opm | unknown.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#define private public
#include "src/driver/Ssd1677Driver.h"
#include "src/driver/Ssd2677Driver.h"
#include "src/lut/Ssd2677Luts.h"
#include "src/lut/Ssd1677Luts.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static const Bytes* lastLut(const EpdBus& bus) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == 0x32) return &i->bytes;
  return nullptr;
}

static void expectRefresh(Ssd1677Driver& d, RefreshMode mode, uint8_t ctrl2, const unsigned char* lut) {
  EpdBus bus;
  const Bytes fb(48000, 0xAA);
  d.display(bus, fb.data(), fb.data(), mode, false);
  assert(lastRegister(bus, 0x22) == ctrl2);
  const Bytes* sent = lastLut(bus);
  if (lut) {
    assert(sent && *sent == Bytes(lut, lut + 105));
    assert(lastRegister(bus, 0x2C) == lut[109]);
  } else {
    assert(!sent);
  }
  assert(!d._customLutActive);
}

// AA follows the Sticky overlay path (its LUT + voltages, border parked at VCOM,
// power-up first, 0xCC). Absolute uses the default X4 factory LUT.
static void expectStickyGray(Ssd1677Driver& d) {
  const unsigned char* lut = lut_grayscale_sticky;
  assert(d.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
  assert(d.grayscaleCapabilities(GrayscaleMode::Absolute).supported() && d._cfg.factoryGrayLut == nullptr);
  EpdBus bus;
  const Bytes fb(48000, 0xFF);
  d.displayGray(bus, fb.data(), false, nullptr, false);
  const Bytes* sent = lastLut(bus);
  assert(sent && *sent == Bytes(lut, lut + 105));
  assert(lastRegister(bus, 0x2C) == lut[109] && lastRegister(bus, 0x3C) == 0x80);
  assert(lastRegister(bus, 0x22) == 0xCC);
  assert(!d._customLutActive);
  bus.clear();
  d.displayGray(bus, fb.data(), false, nullptr, true);
  const Bytes* factory = lastLut(bus);
  assert(factory && *factory == Bytes(lut_factory_quality, lut_factory_quality + 105));
  assert(lastRegister(bus, 0x2C) == lut_factory_quality[109] && lastRegister(bus, 0x22) == 0xCC);
}

int main(int argc, char** argv) {
  const char* which = argc > 1 ? argv[1] : "yrd";
  static const uint8_t yrd[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x02, 0x10, 0x01, 0x00, 0x91};
  static const uint8_t depg[10] = {0xCA, 0xFE, 0x00, 0x16, 0x77, 0x00, 0x08, 0x01, 0x00, 0x4E};
  static const uint8_t opm[10] = {'W', '0', '4', '0', 'B', '3', 0, 0, 0, 0};
  static const uint8_t none[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t* id = !strcmp(which, "depg") ? depg : !strcmp(which, "opm") ? opm : !strcmp(which, "unknown") ? none : yrd;
  memcpy(fakePanelId, id, 10);
  if (!strcmp(which, "se0400")) {
    static const uint8_t rev[3] = {0x07, 0x01, 0x01};
    memcpy(fakePanelId, none, 10);
    memcpy(fakePanelRev, rev, 3);
    assert(piccoProbePanel() == PiccoSe0400nqw47);
    auto& s = ssd2677Driver();
    EpdBus bus;
    s.begin(bus);
    assert(lastRegister(bus, 0x00) == 0x27 && lastRegister(bus, 0xE9) == 0x01);
    Bytes oldFb(48000, 0xFF), newFb(48000, 0xFF);
    newFb[0] = 0x0F;  // left 4 px black, right 4 px white; old all white
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, false);
    // First paint is forced FULL; data is old(hi)/new(lo) pairs, MSB first.
    const Bytes* data = nullptr;
    for (const auto& w : bus.writes) if (w.command == 0x10) data = &w.bytes;
    assert(data && data->size() == 96000);
    assert((*data)[0] == 0xAA && (*data)[1] == 0xFF);  // (1,0)x4 then (1,1)x4
    const Bytes* lut = nullptr;
    for (const auto& w : bus.writes) if (w.command == 0x20) lut = &w.bytes;
    assert(lut && *lut == Bytes(lut_picco_se0400_full, lut_picco_se0400_full + 535));
    assert(lastRegister(bus, 0x12) == 0x00);
    bus.clear();
    s.display(bus, newFb.data(), oldFb.data(), RefreshMode::Fast, true);
    for (const auto& w : bus.writes) if (w.command == 0x20) lut = &w.bytes;
    assert(*lut == Bytes(lut_picco_se0400_fast, lut_picco_se0400_fast + 535));
    assert(lastRegister(bus, 0x02) == 0x00);
    // Overlay AA: LSB mask -> low bit, MSB mask -> high bit, slot-5 LUT.
    assert(s.grayscaleCapabilities(GrayscaleMode::Overlay).supported());
    assert(!s.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    Bytes lsb(48000, 0x00), msb(48000, 0x00);
    lsb[0] = 0xF0;  // px 0-3 LSB
    msb[0] = 0xCC;  // px 0,1,4,5 MSB
    bus.clear();
    s.copyGrayscaleLsb(bus, lsb.data());
    s.copyGrayscaleMsb(bus, msb.data());
    const Bytes* g = nullptr;
    for (const auto& w : bus.writes) if (w.command == 0x10) g = &w.bytes;
    // px: (msb,lsb) = 11 11 01 01 | 10 10 00 00
    assert(g && g->size() == 96000 && (*g)[0] == 0xF5 && (*g)[1] == 0xA0);
    s.displayGray(bus, newFb.data(), false, nullptr, false);
    for (const auto& w : bus.writes) if (w.command == 0x20) lut = &w.bytes;
    assert(*lut == Bytes(lut_picco_se0400_gray, lut_picco_se0400_gray + 535));
    s.deepSleep(bus);
    assert(lastRegister(bus, 0x07) == 0xA5);
    std::printf("Picco se0400: probe, init, packing and refresh passed\n");
    return 0;
  }

  auto& d = static_cast<Ssd1677Driver&>(ssd1677Driver());
  // Sleep latches the display pins; the probe must release them before driving RESET.
  assert(fakeHoldReleased[BoardConfig::ACTIVE.display.rst]);
  // The probe issued Read User ID; an unrecognized ID goes on to CMD 0x70.
  assert(fakePanelCommand == (strcmp(which, "unknown") ? 0x2E : 0x70));
  const uint8_t variant = BoardConfig::ACTIVE.displayControllerVariant;
  EpdBus bus;
  d.begin(bus);
  // First paint is the absolute FULL; then exercise each mode.
  if (!strcmp(which, "yrd") || !strcmp(which, "opm")) {
    assert(variant == (!strcmp(which, "yrd") ? 1 : 3));
    assert(lastRegister(bus, 0x3C) == (!strcmp(which, "yrd") ? 0x01 : 0x03));
    expectRefresh(d, RefreshMode::Full, 0xF7, nullptr);
    expectRefresh(d, RefreshMode::Half, 0xD7, nullptr);
    expectRefresh(d, RefreshMode::Fast, 0xCC, lut_picco_fast);
    expectStickyGray(d);
  } else if (!strcmp(which, "depg")) {
    assert(variant == 2);
    expectRefresh(d, RefreshMode::Full, 0xE4, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Half, 0xC4, lut_picco_depg_full);
    expectRefresh(d, RefreshMode::Fast, 0xCC, lut_picco_depg_fast);
    expectStickyGray(d);
  } else {
    assert(variant == 0);
    assert(d._cfg.fullLut == nullptr && d._cfg.fastLut == nullptr && d._cfg.grayLut == lut_grayscale_sticky);
  }
  std::printf("Picco %s: probe, config and refresh sequences passed\n", which);
  return 0;
}
