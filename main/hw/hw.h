/*
 * hw.h -- battery ADC and WS2812 status LED for small_tv.
 * Reused from weather_clock_lvgl.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void  hw_battery_init(void);
float hw_battery_read_voltage(void);
int   hw_battery_level_percent(void);

void  hw_led_init(void);
void  hw_led_set(uint8_t r, uint8_t g, uint8_t b);
void  hw_led_off(void);

#ifdef __cplusplus
}
#endif
