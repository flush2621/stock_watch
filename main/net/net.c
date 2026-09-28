/*
 * net.c -- WiFi, NTP time, and a generic HTTP GET client.
 * Adapted from small_tv's net.c; weather/geo removed, generic GET added.
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "net.h"
#include "app_config.h"

static const char *TAG = "net";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

static EventGroupHandle_t s_wifi_events;
static int  s_retry = 0;
static bool s_connected = false;
static bool s_wifi_inited = false;
static esp_netif_t *s_sta_netif = NULL;
static char s_ip_str[16] = "";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip_str[0] = '\0';
        if (s_retry < MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "wifi disconnect, retry %d", s_retry);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got ip: %s", s_ip_str);
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

bool net_wifi_is_connected(void) { return s_connected; }

const char *net_ip_str(void) { return s_ip_str; }

int net_rssi_dbm(void)
{
    wifi_ap_record_t ap;
    if (!s_connected) return 0;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

bool net_wifi_connect(const char *ssid, const char *pass, int timeout_s)
{
    if (s_wifi_inited) return s_connected;

    esp_netif_init();
    esp_event_loop_create_default();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wc.sta.sae_pk_mode = WPA3_SAE_PK_MODE_AUTOMATIC;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_events = xEventGroupCreate();
    s_wifi_inited = true;

    ESP_LOGI(TAG, "connecting to wifi %s ...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                          pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_s * 1000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ---------------- NTP ---------------- */
bool net_ntp_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();

    int tries = 0;
    time_t now = 0;
    struct tm ti;
    while (tries < 40) {
        time(&now);
        localtime_r(&now, &ti);
        if (ti.tm_year >= (2024 - 1900)) {
            setenv("TZ", "CST-8", 1);   /* Asia/Shanghai, UTC+8 */
            tzset();
            time(&now); localtime_r(&now, &ti);
            ESP_LOGI(TAG, "time synced: %s", asctime(&ti));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        tries++;
    }
    ESP_LOGW(TAG, "NTP timeout");
    return false;
}

/* ---------------- Generic HTTP GET ---------------- */
#define HTTP_BUF_SIZE 16384
static char   s_http_buf[HTTP_BUF_SIZE];
static size_t s_http_len = 0;
static esp_err_t s_last_err = ESP_OK;
static int       s_last_status = 0;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA &&
        evt->data_len > 0 &&
        s_http_len + evt->data_len < sizeof(s_http_buf) - 1) {
        memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
        s_http_len += evt->data_len;
    }
    return ESP_OK;
}

char *net_http_get(const char *url)
{
    memset(s_http_buf, 0, sizeof(s_http_buf));
    s_http_len = 0;
    s_last_err = ESP_OK;
    s_last_status = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .max_redirection_count = 5,          /* 有的接口会 301 到 https */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        s_last_err = ESP_FAIL;
        return NULL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    s_last_err = err;
    s_last_status = status;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_get failed: %s (%s)", esp_err_to_name(err), url);
        return NULL;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "http status %d: %s", status, url);
    }

    s_http_buf[s_http_len] = '\0';
    return s_http_buf;
}

esp_err_t net_http_last_err(void) { return s_last_err; }

int net_http_last_status(void) { return s_last_status; }

/* "ESP_ERR_HTTP_CONNECT" -> "HTTP_CONNECT" (界面显示用, 去掉前缀省宽度) */
const char *net_err_short_name(esp_err_t err)
{
    const char *n = esp_err_to_name(err);
    if (!n) return "?";
    if (strncmp(n, "ESP_ERR_", 8) == 0) return n + 8;
    return n;
}
