// Slide-switch level + debounced change edge on the Picco (GPIO6, on = LOW).
#include <cassert>
#include <cstdio>

#include <InputManager.h>

int main() {
  constexpr int SW = 6;
  gpioLevels[2] = gpioLevels[5] = gpioLevels[9] = gpioLevels[21] = HIGH;  // buttons idle
  gpioLevels[SW] = LOW;  // boots switched on
  InputManager in;
  in.begin();
  assert(in.hasToggleSwitch() && in.isToggleSwitchOn());
  in.update();
  assert(!in.wasToggleSwitchChanged());  // boot position is not a change

  gpioLevels[SW] = HIGH;  // a bounce shorter than the debounce window is ignored
  in.update();
  fakeNow += 2;
  gpioLevels[SW] = LOW;
  in.update();
  fakeNow += 10;
  in.update();
  assert(in.isToggleSwitchOn() && !in.wasToggleSwitchChanged());

  gpioLevels[SW] = HIGH;  // a held change commits once, after the window
  in.update();
  assert(in.isToggleSwitchOn());
  fakeNow += 10;
  in.update();
  assert(!in.isToggleSwitchOn() && in.wasToggleSwitchChanged());
  in.update();
  assert(!in.wasToggleSwitchChanged());
  std::puts("Picco toggle switch passed");
}
