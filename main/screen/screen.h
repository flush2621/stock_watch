/*
 * screen.h -- ST7789 display init for small_tv.
 * Reused/adapted from weather_clock_lvgl.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize LCD + LVGL. Returns the lv_display_t* (as void*) or NULL. */
void *screen_init(void);

/** Set backlight percent (0..100). */
void screen_set_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif
