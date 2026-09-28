/*
 * hw.c -- battery voltage (ADC1/GPIO6) + WS2812 RGB status LED (RMT).
 * Same board as weather_clock_lvgl.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/rmt_tx.h"
#include "esp_rom_sys.h"
#include "hw.h"
#include "app_config.h"

static const char *TAG = "hw";

static adc_oneshot_unit_handle_t s_adc = NULL;
static bool s_adc_ok = false;

void hw_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t init = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&init, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed"); return;
    }
    adc_oneshot_chan_cfg_t chan = {
        .atten    = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, PIN_BAT_ADC, &chan) != ESP_OK) {
        ESP_LOGE(TAG, "adc channel config failed"); return;
    }
    s_adc_ok = true;
}

float hw_battery_read_voltage(void)
{
    if (!s_adc_ok) return 0.0f;
    /* ADC1 12-bit, 12dB attenuation -> full scale 3.1V. VBAT is divided by
     * two (R8=100K + R10=100K) before reaching GPIO6, so multiply by 2.
     * Average 16 samples: the battery node sags/rings with WiFi TX bursts
     * and ADC noise is a few LSBs, which alone jitters the percent display
     * by 1-2 points. Averaging tames it. */
    const int N = 16;
    int32_t sum = 0;
    for (int i = 0; i < N; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, PIN_BAT_ADC, &raw) == ESP_OK) sum += raw;
        esp_rom_delay_us(50);
    }
    float avg = (float)sum / (float)N;
    float pin_v = avg * 3.1f / 4095.0f;   /* volts at GPIO6 */
    return pin_v * 2.0f;                   /* VBAT = divider x2 */
}

int hw_battery_level_percent(void)
{
    /* EMA over calls so the on-screen number doesn't flicker on every sample.
     * USB charging also lifts VBAT above the true resting voltage, so while
     * plugged the percent reads optimistically high -- unavoidable without a
     * fuel-gauge IC. */
    static float ema = -1.0f;
    float v = hw_battery_read_voltage();
    if (v <= 0.0f) return 0;
    if (ema < 0.0f) ema = v;               /* seed on first valid reading */
    ema = ema * 0.7f + v * 0.3f;
    float vv = ema;
    if (vv >= BAT_V_FULL)  return 100;
    if (vv <= BAT_V_EMPTY) return 0;
    float p = (vv - BAT_V_EMPTY) / (BAT_V_FULL - BAT_V_EMPTY) * 100.0f;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return (int)(p + 0.5f);
}

/* ---------------- WS2812 (GRB) via RMT ---------------- */
static rmt_channel_handle_t s_rmt = NULL;
static rmt_encoder_handle_t s_enc = NULL;

void hw_led_init(void)
{
    rmt_tx_channel_config_t cfg = {
        .gpio_num          = PIN_WS2812,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,
        .mem_block_symbols = 128,
    };
    if (rmt_new_tx_channel(&cfg, &s_rmt) != ESP_OK) {
        ESP_LOGW(TAG, "rmt channel create failed"); return;
    }
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 35, .level1 = 0, .duration1 = 80 },
        .bit1 = { .level0 = 1, .duration0 = 70, .level1 = 0, .duration1 = 60 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_enc) != ESP_OK) {
        ESP_LOGW(TAG, "bytes encoder create failed"); return;
    }
    rmt_enable(s_rmt);
    hw_led_off();
    ESP_LOGI(TAG, "WS2812 ready on GPIO%d", PIN_WS2812);
}

void hw_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt || !s_enc) return;
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_rmt, s_enc, grb, sizeof(grb), &tx);
}

void hw_led_off(void) { hw_led_set(0, 0, 0); }
