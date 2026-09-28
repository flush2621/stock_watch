/*
 * net.h -- WiFi + NTP + 通用 HTTP GET 客户端。
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool net_wifi_connect(const char *ssid, const char *pass, int timeout_s);
bool net_wifi_is_connected(void);
bool net_ntp_sync(void);

/** IP address of STA netif, or "" if not connected. */
const char *net_ip_str(void);

/** RSSI in dBm (0 if not available). */
int net_rssi_dbm(void);

/** 通用 HTTP GET。成功返回指向内部缓冲区的指针(以 '\0' 结尾, 数据在下次调用前有效);
 *  失败返回 NULL。缓冲区 16KB, 足以容纳日K线 JSON(~3.5KB/40根)。 */
char *net_http_get(const char *url);

/** 最近一次 net_http_get 的结果(供上层在界面上显示失败原因)。 */
esp_err_t net_http_last_err(void);
int       net_http_last_status(void);

/** esp_err_to_name() 去掉 "ESP_ERR_" 前缀后的短名字。 */
const char *net_err_short_name(esp_err_t err);

#ifdef __cplusplus
}
#endif
