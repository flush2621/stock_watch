/*
 * appman.c -- HoloCubic-style App manager (LVGL 9, 带 ctx 版本).
 *
 * Layout: each app owns an lv_obj_t sized 240x240 positioned at (index*240, 0)
 * inside a horizontal container that is scrolled with lv_obj_scroll_to_x().
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "appman.h"
#include "hw/imu.h"
#include "app_config.h"

static const char *TAG = "appman";

#define MAX_APPS 8

typedef struct {
    const app_ops_t *ops;
    lv_obj_t        *root;
} app_slot_t;

static app_slot_t s_apps[MAX_APPS];
static int        s_count  = 0;
static int        s_active = 0;
static lv_obj_t  *s_container = NULL;
static volatile int s_req_index = -1;

static lv_obj_t  *s_indicator = NULL;

/* ------------------------------------------------------------------ */
static void indicator_rebuild(void)
{
    if (s_indicator) lv_obj_delete(s_indicator);
    s_indicator = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_indicator);
    lv_obj_set_size(s_indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_indicator, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(s_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_indicator, 4, 0);
    lv_obj_set_style_bg_opa(s_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(s_indicator, false);

    for (int i = 0; i < s_count; i++) {
        lv_obj_t *dot = lv_obj_create(s_indicator);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot,
            lv_color_hex(i == s_active ? 0xFFFFFF : 0x606880), 0);
        lv_obj_set_scrollable(dot, false);
    }
}

/* ------------------------------------------------------------------ */
void appman_register(const app_ops_t *ops)
{
    if (!ops || s_count >= MAX_APPS) return;
    s_apps[s_count].ops  = ops;
    s_apps[s_count].root = NULL;
    s_count++;
}

int appman_count(void)        { return s_count; }
int appman_active_index(void) { return s_active; }

/* ------------------------------------------------------------------ */
void appman_start(void)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock timeout"); return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    s_container = lv_obj_create(scr);
    lv_obj_remove_style_all(s_container);
    lv_obj_set_size(s_container, LCD_H_RES * s_count, LCD_V_RES);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(s_container, false);

    for (int i = 0; i < s_count; i++) {
        lv_obj_t *root = lv_obj_create(s_container);
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, LCD_H_RES, LCD_V_RES);
        lv_obj_set_pos(root, i * LCD_H_RES, 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(root, lv_color_hex(0x0B1220), 0);
        lv_obj_set_scrollable(root, false);
        s_apps[i].root = root;
        const app_ops_t *ops = s_apps[i].ops;
        if (ops->create) ops->create(root, ops->ctx);
    }

    s_active = 0;
    {
        const app_ops_t *ops = s_apps[0].ops;
        if (ops->resume) ops->resume(ops->ctx);
    }
    indicator_rebuild();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "started with %d apps", s_count);
}

/* ------------------------------------------------------------------ */
static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void do_switch(int target)
{
    if (target < 0)        target = 0;
    if (target >= s_count) target = s_count - 1;
    if (target == s_active) return;

    if (!lvgl_port_lock(200)) return;

    int32_t from = lv_obj_get_x(s_container);
    int32_t to   = -target * LCD_H_RES;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_container);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, APP_TRANSITION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_start(&a);

    {
        const app_ops_t *ops = s_apps[s_active].ops;
        if (ops->pause) ops->pause(ops->ctx);
    }
    s_active = target;
    {
        const app_ops_t *ops = s_apps[s_active].ops;
        if (ops->resume) ops->resume(ops->ctx);
    }
    indicator_rebuild();

    lvgl_port_unlock();
}

void appman_next(void) { do_switch(s_active + 1); }
void appman_prev(void) { do_switch(s_active - 1); }

void appman_request_switch(const char *name)
{
    if (!name) return;
    for (int i = 0; i < s_count; i++) {
        const char *n = s_apps[i].ops->name ? s_apps[i].ops->name() : NULL;
        if (n && strcmp(n, name) == 0) { s_req_index = i; return; }
    }
}

void appman_switch_to(const char *name)
{
    if (!name) return;
    for (int i = 0; i < s_count; i++) {
        const char *n = s_apps[i].ops->name ? s_apps[i].ops->name() : NULL;
        if (n && strcmp(n, name) == 0) { do_switch(i); return; }
    }
}

/* ------------------------------------------------------------------ */
static void bg_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(40);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        if (s_req_index >= 0) {
            int t = s_req_index;
            s_req_index = -1;
            do_switch(t);
        }

        imu_tick();
        imu_event_t ev = imu_poll_event();
        switch (ev) {
        case IMU_EV_LEFT:  appman_prev(); break;
        case IMU_EV_RIGHT: appman_next(); break;
        case IMU_EV_TAP:   appman_next(); break;
        default: break;
        }

        if (s_count > 0) {
            const app_ops_t *ops = s_apps[s_active].ops;
            if (ops->tick) {
                if (lvgl_port_lock(50)) {
                    ops->tick(ops->ctx);
                    lvgl_port_unlock();
                }
            }
        }

        vTaskDelayUntil(&last, period);
    }
}

void appman_run_bg_task(void)
{
    xTaskCreate(bg_task, "appman", 6144, NULL, 5, NULL);
}