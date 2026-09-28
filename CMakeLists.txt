# stock_watch -- A股行情监看 (大盘 + 个股K线) for ESP32-S3-LCD-1.3
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stock_watch)

# ---------------------------------------------------------------------------
# Workaround: GCC 14.2 (xtensa-esp-elf) can crash while compiling ESP-IDF's RGB
# LCD driver at the project default -Og:
#     during RTL pass: ira
#     esp_lcd_panel_rgb.c: In function 'rgb_panel_draw_bitmap':
#     internal compiler error: Segmentation fault
# That source is only pulled in for targets with an RGB LCD peripheral
# (CONFIG_SOC_LCDCAM_RGB_LCD_SUPPORTED, e.g. esp32s3). This board drives the
# panel over SPI (ST7789), so the RGB driver is dead code here: compile just
# that one file at -O2 (-Wno-error so a new -O2 warning can't fail the build).
# Remove this block once ESP-IDF is upgraded to a version that does not ICE.
# ---------------------------------------------------------------------------
if(TARGET __idf_esp_lcd)
    set_source_files_properties(
        "${IDF_PATH}/components/esp_lcd/rgb/esp_lcd_panel_rgb.c"
        TARGET_DIRECTORY __idf_esp_lcd
        PROPERTIES COMPILE_OPTIONS "-O2;-Wno-error")
endif()
