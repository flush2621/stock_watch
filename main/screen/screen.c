/*
 * screen.c -- ST7789 240x240 display bound to LVGL 9 via esp_lvgl_port.
 * Same wiring as weather_clock_lvgl. See app_config.h for pin map.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "screen.h"
#include "app_config.h"

static const char *TAG = "screen";
static esp_lcd_panel_handle_t s_panel = NULL;

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    screen_set_backlight(100);
}

void screen_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void *screen_init(void)
{
    backlight_init();

    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_RES),
    };
    gpio_config(&io);

    gpio_set_level(PIN_LCD_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_LCD_SPI_CLK,
        .mosi_io_num     = PIN_LCD_SPI_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * (LCD_V_RES / 4) * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num         = PIN_LCD_DC,
        .cs_gpio_num         = PIN_LCD_CS,
        .pclk_hz             = LCD_SPI_CLK_HZ,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 10,
        .on_color_trans_done = NULL,
        .user_ctx            = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RES,
        .color_space    = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLORS));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = s_panel,
        .buffer_size   = LCD_H_RES * (LCD_V_RES / 6),
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation      = {
            .swap_xy  = LCD_SWAP_XY,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
        .flags = {
            .buff_spiram = false,
            .buff_dma    = true,
            .swap_bytes  = true,   /* ST7789 SPI expects big-endian RGB565 */
        }
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }

    ESP_LOGI(TAG, "screen ready (LVGL 9, %dx%d)", LCD_H_RES, LCD_V_RES);
    return disp;
}
