/*
 * app_stock.c -- 个股页(第 1..N 页)。
 * 显示股票名称/代码、现价/涨跌、关键指标, 以及最近 40 根日K线的蜡烛图。
 * 通过 app_ops_t.ctx 支持多实例(每只股票一个 slot)。
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

static const char *TAG = "app_stock";

#define COL_TEXT       0xFFFFFF
#define COL_DIM        0x8892A6
#define COL_UP         0xE53935   /* 涨-红 */
#define COL_DOWN       0x00C853   /* 跌-绿 */
#define COL_CARD_BG    0x16203A
#define COL_CARD_BD    0x24365E
#define COL_GRID       0x24365E

#define FONT_CN   (&app_font_cn_16)
#define FONT_BIG  (&lv_font_montserrat_28)
#define FONT_MID  (&lv_font_montserrat_20)

typedef struct {
    int         slot;
    char        code[STOCK_CODE_LEN];
    lv_obj_t   *root;
    lv_obj_t   *lbl_name;      /* 名称+代码 */
    lv_obj_t   *lbl_price;     /* 现价 */
    lv_obj_t   *lbl_chg;       /* 涨跌额/涨跌幅 */
    lv_obj_t   *lbl_meta1;     /* 今开 最高 最低 */
    lv_obj_t   *lbl_meta2;     /* 昨收 换手 量额 */
    lv_obj_t   *lbl_time;
    lv_obj_t   *chart;         /* K线容器(自绘蜡烛) */
    stock_quote_t quote;
    kline_t    kline;
    bool       has_quote;
    bool       has_kline;
    bool       kline_failed;   /* K线拉取失败(用于在图上显示状态) */
    char       kline_err[96];  /* K线失败原因(每个数据源一行) */
} stock_ctx_t;

static stock_ctx_t s_ctx[STOCK_MAX];
static bool s_ctx_ready = false;

/* ---------------- K线自绘 ---------------- */
/*
 * LVGL 9 绘制注意(两个坑, 缺一个蜡烛图就是空白):
 *  1) 绘制事件回调里用的坐标是"图层绝对坐标"(屏幕坐标), 不是控件内的相对坐标;
 *     要先用 lv_obj_get_coords() 取控件左上角再加偏移, 否则画到控件外面被裁剪掉。
 *  2) 用户回调先于控件自身的背景绘制执行, 所以必须挂在 LV_EVENT_DRAW_POST_END 上;
 *     挂在 LV_EVENT_DRAW_MAIN 上画的蜡烛会被控件自己的背景盖掉。
 */
/* 在图表框内居中写状态: 标题一行(中文字体), 详细原因若干行(小号字体)。
 * detail 里可以带 '\n'。坐标都是图层绝对坐标。 */
static void chart_status(lv_layer_t *layer, const lv_area_t *oc,
                         const char *title, const char *detail)
{
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(COL_DIM);
    ld.align = LV_TEXT_ALIGN_CENTER;

    int32_t th = lv_font_get_line_height(FONT_CN);
    int32_t dh = 0;
    int     dlines = 0;
    if (detail && detail[0]) {
        dh = lv_font_get_line_height(&lv_font_montserrat_14);
        dlines = 1;
        for (const char *p = detail; *p; p++) {
            if (*p == '\n') dlines++;
        }
        if (dlines > 3) dlines = 3;   /* 框里最多放三行原因 */
    }

    int32_t total = th + (dlines ? 4 + dlines * dh : 0);
    lv_area_t la = *oc;
    la.y1 = (oc->y1 + oc->y2) / 2 - total / 2;
    la.y2 = la.y1 + th;

    ld.font = FONT_CN;
    ld.text = title;
    lv_draw_label(layer, &ld, &la);

    if (dlines) {
        la.y1 = la.y2 + 4;
        la.y2 = la.y1 + dlines * dh;
        ld.font = &lv_font_montserrat_14;
        ld.text = detail;
        lv_draw_label(layer, &ld, &la);
    }
}

static void chart_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    stock_ctx_t *ctx = (stock_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    const int32_t pad = 4;

    /* 该控件在屏幕上的绝对位置 = 本次绘制的坐标原点 */
    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);
    const int32_t ox = oc.x1;
    const int32_t oy = oc.y1;

    /* 没有K线数据: 在框里写明状态, 这样不接串口也能看出是"数据没来"还是"没画出来" */
    if (!ctx->has_kline || ctx->kline.count < 2) {
        chart_status(layer, &oc, ctx->kline_failed ? "K线获取失败" : "K线加载中...",
                     ctx->kline_failed ? ctx->kline_err : NULL);
        return;
    }

    /* 价格范围(留 5% 边) */
    double vmin = ctx->kline.bars[0].low;
    double vmax = ctx->kline.bars[0].high;
    for (int i = 1; i < ctx->kline.count; i++) {
        if (ctx->kline.bars[i].low  < vmin) vmin = ctx->kline.bars[i].low;
        if (ctx->kline.bars[i].high > vmax) vmax = ctx->kline.bars[i].high;
    }
    double span = vmax - vmin;
    if (span <= 0) span = 1.0;
    vmin -= span * 0.05;
    vmax += span * 0.05;
    span = vmax - vmin;

    /* 价格全为 0 说明解析出来的字段有问题(接口偶尔会返回数字而不是字符串) */
    if (vmax <= 0.0 || vmin <= 0.0) {
        chart_status(layer, &oc, "K线数据异常", NULL);
        return;
    }

    int n = ctx->kline.count;
    double slot = (double)(w - 2 * pad) / n;
    double bw = slot * 0.6;   /* 实体宽度 */
    if (bw < 1.0) bw = 1.0;
    if (bw > 6.0) bw = 6.0;

    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.width = 1; ln.opa = LV_OPA_COVER;
    ln.round_start = 1; ln.round_end = 1;

    /* 上下参考线: 只要有数据就一定画, 便于确认自绘是否生效 */
    ln.color = lv_color_hex(COL_GRID);
    ln.p1 = (lv_point_precise_t){ oc.x1 + pad, oc.y1 + pad };
    ln.p2 = (lv_point_precise_t){ oc.x2 - pad, oc.y1 + pad };
    lv_draw_line(layer, &ln);
    ln.p1 = (lv_point_precise_t){ oc.x1 + pad, oc.y2 - pad };
    ln.p2 = (lv_point_precise_t){ oc.x2 - pad, oc.y2 - pad };
    lv_draw_line(layer, &ln);

    lv_draw_rect_dsc_t rc;
    lv_draw_rect_dsc_init(&rc);
    rc.radius = 0; rc.bg_opa = LV_OPA_COVER;

    lv_area_t a;

    for (int i = 0; i < n; i++) {
        const kbar_t *b = &ctx->kline.bars[i];
        bool up = (b->close >= b->open);
        lv_color_t col = lv_color_hex(up ? COL_UP : COL_DOWN);

        int cx = ox + pad + (int)(slot * i + slot / 2);
        int y_high = oy + pad + (int)((vmax - b->high) / span * (h - 2 * pad));
        int y_low  = oy + pad + (int)((vmax - b->low)  / span * (h - 2 * pad));
        int y_open = oy + pad + (int)((vmax - b->open) / span * (h - 2 * pad));
        int y_cls  = oy + pad + (int)((vmax - b->close)/ span * (h - 2 * pad));

        /* 影线(最高-最低) */
        ln.color = col;
        ln.p1 = (lv_point_precise_t){ cx, y_high };
        ln.p2 = (lv_point_precise_t){ cx, y_low };
        lv_draw_line(layer, &ln);

        /* 实体(开-收) */
        int y_top = y_open < y_cls ? y_open : y_cls;
        int y_bot = y_open > y_cls ? y_open : y_cls;
        if (y_bot - y_top < 1) y_bot = y_top + 1;
        rc.bg_color = col;
        a = (lv_area_t){ cx - (int)(bw / 2), y_top, cx + (int)(bw / 2), y_bot };
        lv_draw_rect(layer, &rc, &a);
    }
}

/* ---------------- 构建 ---------------- */
static lv_obj_t *create_cb(lv_obj_t *parent, void *ctxp)
{
    stock_ctx_t *ctx = (stock_ctx_t *)ctxp;
    ctx->root = parent;

    ctx->lbl_name = lv_label_create(parent);
    lv_obj_set_style_text_font(ctx->lbl_name, FONT_CN, 0);
    lv_obj_set_style_text_color(ctx->lbl_name, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text_fmt(ctx->lbl_name, "%s", ctx->code);
    lv_obj_align(ctx->lbl_name, LV_ALIGN_TOP_LEFT, 10, 4);

    ctx->lbl_price = lv_label_create(parent);
    lv_obj_set_style_text_font(ctx->lbl_price, FONT_BIG, 0);
    lv_obj_set_style_text_color(ctx->lbl_price, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ctx->lbl_price, "--");
    lv_obj_align(ctx->lbl_price, LV_ALIGN_TOP_LEFT, 10, 28);

    ctx->lbl_chg = lv_label_create(parent);
    lv_obj_set_style_text_font(ctx->lbl_chg, FONT_MID, 0);
    lv_obj_set_style_text_color(ctx->lbl_chg, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(ctx->lbl_chg, "");
    lv_obj_align(ctx->lbl_chg, LV_ALIGN_TOP_RIGHT, -10, 40);

    ctx->lbl_meta1 = lv_label_create(parent);
    lv_obj_set_style_text_font(ctx->lbl_meta1, FONT_CN, 0);
    lv_obj_set_style_text_color(ctx->lbl_meta1, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ctx->lbl_meta1, "");
    lv_obj_align(ctx->lbl_meta1, LV_ALIGN_TOP_LEFT, 10, 66);

    ctx->lbl_meta2 = lv_label_create(parent);
    lv_obj_set_style_text_font(ctx->lbl_meta2, FONT_CN, 0);
    lv_obj_set_style_text_color(ctx->lbl_meta2, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ctx->lbl_meta2, "");
    lv_obj_align(ctx->lbl_meta2, LV_ALIGN_TOP_LEFT, 10, 88);

    /* K线图容器 */
    ctx->chart = lv_obj_create(parent);
    lv_obj_remove_style_all(ctx->chart);
    lv_obj_set_size(ctx->chart, 224, 100);
    lv_obj_align(ctx->chart, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(ctx->chart, lv_color_hex(COL_CARD_BG), 0);
    lv_obj_set_style_bg_opa(ctx->chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ctx->chart, 10, 0);
    lv_obj_set_scrollable(ctx->chart, false);
    lv_obj_add_event_cb(ctx->chart, chart_draw_cb, LV_EVENT_DRAW_POST_END, ctx);

    ctx->lbl_time = lv_label_create(parent);
    lv_obj_set_style_text_font(ctx->lbl_time, FONT_CN, 0);
    lv_obj_set_style_text_color(ctx->lbl_time, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(ctx->lbl_time, "加载中...");
    lv_obj_align(ctx->lbl_time, LV_ALIGN_BOTTOM_MID, 0, -4);

    return parent;
}

static const char *name_cb(void) { return "stock"; }

/* ---------------- 公共 ---------------- */
static void stock_ctx_ensure(void)
{
    if (s_ctx_ready) return;
    for (int i = 0; i < STOCK_MAX; i++) {
        s_ctx[i].slot = i;
        s_ctx[i].code[0] = '\0';
        s_ctx[i].has_quote = false;
        s_ctx[i].has_kline = false;
        s_ctx[i].kline_failed = false;
        s_ctx[i].kline_err[0] = '\0';
    }
    s_ctx_ready = true;
}

void app_stock_configure(int slot, const char *code)
{
    stock_ctx_ensure();
    if (slot < 0 || slot >= STOCK_MAX) return;
    snprintf(s_ctx[slot].code, sizeof(s_ctx[slot].code), "%s", code ? code : "");
    ESP_LOGI(TAG, "slot %d -> %s", slot, s_ctx[slot].code);
}

const app_ops_t *app_stock_ops(int slot)
{
    stock_ctx_ensure();
    /* 每个 slot 用自己的 ops(带自己的 ctx), 但复用同一组函数指针 */
    static app_ops_t ops[STOCK_MAX];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < STOCK_MAX; i++) {
            ops[i].name   = name_cb;
            ops[i].create = create_cb;
            ops[i].ctx    = &s_ctx[i];
        }
        init = true;
    }
    if (slot < 0 || slot >= STOCK_MAX) slot = 0;
    return &ops[slot];
}

/* ---------------- 桥接 ---------------- */
void app_stock_set_quote(int slot, const stock_quote_t *q)
{
    if (!q) return;
    stock_ctx_ensure();
    if (slot < 0 || slot >= STOCK_MAX) return;
    stock_ctx_t *ctx = &s_ctx[slot];
    ctx->quote = *q;
    ctx->has_quote = true;

    if (!lvgl_port_lock(100)) return;
    if (ctx->lbl_name) {
        lv_label_set_text_fmt(ctx->lbl_name, "%s %s", q->name[0] ? q->name : q->code, q->code);
    }
    bool up = q->change_pct >= 0;
    lv_color_t col = lv_color_hex(up ? COL_UP : COL_DOWN);
    if (ctx->lbl_price) {
        char b[24];
        snprintf(b, sizeof(b), "%.2f", q->price);
        lv_label_set_text(ctx->lbl_price, b);
        lv_obj_set_style_text_color(ctx->lbl_price, col, 0);
    }
    if (ctx->lbl_chg) {
        char b[48];
        snprintf(b, sizeof(b), "%+.2f  %+.2f%%", q->change, q->change_pct);
        lv_label_set_text(ctx->lbl_chg, b);
        lv_obj_set_style_text_color(ctx->lbl_chg, col, 0);
    }
    if (ctx->lbl_meta1) {
        char b[96];
        snprintf(b, sizeof(b), "今开%.2f  最高%.2f", q->open, q->high);
        lv_label_set_text(ctx->lbl_meta1, b);
    }
    if (ctx->lbl_meta2) {
        char b[96];
        snprintf(b, sizeof(b), "最低%.2f  昨收%.2f  换手%.2f%%", q->low, q->prev_close, q->turnover);
        lv_label_set_text(ctx->lbl_meta2, b);
    }
    lvgl_port_unlock();
}

void app_stock_set_kline(int slot, const kline_t *k)
{
    if (!k) return;
    stock_ctx_ensure();
    if (slot < 0 || slot >= STOCK_MAX) return;
    stock_ctx_t *ctx = &s_ctx[slot];
    ctx->kline = *k;
    ctx->has_kline = true;
    ctx->kline_failed = false;
    ctx->kline_err[0] = '\0';
    ESP_LOGI(TAG, "slot %d K线 %d 根, 重绘图表", slot, k->count);

    if (!lvgl_port_lock(100)) return;
    if (ctx->chart) lv_obj_invalidate(ctx->chart);
    lvgl_port_unlock();
}

/* 拉取失败: 记下原因并重绘, 图上会显示"K线获取失败" + 每个数据源的原因 */
void app_stock_set_kline_failed(int slot, const char *reason)
{
    stock_ctx_ensure();
    if (slot < 0 || slot >= STOCK_MAX) return;
    stock_ctx_t *ctx = &s_ctx[slot];

    char err[sizeof(ctx->kline_err)];
    snprintf(err, sizeof(err), "%s", reason ? reason : "");
    bool changed = (ctx->kline_err[0] != '\0' && strcmp(ctx->kline_err, err) != 0);
    snprintf(ctx->kline_err, sizeof(ctx->kline_err), "%s", err);
    if (changed) {
        ESP_LOGW(TAG, "slot %d K线获取失败: %s", slot, ctx->kline_err);
    }
    if (ctx->kline_failed && !changed) return;   /* 状态没变就不用重绘 */
    ctx->kline_failed = true;

    if (!lvgl_port_lock(100)) return;
    if (ctx->chart) lv_obj_invalidate(ctx->chart);
    lvgl_port_unlock();
}

void app_stock_set_updated(int slot, bool ok)
{
    stock_ctx_ensure();
    if (slot < 0 || slot >= STOCK_MAX) return;
    stock_ctx_t *ctx = &s_ctx[slot];
    if (!lvgl_port_lock(100)) return;
    if (ctx->lbl_time) {
        if (!ok) {
            lv_label_set_text(ctx->lbl_time, "行情获取失败");
            lv_obj_set_style_text_color(ctx->lbl_time, lv_color_hex(COL_UP), 0);
        } else {
            time_t now; struct tm ti;
            time(&now); localtime_r(&now, &ti);
            char b[40];
            snprintf(b, sizeof(b), "更新 %02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
            lv_label_set_text(ctx->lbl_time, b);
            lv_obj_set_style_text_color(ctx->lbl_time, lv_color_hex(COL_DIM), 0);
        }
    }
    lvgl_port_unlock();
}
