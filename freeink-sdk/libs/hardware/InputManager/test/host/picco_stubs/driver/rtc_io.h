#pragma once
enum { RTC_GPIO_MODE_INPUT_ONLY };
inline int rtc_gpio_init(int) { return 0; }
inline int rtc_gpio_set_direction(int, int) { return 0; }
inline int rtc_gpio_pullup_dis(int) { return 0; }
inline int rtc_gpio_pulldown_en(int) { return 0; }
