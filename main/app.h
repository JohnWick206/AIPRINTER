// 模块间接口。C / C++ 混编，PNGdec 是 C++，其余是 C。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ───────── 屏幕：GMT032-8P 3.2" ST7789，240x320，无显存偏移 ─────────
#define LCD_W 240
#define LCD_H 320

// 1 = 只做屏幕自检：红绿蓝闪三下 + 八条彩条后停住，不初始化 LVGL。
//     用来单独确认 SPI 通路、初始化序列、字节序和分辨率。
// 0 = 进正常流程（受 ANIM_DEMO 控制走动画还是走完整业务）
#define SCREEN_TEST 0

// 1 = 只跑点阵加载动画，上电即开始，不连 WiFi、不接串口（独立看效果用）
// 0 = 正常业务流程：串口输入 → 云端出图 → 屏上显示
#define ANIM_DEMO 1

// 1 = 播 GrokBot 表情动画（预烘焙图集，优先级高于下面两个）
#define GROKBOT_DEMO 1

// 演示跑哪一个（仅 ANIM_DEMO=1 时有效）：
//   1 = Refine Frame 等待态（扫光带 + 状态胶囊），按五个阶段循环
//   0 = 旧的点阵光场（DotField 移植版）
#define DEMO_REFINE 1

// 初始化 SPI + ST7789，注册 LVGL 显示驱动，并启动 LVGL 刷新任务
void display_init(void);

// LVGL 不是线程安全的。任何跨任务访问 UI 的地方都要先加锁
void display_lock(void);
void display_unlock(void);

// 整屏绘制缓冲（LV_COLOR_16_SWAP=1 布局，每个像素已是 MSB first 的 RGB565）。
// 只有在持有 display_lock 且 LVGL 不重绘时才可直接写，比如跑点阵动画的时候
uint16_t *display_fb(void);

// 把整屏缓冲推给面板。调用前必须已持有 display_lock
void display_present(void);

// 分块推屏：把内部 RAM 里一段 240xh 的条带（已字节交换的 RGB565）推到 y0 处。
// 点阵动画的点会移动缩放，必须每帧整幅重绘，用条带缓冲才能把绘制藏进 DMA 里。
// 调用前必须已持有 display_lock。
void display_push_band(int y0, int h, const uint16_t *buf);

// 等上一次 display_push_band 的 DMA 完成。与 push 成对使用，实现双缓冲流水
void display_wait_flush(void);

// ───────── 界面（ui.c）─────────
void ui_init(void);

// 显示一行中文状态。progress 0..100 显示进度条，<0 隐藏
void ui_set_status(const char *cn, int progress);

// 状态下面那行小字（提示词、剩余秒数等）
void ui_set_hint(const char *text);

// 把解码好的图显示出来（rgb565 大端，即 LV_COLOR_16_SWAP=1 的内存布局）。
// 调用后 buffer 仍被 LVGL 引用，调用方不要释放或改写
void ui_show_image(uint16_t *rgb565, int w, int h);

// 整屏标脏，逼 LVGL 下一帧全量重画。
// 点阵动画直接写帧缓冲，结束后必须调这个，否则 LVGL 不知道该重画
void ui_redraw(void);

// ───────── Refine Frame（refine.c）─────────
// 图片生成等待态的界面：深灰纸面 + 扫光带 + 左下角状态胶囊。
// 状态机和云端流程一一对应：排队 → 生成中 → 下载中 → 完成 / 失败
typedef enum {
    RF_QUEUED = 0,
    RF_GENERATING,
    RF_REFINING,
    RF_COMPLETE,
    RF_ERROR
} rf_status_t;

void rf_init(void);
void rf_set_status(rf_status_t st);

// ───────── GrokBot 表情动画（grokbot.c）─────────
// 53 个状态自动轮播，每个状态正放一遍后切换
void grokbot_start(void);
int grokbot_state_count(void);

// ───────── 点阵加载动画（anim.c）─────────
// IMAGE2 / ChatGPT 生图那种「点阵光场」：点不动，两团光在上面游走
void anim_init(void);
void anim_start(void);
void anim_stop(void);
bool anim_active(void);

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
