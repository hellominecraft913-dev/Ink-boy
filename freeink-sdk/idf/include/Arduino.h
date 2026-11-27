#pragma once

#if defined(ARDUINO)
#error "FreeInk's ESP-IDF Arduino shim must not be built with arduino-esp32; use the real core's headers."
#endif

// FreeInk SDK — the slice of the Arduino API FreeInk uses, on plain ESP-IDF.
//
// Only on the include path of the ESP-IDF component (CMakeLists.txt at the
// repo root), so the SDK sources build unchanged without arduino-esp32.
// Arduino builds never see this file.

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIGH 1
#define LOW 0

#define INPUT 0x01
#define OUTPUT 0x03
#define PULLUP 0x04
#define INPUT_PULLUP 0x05
#define PULLDOWN 0x08
#define INPUT_PULLDOWN 0x09
#define OPEN_DRAIN 0x10
#define OUTPUT_OPEN_DRAIN 0x13

#define RISING 0x01
#define FALLING 0x02
#define CHANGE 0x03

#define PROGMEM
#define PSTR(s) (s)
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))

#define digitalPinToInterrupt(p) (p)

void pinMode(uint8_t pin, uint8_t mode);
void attachInterrupt(uint8_t pin, void (*isr)(void), int mode);
void detachInterrupt(uint8_t pin);

inline void digitalWrite(uint8_t pin, uint8_t val) { gpio_set_level(static_cast<gpio_num_t>(pin), val); }
inline int digitalRead(uint8_t pin) { return gpio_get_level(static_cast<gpio_num_t>(pin)); }

inline unsigned long millis() { return static_cast<unsigned long>(esp_timer_get_time() / 1000); }
inline unsigned long micros() { return static_cast<unsigned long>(esp_timer_get_time()); }
inline void delayMicroseconds(uint32_t us) { esp_rom_delay_us(us); }
// Like arduino-esp32: at least one tick, so delay(1) in a poll loop yields.
inline void delay(uint32_t ms) {
  const TickType_t t = pdMS_TO_TICKS(ms);
  vTaskDelay(t ? t : 1);
}
inline void yield() { taskYIELD(); }

inline void* ps_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// Serial / Serial0: log output to the ESP-IDF console (stdout).
class HardwareSerial {
 public:
  void begin(unsigned long) {}
  explicit operator bool() const { return true; }
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  size_t print(const char* s) { return fputs(s, stdout) < 0 ? 0 : strlen(s); }
  size_t println(const char* s = "") { return print(s) + print("\n"); }
  void flush() { fflush(stdout); }
};
extern HardwareSerial Serial, Serial0;
