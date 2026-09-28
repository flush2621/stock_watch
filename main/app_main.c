/*
 * app_main.c -- A股行情监看 入口。
 *
 * Boot flow:
 *   1. NVS -> battery ADC -> WS2812
 *   2. QMI8658 IMU probe (可选, 失败仍可静态翻页)
 *   3. SD 卡挂载 -> 读取 /stock/code.txt
 *   4. LCD + LVGL
 *   5. 注册 App: 大盘(1) + 个股(N≤4) => 总页数 ≤ 5
 *   6. 启动 App 管理器(IMU 翻页) + 行情刷新任务
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "screen/screen.h"
#include "hw/hw.h"
#include "hw/imu.h"
#include "hw/sdcard.h"
#include "net/net.h"
#include "stock/stock.h"
#include "appman/appman.h"
#include "apps/apps.h"

static const char *TAG = "stock_watch";

/* 从 code.txt 读出的股票代码(最多 STOCK_MAX 只) */
static char s_codes[STOCK_MAX][STOCK_CODE_LEN];
static int  s_code_count = 0;
/* 每只股票的K线是否已拿到(没拿到就每轮重试, 而不是等 5 分钟) */
static bool s_kline_ok[STOCK_MAX];

static void stock_refresh_task(void *arg)
{
    (void)arg;

    app_market_set_updated(false);
    for (int i = 0; i < s_code_count; i++) app_stock_set_updated(i, false);

    if (!net_wifi_connect(WIFI_SSID, WIFI_PASS, 25)) {
        ESP_LOGE(TAG, "WiFi 连接失败, 行情不可用");
        hw_led_set(8, 0, 0);
        vTaskDelete(NULL);
        return;
    }
    hw_led_set(0, 8, 0);
    net_ntp_sync();

    int kline_cycle = KLINE_REFRESH_S / QUOTE_REFRESH_S;
    if (kline_cycle < 1) kline_cycle = 1;
    int cycle = 0;

    while (1) {
        /* 大盘指数 */
        index_quote_t idx[INDEX_COUNT] = {0};
        int n = stock_fetch_indices(idx, INDEX_COUNT);
        if (n > 0) {
            app_market_set_indices(idx, n);
            app_market_set_updated(true);
        } else {
            app_market_set_updated(false);
        }

        /* 每只个股: 实时行情每周期拉, K线隔几个周期拉 */
        for (int i = 0; i < s_code_count; i++) {
            stock_quote_t q;
            if (stock_fetch_quote(s_codes[i], &q)) {
                app_stock_set_quote(i, &q);
                app_stock_set_updated(i, true);
            } else {
                app_stock_set_updated(i, false);
            }

            if (cycle == 0 || !s_kline_ok[i]) {
                kline_t k;
                if (stock_fetch_kline(s_codes[i], &k)) {
                    s_kline_ok[i] = true;
                    app_stock_set_kline(i, &k);
                } else {
                    /* 失败时图上显示"K线获取失败" + 各数据源原因, 下一轮(30s)自动重试 */
                    app_stock_set_kline_failed(i, stock_kline_error());
                }
            }
        }

        cycle = (cycle + 1) % kline_cycle;
        vTaskDelay(pdMS_TO_TICKS(QUOTE_REFRESH_S * 1000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    hw_battery_init();
    hw_led_init();
    hw_led_set(2, 2, 8);   /* dim blue = booting */

    vTaskDelay(pdMS_TO_TICKS(100));
    imu_init();
    sdcard_init();

    ESP_LOGI(TAG, "=====================================================");
    ESP_LOGI(TAG, "stock_watch BUILD %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "IMU present: %s", imu_is_present() ? "YES" : "NO");
    ESP_LOGI(TAG, "SD mounted: %s", sdcard_is_mounted() ? "YES" : "NO");
    ESP_LOGI(TAG, "=====================================================");

    if (!screen_init()) {
        ESP_LOGE(TAG, "screen init failed");
        return;
    }

    /* 读股票代码 */
    s_code_count = stock_load_codes(s_codes, STOCK_MAX);
    ESP_LOGI(TAG, "股票代码 %d 只, 总页数 %d", s_code_count, 1 + s_code_count);

    /* 注册 App: 大盘 + 个股 */
    appman_register(app_market_ops());
    for (int i = 0; i < s_code_count; i++) {
        app_stock_configure(i, s_codes[i]);
        appman_register(app_stock_ops(i));
    }

    appman_start();
    appman_run_bg_task();

    xTaskCreate(stock_refresh_task, "stock_refresh", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "stock_watch started, %d pages", 1 + s_code_count);
}
