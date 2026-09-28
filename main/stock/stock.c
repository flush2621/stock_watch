/*
 * stock.c -- A股行情数据层。
 *
 * 数据源(均为免费接口, UTF-8 JSON, 无需鉴权):
 *   大盘指数批量: http://push2.eastmoney.com/api/qt/ulist.np/get?secids=...&fields=f2,f3,f4,f12,f14
 *       f2=最新价(×100) f3=涨跌幅(×100) f4=涨跌额(×100) f12=代码 f14=名称
 *   个股实时:     http://push2.eastmoney.com/api/qt/stock/get?secid=...&fields=...
 *       f43=现价 f44=最高 f45=最低 f46=今开 f47=成交量(手) f48=成交额(元)
 *       f57=代码 f58=名称 f60=昨收 f162=市盈率 f167=市净率 f168=换手率
 *       f169=涨跌额 f170=涨跌幅   (价格/涨跌幅/换手/市盈率均为 ×100 整数)
 *   个股日K线(依次尝试, 任一成功即可):
 *     a) http://push2his.eastmoney.com/api/qt/stock/kline/get?secid=<secid>&klt=101&fqt=1&end=20500101&lmt=40
 *        data.klines = [ "日期,开,收,高,低,量,额,振幅", ... ]  (升序, 实测 40 根)
 *     b) https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=<sym>,day,,,80,qfq
 *        data.<sym>.qfqday = [ [date, open, close, high, low, volume], ... ]
 *     注意: 腾讯的 http 入口返回 302 跳 https, 所以这里直接用 https(需要证书 bundle)。
 *
 * secid 映射: 6xxxxx -> 沪市 "1.6xxxxx" / tencent "sh6xxxxx"
 *             0/3xxxxx -> 深市 "0.xxxxxx" / tencent "szxxxxx"
 *             8/4/9xxxxx -> 北交所 "0.xxxxxx" / tencent "bjxxxxx"
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "cJSON.h"
#include "stock.h"
#include "net.h"
#include "app_config.h"

static const char *TAG = "stock";

#define EM_QUOTE_URL  "http://push2.eastmoney.com/api/qt/stock/get?secid=%s" \
                      "&fields=f43,f44,f45,f46,f47,f48,f57,f58,f60,f162,f167,f168,f169,f170"
#define EM_INDEX_URL  "http://push2.eastmoney.com/api/qt/ulist.np/get?secids=" INDEX_SECIDS \
                      "&fields=f2,f3,f4,f12,f14"

/* ---------------- 代码归一化 ---------------- */
static void code_to_secid(const char *code, char *out, size_t n)
{
    /* 沪市(6/9/5开头) -> 1.xxx, 其余(深0/3/2/1, 北8/4) -> 0.xxx */
    if (code[0] == '6' || code[0] == '9' || code[0] == '5') {
        snprintf(out, n, "1.%s", code);
    } else {
        snprintf(out, n, "0.%s", code);
    }
}

static void code_to_symbol(const char *code, char *out, size_t n)
{
    if (code[0] == '6' || code[0] == '9' || code[0] == '5') {
        snprintf(out, n, "sh%s", code);
    } else if (code[0] == '8' || code[0] == '4') {
        snprintf(out, n, "bj%s", code);
    } else {
        snprintf(out, n, "sz%s", code);
    }
}

/* ---------------- 从 SD 卡读代码 ---------------- */
int stock_load_codes(char codes[][STOCK_CODE_LEN], int max)
{
    int count = 0;
    FILE *f = fopen(STOCK_CODE_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "无法打开 %s (SD卡未插/目录不存在)", STOCK_CODE_PATH);
        return 0;
    }

    char line[128];
    while (count < max && fgets(line, sizeof(line), f)) {
        /* trim leading whitespace */
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        size_t len = strlen(s);
        while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                           s[len-1] == '\r' || s[len-1] == '\n')) {
            s[--len] = '\0';
        }
        if (len == 0 || s[0] == '#') continue;

        /* 去 sh/sz/bj 前缀 */
        char *p = s;
        if (len >= 2 &&
            (p[0] == 's' || p[0] == 'S') &&
            (p[1] == 'h' || p[1] == 'H' || p[1] == 'z' || p[1] == 'Z')) {
            p += 2;
        } else if (len >= 2 &&
                   (p[0] == 'b' || p[0] == 'B') &&
                   (p[1] == 'j' || p[1] == 'J')) {
            p += 2;
        }

        /* 去 .SH/.SZ 后缀 */
        char norm[STOCK_CODE_LEN];
        size_t clen = 0;
        for (size_t i = 0; p[i] && clen < sizeof(norm) - 1; i++) {
            if (p[i] == '.') break;
            norm[clen++] = p[i];
        }
        norm[clen] = '\0';

        if (norm[0]) {
            snprintf(codes[count], STOCK_CODE_LEN, "%s", norm);
            ESP_LOGI(TAG, "股票代码[%d]: %s", count, codes[count]);
            count++;
        }
    }
    fclose(f);
    return count;
}

/* ---------------- cJSON 辅助 ---------------- */
static double jnum(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsNumber(v)) return v->valuedouble;
    return def;
}

static void jstr(const cJSON *obj, const char *key, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsString(v)) snprintf(out, n, "%s", v->valuestring);
    else out[0] = '\0';
}

/* ---------------- 大盘指数 ---------------- */
int stock_fetch_indices(index_quote_t out[], int max)
{
    char *buf = net_http_get(EM_INDEX_URL);
    if (!buf) return 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return 0;

    int n = 0;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *diff = data ? cJSON_GetObjectItemCaseSensitive(data, "diff") : NULL;
    if (diff && cJSON_IsArray(diff)) {
        cJSON *item;
        cJSON_ArrayForEach(item, diff) {
            if (n >= max) break;
            jstr(item, "f14", out[n].name, sizeof(out[n].name));
            out[n].price      = jnum(item, "f2", 0) / 100.0;
            out[n].change_pct = jnum(item, "f3", 0) / 100.0;
            out[n].change     = jnum(item, "f4", 0) / 100.0;
            out[n].valid      = true;
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

/* ---------------- 个股实时 ---------------- */
bool stock_fetch_quote(const char *code, stock_quote_t *out)
{
    char secid[16];
    code_to_secid(code, secid, sizeof(secid));
    char url[256];
    snprintf(url, sizeof(url), EM_QUOTE_URL, secid);

    char *buf = net_http_get(url);
    if (!buf) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;

    bool ok = false;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (data && cJSON_IsObject(data)) {
        jstr(data, "f58", out->name, sizeof(out->name));
        jstr(data, "f57", out->code, sizeof(out->code));
        out->price      = jnum(data, "f43", 0) / 100.0;
        out->high       = jnum(data, "f44", 0) / 100.0;
        out->low        = jnum(data, "f45", 0) / 100.0;
        out->open       = jnum(data, "f46", 0) / 100.0;
        out->volume     = jnum(data, "f47", 0);          /* 手 */
        out->amount     = jnum(data, "f48", 0);          /* 元 */
        out->prev_close = jnum(data, "f60", 0) / 100.0;
        out->pe         = jnum(data, "f162", 0) / 100.0;
        out->pb         = jnum(data, "f167", 0) / 100.0;
        out->turnover   = jnum(data, "f168", 0) / 100.0;
        out->change     = jnum(data, "f169", 0) / 100.0;
        out->change_pct = jnum(data, "f170", 0) / 100.0;
        out->valid      = (out->price > 0.0);
        ok = out->valid;
    }
    cJSON_Delete(root);
    return ok;
}

/* ---------------- 个股日K线 ---------------- */
/*
 * 依次尝试两个数据源, 任一成功即返回(免费接口, 无需鉴权)。以下为 2026-09-28 实测结果:
 *
 *  1) 东财 push2his (http, 不跳转) —— 首选, 实测 HTTP 200, 40 根, 升序, 列序与解析一致:
 *     http://push2his.eastmoney.com/api/qt/stock/kline/get?secid=1.600519&klt=101&fqt=1&end=20500101&lmt=40
 *     data.klines = [ "2026-08-03,1350.60,1358.98,1363.35,1346.00,36147,4898665275.00,1.28", ... ]
 *                    日期,开,收,高,低,量,额,振幅           (fields2=f51..f58)
 *
 *  2) 腾讯 ifzq (必须用 https) —— 备用。实测它的 http 入口返回 302 跳 https,
 *     走 http 会白跑一跳; 而设备上 https 握手又需要证书 bundle, 所以只当兜底:
 *     https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh600519,day,,,80,qfq
 *     data.<sym>.qfqday = [ [date, open, close, high, low, volume], ... ]
 *
 *  注: 东财 push2.eastmoney.com(实时行情那个域名)的 kline 接口返回的是空数组
 *      ("klines":[], "dktotal":0), 不能当K线源用, 已去掉。
 *
 * 每个失败的数据源都会把原因记到 s_kerr, 由界面直接显示, 不用接串口。
 */
#define EM_KLINE_FIELDS "&fields1=f1,f2,f3,f4,f5,f6&fields2=f51,f52,f53,f54,f55,f56,f57,f58"
#define EM_KLINE_URL  "http://push2his.eastmoney.com/api/qt/stock/kline/get?secid=%s" \
                      "&klt=101&fqt=1&end=20500101&lmt=40" EM_KLINE_FIELDS
#define TX_KLINE_URL  "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=%s,day,,,80,qfq"

#define KLINE_TMP_MAX 80
static kbar_t s_tmp[KLINE_TMP_MAX];   /* static: 避免 ~4KB 栈压力 */
static char   s_kerr[160];            /* 每个数据源一行失败原因(多行文本) */

const char *stock_kline_error(void) { return s_kerr; }

static void kerr_add(const char *src, const char *what)
{
    size_t len = strlen(s_kerr);
    if (len && len + 1 < sizeof(s_kerr)) {
        s_kerr[len++] = '\n';
        s_kerr[len] = '\0';
    }
    snprintf(s_kerr + len, sizeof(s_kerr) - len, "%s:%s", src, what);
}

static void kerr_add_status(const char *src, int status)
{
    char t[16];
    snprintf(t, sizeof(t), "HTTP%d", status);
    kerr_add(src, t);
}

/* 把 s_tmp[0..m) 里最近的 KLINE_MAX 根拷贝到 out */
static bool kline_finish(const char *code, kline_t *out, int m, const char *src)
{
    if (m <= 0) {
        kerr_add(src, "无数据");
        return false;
    }
    int start = m > KLINE_MAX ? (m - KLINE_MAX) : 0;
    int n = m - start;
    memcpy(out->bars, &s_tmp[start], (size_t)n * sizeof(kbar_t));
    out->count = n;
    ESP_LOGI(TAG, "%s K线 %d 根 (%s ~ %s) via %s", code, n,
             out->bars[0].date, out->bars[n - 1].date, src);
    return true;
}

/* "2026-09-02,10.31,10.25,10.40,10.20,123456,12345678.00,1.85" -> kbar_t */
static void em_row_to_bar(const char *s, kbar_t *b)
{
    memset(b, 0, sizeof(*b));
    double v[7] = { 0 };
    int fi = 0;
    const char *p = s;
    while (p && *p && fi < 8) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (fi == 0) {
            if (len > sizeof(b->date) - 1) len = sizeof(b->date) - 1;
            memcpy(b->date, p, len);
            b->date[len] = '\0';
        } else if (fi - 1 < 7) {
            char t[24];
            if (len > sizeof(t) - 1) len = sizeof(t) - 1;
            memcpy(t, p, len);
            t[len] = '\0';
            v[fi - 1] = atof(t);
        }
        fi++;
        p = comma ? comma + 1 : NULL;
    }
    b->open   = v[0];
    b->close  = v[1];
    b->high   = v[2];
    b->low    = v[3];
    b->volume = v[4];
}

/* 东财日K */
static bool kline_from_em(const char *code, kline_t *out, const char *url_fmt, const char *src)
{
    char secid[16];
    code_to_secid(code, secid, sizeof(secid));
    char url[320];
    snprintf(url, sizeof(url), url_fmt, secid);

    char *buf = net_http_get(url);
    if (!buf) {
        kerr_add(src, net_err_short_name(net_http_last_err()));
        return false;
    }
    if (net_http_last_status() != 200) {
        kerr_add_status(src, net_http_last_status());
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "%s 东财K线响应不是合法JSON, 前100字节: %.100s", code, buf);
        kerr_add(src, "JSON");
        return false;
    }

    int m = 0;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *kl   = data ? cJSON_GetObjectItemCaseSensitive(data, "klines") : NULL;
    if (kl && cJSON_IsArray(kl)) {
        cJSON *it;
        cJSON_ArrayForEach(it, kl) {
            if (m >= KLINE_TMP_MAX) break;
            if (!cJSON_IsString(it)) continue;
            em_row_to_bar(it->valuestring, &s_tmp[m]);
            m++;
        }
    }
    cJSON_Delete(root);
    return kline_finish(code, out, m, src);
}

/* 腾讯日K(https 直连) */
static bool kline_from_tx(const char *code, kline_t *out, const char *src)
{
    char sym[16];
    code_to_symbol(code, sym, sizeof(sym));
    char url[256];
    snprintf(url, sizeof(url), TX_KLINE_URL, sym);

    char *buf = net_http_get(url);
    if (!buf) {
        kerr_add(src, net_err_short_name(net_http_last_err()));
        return false;
    }
    if (net_http_last_status() != 200) {
        kerr_add_status(src, net_http_last_status());
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "%s 腾讯K线响应不是合法JSON, 前100字节: %.100s", code, buf);
        kerr_add(src, "JSON");
        return false;
    }

    int m = 0;
    cJSON *data  = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *snode = data ? cJSON_GetObjectItemCaseSensitive(data, sym) : NULL;
    cJSON *bars  = snode ? cJSON_GetObjectItemCaseSensitive(snode, "qfqday") : NULL;
    if (!bars) bars = snode ? cJSON_GetObjectItemCaseSensitive(snode, "day") : NULL;

    if (bars && cJSON_IsArray(bars)) {
        cJSON *b;
        cJSON_ArrayForEach(b, bars) {
            if (m >= KLINE_TMP_MAX) break;
            if (!cJSON_IsArray(b)) continue;
            cJSON *d  = cJSON_GetArrayItem(b, 0);
            cJSON *o  = cJSON_GetArrayItem(b, 1);
            cJSON *c  = cJSON_GetArrayItem(b, 2);
            cJSON *hi = cJSON_GetArrayItem(b, 3);
            cJSON *lo = cJSON_GetArrayItem(b, 4);
            cJSON *v  = cJSON_GetArrayItem(b, 5);
            memset(&s_tmp[m], 0, sizeof(s_tmp[m]));
            if (d && cJSON_IsString(d)) {
                snprintf(s_tmp[m].date, sizeof(s_tmp[m].date), "%s", d->valuestring);
            }
            s_tmp[m].open   = (o  && cJSON_IsString(o))  ? atof(o->valuestring)  : 0;
            s_tmp[m].close  = (c  && cJSON_IsString(c))  ? atof(c->valuestring)  : 0;
            s_tmp[m].high   = (hi && cJSON_IsString(hi)) ? atof(hi->valuestring) : 0;
            s_tmp[m].low    = (lo && cJSON_IsString(lo)) ? atof(lo->valuestring) : 0;
            s_tmp[m].volume = (v  && cJSON_IsString(v))  ? atof(v->valuestring)  : 0;
            m++;
        }
    }
    cJSON_Delete(root);
    return kline_finish(code, out, m, src);
}

bool stock_fetch_kline(const char *code, kline_t *out)
{
    if (!out || !code) return false;
    out->count = 0;
    s_kerr[0] = '\0';

    if (kline_from_em(code, out, EM_KLINE_URL, "东财")) return true;
    if (kline_from_tx(code, out, "腾讯"))               return true;

    ESP_LOGW(TAG, "%s K线全部数据源失败:\n%s", code, s_kerr);
    return false;
}
