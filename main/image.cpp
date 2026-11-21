// PNG 解码 + 等比缩放到指定框内
//
// 用 PNGdec（C++），对外只暴露一个 C 函数。
// 缩放用面积平均（源 480x640 → 60x80 正好 1/8），比最近邻干净，
// 代价是每行多一圈加法，实测可以忽略。
//
// 输出的是紧凑的 dw*dh 缓冲，不是整屏。上一版直接写屏幕帧缓冲，
// 接 LVGL 之后帧缓冲归 LVGL 所有，必须改成独立缓冲。

#include "app.h"

#include <string.h>
#include <stdlib.h>

#include "PNGdec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "img";

static PNG      s_png;
static uint16_t s_line[2048];      // 单行 RGB565，480 宽足够

static uint16_t *s_dst;            // 目标缓冲
static int s_stride;               // 每行像素数
static int s_sw, s_sh;             // 源尺寸
static int s_dw, s_dh;             // 目标图像尺寸
static int s_last_ty;
static int s_rows;                 // 当前目标行累计了多少源行
static uint32_t *s_acc;            // s_dw * 3 累加器

static void compute_fit(int boxW, int boxH)
{
    // 整数版 min(boxW/sw, boxH/sh)，避免浮点
    if ((long)boxW * s_sh <= (long)boxH * s_sw) {
        s_dw = boxW;
        s_dh = (int)((long)s_sh * boxW / s_sw);
    } else {
        s_dh = boxH;
        s_dw = (int)((long)s_sw * boxH / s_sh);
    }
    if (s_dw < 1) s_dw = 1;
    if (s_dh < 1) s_dh = 1;
}

// 把一个目标行写进缓冲（大端 RGB565，即 LV_COLOR_16_SWAP=1 的内存布局）
static void flush_row(int ty)
{
    if (ty < 0 || ty >= s_dh || s_rows == 0) return;

    uint16_t *dst = s_dst + (size_t)ty * s_stride;
    for (int tx = 0; tx < s_dw; tx++) {
        int x0 = (int)((long)tx * s_sw / s_dw);
        int x1 = (int)((long)(tx + 1) * s_sw / s_dw);
        if (x1 > s_sw) x1 = s_sw;
        if (x1 <= x0) x1 = x0 + 1;

        uint32_t div = (uint32_t)(x1 - x0) * (uint32_t)s_rows;
        uint32_t r = s_acc[tx * 3 + 0] / div;
        uint32_t g = s_acc[tx * 3 + 1] / div;
        uint32_t b = s_acc[tx * 3 + 2] / div;

        uint16_t v = (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
        dst[tx] = (uint16_t)((v >> 8) | (v << 8));
    }
}

static int draw_cb(PNGDRAW *pDraw)
{
    if (pDraw->y >= s_sh) return 1;

    // LITTLE_ENDIAN = 主机序，方便直接拆位计算
    s_png.getLineAsRGB565(pDraw, s_line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

    int ty = (int)((long)pDraw->y * s_dh / s_sh);
    if (ty >= s_dh) ty = s_dh - 1;

    if (ty != s_last_ty) {
        flush_row(s_last_ty);
        memset(s_acc, 0, sizeof(uint32_t) * 3 * (size_t)s_dw);
        s_rows = 0;
        s_last_ty = ty;
    }

    for (int tx = 0; tx < s_dw; tx++) {
        int x0 = (int)((long)tx * s_sw / s_dw);
        int x1 = (int)((long)(tx + 1) * s_sw / s_dw);
        if (x1 > s_sw) x1 = s_sw;
        if (x1 <= x0) x1 = x0 + 1;

        uint32_t r = 0, g = 0, b = 0;
        for (int x = x0; x < x1; x++) {
            uint16_t v = s_line[x];
            r += (v >> 11) & 0x1F;
            g += (v >> 5) & 0x3F;
            b += v & 0x1F;
        }
        s_acc[tx * 3 + 0] += r;
        s_acc[tx * 3 + 1] += g;
        s_acc[tx * 3 + 2] += b;
    }
    s_rows++;
    return 1;
}

extern "C" uint16_t *image_decode_fit(const uint8_t *png, size_t len, int boxW, int boxH,
                                      int *out_w, int *out_h)
{
    int rc = s_png.openRAM((uint8_t *)png, (int)len, draw_cb);
    if (rc != PNG_SUCCESS) {
        ESP_LOGE(TAG, "openRAM failed %d", rc);
        return nullptr;
    }

    s_sw = s_png.getWidth();
    s_sh = s_png.getHeight();
    if (s_sw <= 0 || s_sh <= 0 || s_sw > 2048 || s_sh > 2048) {
        ESP_LOGE(TAG, "bad size %dx%d", s_sw, s_sh);
        s_png.close();
        return nullptr;
    }

    compute_fit(boxW, boxH);

    size_t bytes = (size_t)s_dw * s_dh * 2;
    uint16_t *buf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "psram alloc %u failed", (unsigned)bytes);
        s_png.close();
        return nullptr;
    }
    memset(buf, 0, bytes);

    s_dst    = buf;
    s_stride = s_dw;

    s_acc = (uint32_t *)malloc(sizeof(uint32_t) * 3 * (size_t)s_dw);
    if (!s_acc) {
        s_png.close();
        free(buf);
        return nullptr;
    }
    memset(s_acc, 0, sizeof(uint32_t) * 3 * (size_t)s_dw);
    s_rows = 0;
    s_last_ty = -1;

    int64_t t0 = esp_timer_get_time();
    rc = s_png.decode(nullptr, 0);
    flush_row(s_last_ty);                    // 最后一行
    int64_t dt = (esp_timer_get_time() - t0) / 1000;
    s_png.close();
    free(s_acc);
    s_acc = nullptr;

    if (rc != PNG_SUCCESS) {
        ESP_LOGE(TAG, "decode failed %d", rc);
        free(buf);
        return nullptr;
    }

    if (out_w) *out_w = s_dw;
    if (out_h) *out_h = s_dh;
    ESP_LOGI(TAG, "decoded %lld ms  %dx%d -> %dx%d", (long long)dt, s_sw, s_sh, s_dw, s_dh);
    return buf;
}
