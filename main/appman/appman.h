/*
 * appman.h -- HoloCubic-style App manager.
 *
 * Each "app" owns a top-level lv_obj_t* (a child of lv_screen_active()).
 * The manager keeps a ring of apps, slides them horizontally on switch,
 * and calls life-cycle hooks (create/resume/pause/destroy/tick).
 *
 * 相比 small_tv 的版本, 这里给每个生命周期钩子增加了 void *ctx 参数,
 * 以便"个股页"用同一套函数指针驱动多个实例(每只股票一个 slot)。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_desc_s app_desc_t;

typedef struct {
    /* Called once, before create(), to give the app its name for lookup. */
    const char *(*name)(void);
    /* Build LVGL widgets on the given parent. Return the root obj. */
    lv_obj_t *(*create)(lv_obj_t *parent, void *ctx);
    /* Optional: called when the app becomes active. */
    void      (*resume)(void *ctx);
    /* Optional: called when the app is scrolled off-screen. */
    void      (*pause)(void *ctx);
    /* Optional: called every ~40 ms while active. */
    void      (*tick)(void *ctx);
    /* Optional: called before destroying the root object. */
    void      (*destroy)(void *ctx);
    /* Per-instance context passed to every callback. */
    void *ctx;
} app_ops_t;

/** Register an app. Copies the ops pointer (must remain valid). */
void appman_register(const app_ops_t *ops);

/** Build all registered apps and show the first one. */
void appman_start(void);

/** Switch APIs (thread-safe: they take the LVGL lock internally). */
void appman_next(void);
void appman_prev(void);
void appman_switch_to(const char *name);

/** Lock-free switch request: posts a target app name, consumed by the
 *  background task. Safe to call from inside LVGL timer callbacks. */
void appman_request_switch(const char *name);

/** Current active index (0-based). */
int  appman_active_index(void);
int  appman_count(void);

/** Background tick task: polls imu_poll_event() and calls ops->tick(). */
void appman_run_bg_task(void);

#ifdef __cplusplus
}
#endif