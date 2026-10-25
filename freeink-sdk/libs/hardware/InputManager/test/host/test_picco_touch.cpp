// Picco TMA525C (Parade PIP): mode check, bootloader launch, touch decode, lift-off.
#include <cassert>
#include <cstdio>

#include <Wire.h>
#include <InputManager.h>

static std::vector<uint8_t> touchReport(uint16_t x, uint16_t y, uint8_t event, uint8_t count = 1) {
  return {17, 0, 0x01, 0, 0, count, 0, 0x00, static_cast<uint8_t>(event << 5), static_cast<uint8_t>(x),
          static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8), 0, 0, 0, 0};
}

int main() {
  // Bootloader at power-up: the driver must send launch-app with the PIP CRC.
  gpioLevels[2] = gpioLevels[5] = gpioLevels[9] = gpioLevels[21] = gpioLevels[6] = 1;
  Wire.mode = 0xFF;
  Wire.sync();
  InputManager in;
  in.begin();
  assert(in.hasTouch());
  bool launched = false;
  for (const auto& w : Wire.writes)
    if (w.size() == 13 && w[7] == 0x3B) {
      launched = true;
      assert(w[10] == 0x20 && w[11] == 0xC7 && w[12] == 0x17);  // CRC-16/CCITT-FALSE(01 3B 00 00) = 0xC720
    }
  assert(launched);

  // Finger down at raw (100, 200): stock orientation (480-100, 800-200) = (380, 600),
  // swapXY onto the 800x480 panel frame -> (600, 380), flipY (hardware-confirmed) -> (600, 99).
  Wire.push(touchReport(100, 200, 0));
  fakeNow += 20;
  in.update();
  assert(in.wasTouchPressed() && in.isTouchPressed());
  auto p = in.getTouchPoint();
  assert(p.valid && p.x == 600 && p.y == 99);

  // Lift-off record releases.
  Wire.push(touchReport(100, 200, 3));
  fakeNow += 20;
  in.update();
  assert(!in.isTouchPressed() && in.wasTouchReleased());

  // A still finger may stop producing reports: the touch stays down and long
  // press fires; only a missed lift-off is released, after the safety net.
  Wire.push(touchReport(240, 400, 0));
  fakeNow += 20;
  in.update();
  assert(in.isTouchPressed());
  bool longPress = false;
  for (int i = 0; i < 30; ++i) {
    fakeNow += 20;
    in.update();
    float nx, ny;
    longPress |= in.wasTouchLongPress(nx, ny);
  }
  assert(in.isTouchPressed() && longPress);
  fakeNow += 3100;
  in.update();
  assert(!in.isTouchPressed());
  std::puts("Picco TMA525C touch passed");
}
