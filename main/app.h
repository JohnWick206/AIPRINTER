// 模块间接口。C / C++ 混编，PNGdec 是 C++，其余是 C。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ───────── 屏幕：0.96" ST7735，160x80，MADCTL=0xA8，显存偏移 (1,26) ─────────
#define LCD_W 160
#define LCD_H 80

// 初始化 SPI + ST7735，注册 LVGL 显示驱动，并启动 LVGL 刷新任务
void display_init(void);

// LVGL 不是线程安全的。任何跨任务访问 UI 的地方都要先加锁
void display_lock(void);
void display_unlock(void);

// ───────── 界面（ui.c）─────────
void ui_init(void);

// 显示一行中文状态。progress 0..100 显示进度条，<0 隐藏
void ui_set_status(const char *cn, int progress);

// 状态下面那行小字（提示词、剩余秒数等）
void ui_set_hint(const char *text);

// 把解码好的图显示出来（rgb565 大端，即 LV_COLOR_16_SWAP=1 的内存布局）。
// 调用后 buffer 仍被 LVGL 引用，调用方不要释放或改写
void ui_show_image(uint16_t *rgb565, int w, int h);

// ───────── 网络 ─────────
// POST，返回 malloc 的响应字符串（调用方 free），失败返回 NULL。
// out_status 带回 HTTP 状态码（429 = 服务端限流，详见 config.h 的 IPM 说明）
char *http_post_json(const char *url, const char *body, int *out_len, int *out_status);

// GET，返回 PSRAM 里的完整图片字节（调用方 free），失败返回 NULL
uint8_t *http_get_all(const char *url, size_t *out_len);

// ───────── 图像 ─────────
// 解码 PNG 并等比缩放，装进 boxW x boxH 的框里（居中留白）。
// 返回 PSRAM 里的紧凑 RGB565 大端缓冲（调用方 free），失败返回 NULL。
uint16_t *image_decode_fit(const uint8_t *png, size_t len, int boxW, int boxH,
                           int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
