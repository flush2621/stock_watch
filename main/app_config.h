/*
 * app_config.h -- board pin definitions and app constants.
 * Board: ESP32-S3-LCD-1.3 (ESP32-S3R8 module, Waveshare-style)
 *  - 1.3" 240x240 ST7789 LCD (SPI)
 *  - QMI8658 6-axis IMU (I2C, shared with SD)
 *  - WS2812 status LED (RMT)
 *  - PL4054 Li-Ion charger + VBAT / ADC divider
 *  - CH343 USB-Serial (UART0)
 *
 * 本工程: A股行情监看 (大盘指数 + 个股行情/K线)
 * 数据源(免费接口, UTF-8 JSON):
 *   - 大盘/个股实时: 东方财富 push2.eastmoney.com
 *   - 个股日K线:     腾讯 web.ifzq.gtimg.cn
 */
#pragma once

/* ---------------- LCD (ST7789VW, SPI2) ---------------- */
#define PIN_LCD_SPI_MOSI   41
#define PIN_LCD_SPI_CLK    40
#define PIN_LCD_DC         38
#define PIN_LCD_CS         39
#define PIN_LCD_RES        42
#define PIN_LCD_BL         20        /* backlight (PWM) */

#define LCD_H_RES           240
#define LCD_V_RES           240
#define LCD_INVERT_COLORS   1
#define LCD_MIRROR_X        0
#define LCD_MIRROR_Y        0
#define LCD_SWAP_XY         0    /* 1 = rotate 90/270 deg */
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_SPI_CLK_HZ      (40 * 1000 * 1000)

/* ---------------- QMI8658 IMU (I2C0) ---------------- */
#define PIN_IMU_SDA         47
#define PIN_IMU_SCL         48
#define PIN_IMU_INT1        46
#define PIN_IMU_INT2        45
#define IMU_I2C_PORT        0
#define IMU_I2C_HZ          100000
#define IMU_ADDR_PRI        0x6B
#define IMU_ADDR_SEC        0x6A

/* ---------------- Battery ADC ---------------- */
#define PIN_BAT_ADC         ADC_CHANNEL_5   /* GPIO6 = ADC1_CH5 */
#define PIN_BAT_GPIO        6
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12
#define BAT_V_FULL          4.20f
#define BAT_V_EMPTY         3.30f

/* ---------------- WS2812 RGB status LED ---------------- */
#define PIN_WS2812          15

/* ---------------- Micro-SD card (SPI3, FATFS @ /sdcard) ---------------- */
#define PIN_SD_MISO         16
#define PIN_SD_CS           17
#define PIN_SD_MOSI         18
#define PIN_SD_CLK          21
#define SD_SPI_HOST         SPI3_HOST
#define SD_MOUNT_POINT      "/sdcard"
#define SD_SPI_HZ           (40 * 1000 * 1000)

/* ---------------- WiFi credentials ---------------- */
#define WIFI_SSID           "XX"
#define WIFI_PASS           "xxxxxxxx"

/* ---------------- 股票行情 ---------------- */
/* SD 卡里的股票代码文件: /sdcard/stock/code.txt (一行一个代码) */
#define STOCK_CODE_PATH     SD_MOUNT_POINT "/stock/code.txt"

/* 最多几只个股(总页数 = 1 大盘 + N 个股, 上限 5 页 => 最多 4 只) */
#define STOCK_MAX           4

/* 刷新周期(秒) */
#define QUOTE_REFRESH_S     30      /* 大盘+个股实时行情 */
#define KLINE_REFRESH_S     300     /* 个股日K线 */

/* 东财大盘指数(固定 4 个): 上证/深成/创业板/沪深300 */
#define INDEX_COUNT         4
#define INDEX_SECIDS        "1.000001,0.399001,0.399006,1.000300"

/* ---------------- App manager ---------------- */
#define APP_TRANSITION_MS   350
#define APP_TILT_DEG        22.0f
#define APP_TILT_DEBOUNCE   500
