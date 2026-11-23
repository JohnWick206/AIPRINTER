// 串口一行提示词 → 云端出图（480x640）→ 解码缩放 → 0.96" 屏显示
//
// 纯 ESP-IDF 工程：CMake + Ninja，无 Arduino 层。
// 屏参数已实测：MADCTL=0xA8，160x80 横向，显存偏移 (1,26)
// 界面由 LVGL 8.4 渲染，中文用 4bpp 抗锯齿字模（font_cn_24 / font_cn_16）。

#include "app.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "app";

static EventGroupHandle_t s_wifi_evt;
#define WIFI_OK_BIT BIT0

// ───────── WiFi ─────────
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected reason=%d, retry", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_evt, WIFI_OK_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_evt, WIFI_OK_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);   // 热点只支持 20M 时自动回落

    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "RSSI %d dBm  channel %d", ap.rssi, ap.primary);
    }
    ESP_LOGI(TAG, "heap internal %u  psram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ───────── JSON 转义 ─────────
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (; *src && o + 2 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c >= 0x20)         { dst[o++] = (char)c; }
    }
    dst[o] = 0;
}

// ───────── 主流程 ─────────
static void show_error(const char *msg)
{
    anim_stop();                 // 先把屏幕从点阵动画手里收回来
    ESP_LOGE(TAG, "!! %s", msg);
    ui_set_status("出错", -1);
    ui_set_hint(msg);
}

// 上次请求的提示词与发起时刻（限流用）
static char    s_last_prompt[256];
static int64_t s_last_try_us;

// 上一张图，换图时释放
static uint16_t *s_photo;

// 冷却期内返回 true，并把剩余秒数写进 sec
static bool cooldown_left(long *sec)
{
    if (s_last_try_us == 0) return false;
    int64_t el = (esp_timer_get_time() - s_last_try_us) / 1000000;
    int64_t left = (int64_t)MIN_INTERVAL_S - el;
    if (left <= 0) return false;
    if (sec) *sec = (long)left;
    return true;
}

static void run_once(const char *prompt)
{
    // 客户端先挡一道，避免明知超限还去撞 429
    long left = 0;
    if (cooldown_left(&left)) {
        ESP_LOGW(TAG, "cooldown: %ld s left (server quota 2 img/min)", left);
        ESP_LOGW(TAG, "wait, then type: retry");
        char buf[40];
        snprintf(buf, sizeof(buf), "请稍后重试 %lds", left);
        ui_set_status("等待", -1);
        ui_set_hint(buf);
        return;
    }

    char esc[512];
    char body[1024];
    json_escape(prompt, esc, sizeof(esc));

    int n = snprintf(body, sizeof(body),
                     "{\"model\":\"%s\",\"prompt\":\"%s%s\",\"image_size\":\"%s\",\"num_inference_steps\":%d}",
                     MODEL_NAME, esc, IMG_STYLE, IMG_SIZE, IMG_STEPS);
    if (n < 0 || n >= (int)sizeof(body)) {
        show_error("body too long");
        return;
    }

    ESP_LOGI(TAG, "\n=== generate: %s ===", prompt);
    int64_t t_all = esp_timer_get_time();
    s_last_try_us = t_all;

    // 1) 请求生成
    // 整条链路（生成 → 下载 → 解码）都显示点阵光场，比进度条更像「正在生成」
    anim_start();

    int rlen = 0, status = 0;
    char *resp = http_post_json(API_URL, body, &rlen, &status);
    if (!resp) {
        if (status == 429) {
            // 服务端限流：账号级 IPM=2，每分钟只能出 2 张，等满 60 s 才恢复
            ESP_LOGE(TAG, "!! 429 rate limited (IPM=2: max 2 images per minute)");
            ESP_LOGE(TAG, "   server quota resets after ~60 s. then type: retry");
            anim_stop();
            ui_set_status("限流", -1);
            ui_set_hint("等一分钟再输入 retry");
        } else if (status == 401 || status == 403) {
            ESP_LOGE(TAG, "!! %d auth failed, check API_KEY in config.h", status);
            show_error("auth failed");
        } else {
            show_error("POST failed");
        }
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) { show_error("JSON parse failed"); return; }

    cJSON *images = cJSON_GetObjectItem(root, "images");
    cJSON *first = images ? cJSON_GetArrayItem(images, 0) : NULL;
    cJSON *jurl = first ? cJSON_GetObjectItem(first, "url") : NULL;
    char *url = (jurl && cJSON_IsString(jurl)) ? strdup(jurl->valuestring) : NULL;
    cJSON_Delete(root);
    if (!url) { show_error("no url in response"); return; }
    ESP_LOGI(TAG, "url %d bytes", (int)strlen(url));

    // 2) 下载（动画继续跑，不打断）
    size_t plen = 0;
    uint8_t *png = http_get_all(url, &plen);
    free(url);
    if (!png) { show_error("download failed"); return; }

    // 3) 解码 + 缩放
    int dw = 0, dh = 0;
    uint16_t *photo = image_decode_fit(png, plen, LCD_W, LCD_H, &dw, &dh);
    free(png);
    if (!photo) { show_error("decode failed"); return; }

    // 4) 上屏
    anim_stop();            // 交还屏幕，LVGL 全量重画
    ui_show_image(photo, dw, dh);
    free(s_photo);          // 上一张此时已经没人引用了
    s_photo = photo;

    ESP_LOGI(TAG, "total %.2f s", (esp_timer_get_time() - t_all) / 1000000.0);
}

// ───────── 串口输入 ─────────
static void input_task(void *arg)
{
    static char line[256];
    int n = 0;
    bool last_cr = false;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " READY - type a prompt, then press Enter");
    ESP_LOGI(TAG, " commands: retry = resend last prompt");
    ESP_LOGI(TAG, " quota: 2 images/min, %d s between requests", MIN_INTERVAL_S);
    ESP_LOGI(TAG, "==============================================");
    printf("> ");
    fflush(stdout);

    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        // 退格：不同终端发 0x7F 或 0x08
        if (c == 0x7F || c == 0x08) {
            if (n > 0) {
                n--;
                printf("\b \b");            // 擦掉一个字符
                fflush(stdout);
            }
            continue;
        }
        // 其它控制字符直接忽略（方向键、Tab 等会发转义序列）
        if (c < 0x20 && c != '\r' && c != '\n') continue;

        // 串口终端按回车通常只发 \r，也有的发 \r\n，两种都要认
        if (c == '\r' || c == '\n') {
            if (c == '\n' && last_cr) { last_cr = false; continue; }  // 吃掉 \r\n 的后半截
            last_cr = (c == '\r');
            printf("\r\n");
            line[n] = 0;
            if (n > 0) {
                if (strcmp(line, "retry") == 0 || strcmp(line, "again") == 0) {
                    if (s_last_prompt[0]) {
                        ESP_LOGI(TAG, "retry: %s", s_last_prompt);
                        run_once(s_last_prompt);
                    } else {
                        ESP_LOGW(TAG, "nothing to retry yet");
                    }
                } else {
                    strncpy(s_last_prompt, line, sizeof(s_last_prompt) - 1);
                    s_last_prompt[sizeof(s_last_prompt) - 1] = 0;
                    run_once(line);
                }
            } else {
                ESP_LOGI(TAG, "(empty line ignored)");
            }
            n = 0;
            printf("> ");
            fflush(stdout);
        } else if (n < (int)sizeof(line) - 1) {
            last_cr = false;
            line[n++] = (char)c;
            putchar(c);          // 回显
            fflush(stdout);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

#if SCREEN_TEST
    // 只验证屏幕硬件：不初始化 LVGL、不连 WiFi、不碰动画
    display_init();
    ESP_LOGI(TAG, "=== SCREEN TEST MODE (no LVGL, no wifi) ===");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#elif ANIM_DEMO
#if GROKBOT_DEMO
    // 播 GrokBot 表情动画：53 个状态自动轮播，不连 WiFi
    display_init();
    grokbot_start();
    ESP_LOGI(TAG, "=== GROKBOT DEMO: %d states, wifi disabled ===", grokbot_state_count());
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
#elif DEMO_REFINE
    // 演示 Refine Frame 的等待态：纸面 + 扫光带 + 状态胶囊，五阶段循环
    display_init();
    ui_init();
    rf_init();
    ESP_LOGI(TAG, "=== REFINE FRAME DEMO (wifi disabled) ===");
    for (;;) {
        rf_set_status(RF_QUEUED);     ESP_LOGI(TAG, "status: queued");     vTaskDelay(pdMS_TO_TICKS(2500));
        rf_set_status(RF_GENERATING); ESP_LOGI(TAG, "status: generating"); vTaskDelay(pdMS_TO_TICKS(3500));
        rf_set_status(RF_REFINING);   ESP_LOGI(TAG, "status: refining");   vTaskDelay(pdMS_TO_TICKS(3500));
        rf_set_status(RF_COMPLETE);   ESP_LOGI(TAG, "status: complete");   vTaskDelay(pdMS_TO_TICKS(3000));
        rf_set_status(RF_ERROR);      ESP_LOGI(TAG, "status: error");      vTaskDelay(pdMS_TO_TICKS(3000));
    }
#else
    // 独立演示：上电直接进点阵动画，不连 WiFi。
    display_init();
    ui_init();
    anim_init();
    ESP_LOGI(TAG, "=== ANIM DEMO: dot field only, wifi disabled ===");
    anim_start();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
#else
    display_init();
    ui_init();
    anim_init();
    ui_set_status("启动", -1);
    ESP_LOGI(TAG, "=== serial prompt -> image -> LCD (LVGL) ===");
    ESP_LOGI(TAG, "image %s  lcd %dx%d", IMG_SIZE, LCD_W, LCD_H);

    ui_set_status("连接中", -1);
    wifi_start();

    ui_set_status("就绪", -1);
    ui_set_hint(DEFAULT_PROMPT);
    ESP_LOGI(TAG, "example prompt: %s", DEFAULT_PROMPT);

    xTaskCreate(input_task, "input", 12288, NULL, 5, NULL);
#endif
}
