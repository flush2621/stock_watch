/*
 * stock.h -- A股行情数据层。
 * 从东方财富/腾讯免费接口拉取大盘指数与个股行情/K线, 用 cJSON 解析。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STOCK_CODE_LEN  16
#define STOCK_NAME_LEN  32
#define KLINE_MAX       40     /* 保留最近 40 根日K */

/* 个股实时行情 */
typedef struct {
    char    name[STOCK_NAME_LEN];
    char    code[STOCK_CODE_LEN];
    double  price;         /* 现价 */
    double  prev_close;    /* 昨收 */
    double  open;          /* 今开 */
    double  high;          /* 最高 */
    double  low;           /* 最低 */
    double  change;        /* 涨跌额 */
    double  change_pct;    /* 涨跌幅 % */
    double  volume;        /* 成交量(手) */
    double  amount;        /* 成交额(元) */
    double  turnover;      /* 换手率 % */
    double  pe;            /* 市盈率 */
    double  pb;            /* 市净率 */
    bool    valid;
} stock_quote_t;

/* 大盘指数 */
typedef struct {
    char    name[STOCK_NAME_LEN];
    double  price;
    double  change;
    double  change_pct;
    bool    valid;
} index_quote_t;

/* K线单根 */
typedef struct {
    char    date[12];      /* "2026-09-02" */
    double  open;
    double  close;
    double  high;
    double  low;
    double  volume;
} kbar_t;

typedef struct {
    int     count;
    kbar_t  bars[KLINE_MAX];
} kline_t;

/** 从 SD 卡读取股票代码(code.txt 一行一个, 支持 sh/sz/bj 前缀与 .SH/.SZ 后缀,
 *  跳过空行与 '#' 注释)。返回读取数量(≤ STOCK_MAX)。 */
int  stock_load_codes(char codes[][STOCK_CODE_LEN], int max);

/** 拉取大盘指数, 返回成功数量。 */
int  stock_fetch_indices(index_quote_t out[], int max);

/** 拉取个股实时行情。 */
bool stock_fetch_quote(const char *code, stock_quote_t *out);

/** 拉取个股日K线(前复权)。失败时可用 stock_kline_error() 取原因。 */
bool stock_fetch_kline(const char *code, kline_t *out);

/** 最近一次 stock_fetch_kline 失败的原因(多行文本, 每个数据源一行), 成功时为空串。 */
const char *stock_kline_error(void);

#ifdef __cplusplus
}
#endif
