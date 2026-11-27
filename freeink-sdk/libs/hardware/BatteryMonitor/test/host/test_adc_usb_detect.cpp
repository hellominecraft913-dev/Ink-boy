// Boards with a usbDetect pin but no charger IC can still report external power:
// the pin is the input rail's presence signal. The PaperS3 needs exactly this —
// with a full battery the charger stops (isCharging() goes false) while the cable
// stays plugged in, so isCharging() alone cannot answer "am I on USB?".
//
// The polarity matters: the PaperS3's GPIO5 is actively driven LOW while a cable
// is attached (measured on hardware), which is why BoardProfile carries
// usbDetectActiveHigh and the pin is read with the pull that puts a disconnected
// line in the absent state.

#include <Arduino.h>  // the host stub (hostDigitalPin)
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <vector>  // BoardConfig.h range-for over a pin list

#include <BatteryMonitor.h>
#include <BoardConfig.h>

// The stub I2C bus has nothing on it: the charger/gauge path answers "not
// observed", which is exactly the PaperS3 situation, so the code must fall
// through to the detect pin.
bool hostI2cRead(uint8_t, uint8_t, uint8_t*, size_t) { return false; }
bool hostI2cWrite(uint8_t, const std::vector<uint8_t>&) { return false; }

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

}  // namespace

int main() {
  // PaperS3: usbDetect GPIO5, active-low, no gauge, no charger.
  BoardConfig::ACTIVE = BoardConfig::M5PAPER_S3;
  BatteryMonitor monitor;

  bool known = false;
  hostDigitalPin = LOW;  // driven low with a cable attached
  check(monitor.isExternalPowerPresent(&known), "active-low board: cable in -> present");
  check(known, "active-low board: cable in -> known");

  hostDigitalPin = HIGH;  // pulled up when nothing is connected
  known = true;
  check(!monitor.isExternalPowerPresent(&known), "active-low board: no cable -> absent");
  check(known, "active-low board: no cable -> known");

  // The default polarity (documented "HIGH = USB present") still reads the other
  // way round, so boards that rely on it are unaffected.
  BoardConfig::ACTIVE = BoardConfig::M5PAPER_S3;
  BoardConfig::ACTIVE.usbDetectActiveHigh = true;
  hostDigitalPin = HIGH;
  check(monitor.isExternalPowerPresent(&known), "active-high board: HIGH -> present");
  hostDigitalPin = LOW;
  check(!monitor.isExternalPowerPresent(&known), "active-high board: LOW -> absent");

  // A board with no detect pin keeps answering "no opinion".
  BoardConfig::ACTIVE.usbDetect = BatteryMonitor::PIN_NONE;
  known = true;
  check(!monitor.isExternalPowerPresent(&known), "no detect pin -> false");
  check(!known, "no detect pin -> unknown");

  // Both X3 panels share SDA20/SCL0. SDA must never become a USB input.
  for (const auto& profile : {BoardConfig::XTEINK_X3, BoardConfig::XTEINK_X3_UC8279}) {
    BoardConfig::ACTIVE = profile;
    BatteryMonitor x3Monitor;
    std::printf("  profile: %s\n", profile.name);
    check(profile.usbDetect == BoardConfig::PIN_UNASSIGNED, "X3: no dedicated USB-detect pin");
    check(profile.batteryGauge.i2cSda == 20 && profile.batteryGauge.i2cScl == 0, "X3: gauge keeps SDA20/SCL0");
    for (int level : {LOW, HIGH}) {
      hostDigitalPin = level;
      hostPinModeCalls = 0;
      hostDigitalReadCalls = 0;
      known = true;
      check(!x3Monitor.isExternalPowerPresent(&known), "X3: no external-power observation");
      check(!known, "X3: caller must fall back to gauge charging status");
      check(hostPinModeCalls == 0, "X3: USB check does not configure GPIO");
      check(hostDigitalReadCalls == 0, "X3: USB check does not read GPIO");
    }
  }

  std::printf(failures == 0 ? "all usb-detect checks passed\n" : "%d check(s) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
