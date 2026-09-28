/*
 * apps.h -- 行情应用的注册入口 + 后台数据桥接。
 */
#pragma once

#include "appman/appman.h"
#include "stock/stock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 大盘页(第 0 页) */
const app_ops_t *app_market_ops(void);

/* 个股页: 支持多实例。slot ∈ [0, STOCK_MAX)。 */
void             app_stock_configure(int slot, const char *code);
const app_ops_t *app_stock_ops(int slot);

/* ---- 后台网络任务调用的桥接函数(内部自取 LVGL 锁) ---- */
void app_market_set_indices(const index_quote_t *idx, int count);
void app_market_set_updated(bool ok);

void app_stock_set_quote(int slot, const stock_quote_t *q);
void app_stock_set_kline(int slot, const kline_t *k);
void app_stock_set_kline_failed(int slot, const char *reason);
void app_stock_set_updated(int slot, bool ok);

#ifdef __cplusplus
}
#endif
