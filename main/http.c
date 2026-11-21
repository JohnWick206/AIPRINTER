// HTTPS：POST 拿 url，GET 下载整张 PNG
//
// 两个关键点：
//  1. buffer_size_tx 必须够大——请求行是 "GET <path>"，S3 预签名地址实测 1575 字节，
//     设 1024 会报 HTTP_CLIENT: Out of buffer
//  2. 下载循环只在 read() 返回 <=0 时退出，不靠 Content-Length 猜，避免空转超时

#include "app.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "http";

#define RESP_MAX 8192

static char   s_resp[RESP_MAX];
static size_t s_resp_len;

static esp_err_t on_post_data(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        size_t room = RESP_MAX - 1 - s_resp_len;
        size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        if (n) {
            memcpy(s_resp + s_resp_len, e->data, n);
            s_resp_len += n;
            s_resp[s_resp_len] = 0;   // 必须置零：否则 %.200s 会读到上一轮残留的响应尾巴
        }
    }
    return ESP_OK;
}

char *http_post_json(const char *url, const char *body, int *out_len, int *out_status)
{
    s_resp_len = 0;
    s_resp[0] = 0;
    if (out_status) *out_status = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = on_post_data,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { ESP_LOGE(TAG, "client init failed"); return NULL; }

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", "Bearer " API_KEY);
    esp_http_client_set_post_field(c, body, (int)strlen(body));

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int64_t dt = (esp_timer_get_time() - t0) / 1000;
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST failed %s", esp_err_to_name(err));
        return NULL;
    }
    if (out_status) *out_status = status;
    ESP_LOGI(TAG, "POST %d  %lld ms  %d B", status, (long long)dt, (int)s_resp_len);
    if (status != 200) {
        ESP_LOGE(TAG, "response: %.200s", s_resp);
        return NULL;
    }

    char *out = malloc(s_resp_len + 1);
    if (!out) return NULL;
    memcpy(out, s_resp, s_resp_len);
    out[s_resp_len] = 0;
    if (out_len) *out_len = (int)s_resp_len;
    return out;
}

uint8_t *http_get_all(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 8192,    // 请求行最长可达 1575+ 字节
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { ESP_LOGE(TAG, "client init failed"); return NULL; }

    int64_t t0 = esp_timer_get_time();
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGE(TAG, "open failed (url %d bytes)", (int)strlen(url));
        esp_http_client_cleanup(c);
        return NULL;
    }
    int64_t hdr_done = esp_timer_get_time();
    int64_t len = esp_http_client_fetch_headers(c);
    int64_t hdr_ms = (esp_timer_get_time() - hdr_done) / 1000;
    ESP_LOGI(TAG, "HTTP %d  Content-Length %lld  (tls+header %lld ms)",
             esp_http_client_get_status_code(c), (long long)len, (long long)hdr_ms);

    if (esp_http_client_get_status_code(c) != 200) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }

    size_t cap = (len > 0) ? (size_t)len : (size_t)4 * 1024 * 1024;
    uint8_t *p = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!p) {
        ESP_LOGE(TAG, "PSRAM alloc %u failed", (unsigned)cap);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }

    static uint8_t chunk[16384];
    size_t total = 0;
    int64_t body_start = esp_timer_get_time();
    while (total < cap) {
        int n = esp_http_client_read(c, (char *)chunk, sizeof(chunk));
        if (n <= 0) break;
        size_t room = cap - total;
        size_t cp = (size_t)n < room ? (size_t)n : room;
        memcpy(p + total, chunk, cp);
        total += cp;
        if ((size_t)n > room) break;
    }
    int64_t body_us = esp_timer_get_time() - body_start;

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    double sec = body_us / 1000000.0;
    ESP_LOGI(TAG, "download %u B  %.2f s  %.1f KB/s",
             (unsigned)total, sec, sec > 0 ? (total / 1024.0) / sec : 0.0);
    ESP_LOGI(TAG, "total %.2f s", (esp_timer_get_time() - t0) / 1000000.0);

    if (total == 0) { free(p); return NULL; }
    if (out_len) *out_len = total;
    return p;
}
