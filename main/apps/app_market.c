/*
 * app_market.c -- A股大盘页(第 0 页)。
 * 显示 4 个指数(上证/深成/创业板/沪深300)的名称、点位、涨跌幅。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "app_config.h"
#include "apps.h"
#include "app_font_cn_16.h"

static const char *TAG = "app_market";

#define COL_TEXT       0xFFFFFF
#define COL_DIM        0x8892A6
#define COL_UP         0xE53935   /* 涨-红 */
#define COL_DOWN       0x00C853   /* 跌-绿 */
#define COL_CARD_BG    0x16203A
#define COL_CARD_BD    0x24365E

#define FONT_CN   (&app_font_cn_16)
#define FONT_BIG  (&lv_font_montserrat_28)

static lv_obj_t *s_root  = NULL;
static lv_obj_t *s_cards[INDEX_COUNT]    = {0};
static lv_obj_t *s_name[INDEX_COUNT]     = {0};
static lv_obj_t *s_price[INDEX_COUNT]    = {0};
static lv_obj_t *s_chg[INDEX_COUNT]      = {0};
static lv_obj_t *s_time = NULL;

static const char *DEFAULT_NAMES[INDEX_COUNT] = {
    "上证指数", "深证成指", "创业板指", "沪深300",
};

static lv_obj_t *create_cb(lv_obj_t *parent, void *ctx)
{
    (void)ctx;
    s_root = parent;
    ESP_LOGI(TAG, "market page created");

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, FONT_CN, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(title, "A股大盘");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    for (int i = 0; i < INDEX_COUNT; i++) {
        s_cards[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(s_cards[i]);
        lv_obj_set_size(s_cards[i], 224, 44);
        lv_obj_align(s_cards[i], LV_ALIGN_TOP_MID, 0, 24 + i * 48);
        lv_obj_set_style_bg_opa(s_cards[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(COL_CARD_BG), 0);
        lv_obj_set_style_radius(s_cards[i], 12, 0);
        lv_obj_set_style_border_width(s_cards[i], 1, 0);
        lv_obj_set_style_border_color(s_cards[i], lv_color_hex(COL_CARD_BD), 0);
        lv_obj_set_scrollable(s_cards[i], false);

        s_name[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_name[i], FONT_CN, 0);
        lv_obj_set_style_text_color(s_name[i], lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(s_name[i], DEFAULT_NAMES[i]);
        lv_obj_align(s_name[i], LV_ALIGN_LEFT_MID, 10, -9);

        s_chg[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_chg[i], FONT_CN, 0);
        lv_obj_set_style_text_color(s_chg[i], lv_color_hex(COL_DIM), 0);
        lv_label_set_text(s_chg[i], "--");
        lv_obj_align(s_chg[i], LV_ALIGN_LEFT_MID, 10, 10);

        s_price[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_price[i], FONT_BIG, 0);
        lv_obj_set_style_text_color(s_price[i], lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(s_price[i], "--");
        lv_obj_align(s_price[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }

    s_time = lv_label_create(parent);
    lv_obj_set_style_text_font(s_time, FONT_CN, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(s_time, "连接中...");
    lv_obj_align(s_time, LV_ALIGN_BOTTOM_MID, 0, -4);

    return parent;
}

static const char *name_cb(void) { return "market"; }

static const app_ops_t OPS = {
    .name = name_cb,
    .create = create_cb,
    .ctx = NULL,
};

const app_ops_t *app_market_ops(void) { return &OPS; }

/* ---------------- 桥接 ---------------- */
void app_market_set_indices(const index_quote_t *idx, int count)
{
    if (!lvgl_port_lock(100)) return;

    int n = count < INDEX_COUNT ? count : INDEX_COUNT;
    for (int i = 0; i < n; i++) {
        if (idx[i].name[0] && s_name[i]) lv_label_set_text(s_name[i], idx[i].name);

        bool up = idx[i].change_pct >= 0;
        lv_color_t col = lv_color_hex(up ? COL_UP : COL_DOWN);

        if (s_price[i]) {
            char b[32];
            snprintf(b, sizeof(b), "%.2f", idx[i].price);
            lv_label_set_text(s_price[i], b);
            lv_obj_set_style_text_color(s_price[i], col, 0);
        }
        if (s_chg[i]) {
            char b[32];
            snprintf(b, sizeof(b), "涨跌幅 %+.2f%%", idx[i].change_pct);
            lv_label_set_text(s_chg[i], b);
            lv_obj_set_style_text_color(s_chg[i], col, 0);
        }
    }
    lvgl_port_unlock();
}

void app_market_set_updated(bool ok)
{
    if (!lvgl_port_lock(100)) return;
    if (s_time) {
        if (!ok) {
            lv_label_set_text(s_time, "行情获取失败");
            lv_obj_set_style_text_color(s_time, lv_color_hex(COL_UP), 0);
        } else {
            time_t now; struct tm ti;
            time(&now); localtime_r(&now, &ti);
            char b[32];
            snprintf(b, sizeof(b), "更新 %02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
            lv_label_set_text(s_time, b);
            lv_obj_set_style_text_color(s_time, lv_color_hex(COL_DIM), 0);
        }
    }
    lvgl_port_unlock();
}
