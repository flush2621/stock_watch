/*
 * sdcard.h -- Micro-SD card (SPI3) + FATFS mount for small_tv.
 *
 * The ESP32-S3-LCD-1.3 board has a TF card holder wired to SPI3:
 *   SD_MISO=GPIO16, SD_CS=GPIO17, SD_MOSI=GPIO18, SD_CLK=GPIO21
 * (extracted from the official schematic ESP32S3_1.3inch.pdf).
 *
 * Used by the HoloCubic-style butterfly boot animation: full-screen frames
 * are too big for flash, so they live on the SD card as LVGL-9 RGB565 .bin
 * files and are streamed in via LVGL's POSIX filesystem driver.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init SPI3 + SDSPI + FATFS, mount at SD_MOUNT_POINT ("/sdcard").
 *  Returns false (and logs) if no card is present -- the rest of the
 *  firmware keeps running, only the SD-backed boot animation is skipped. */
bool sdcard_init(void);

/** True if sdcard_init() mounted a card. */
bool sdcard_is_mounted(void);

#ifdef __cplusplus
}
#endif
