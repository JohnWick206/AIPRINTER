// GrokBot 表情动画播放器：在 240x320 屏上播预烘焙的 4bit 灰度帧图集。
//
// 图集格式（GBZ1，全部小端）：
//   0   char[4]  "GBZ1"
//   4   u16      version
//   6   u16      width
//   8   u16      height
//   10  u8       fps
//   11  u8       clip_count
//   12  u32      reserved
//   16  clip[clip_count]  { char name[16]; u16 frames; u16 start; u32 offset; u32 size; u32 packed }
//   每个 clip 的 offset 处是 frames 个索引项 { u32 frame_off; u32 len|flags<<31 }
//   帧数据位于 clip.offset + frame_off：{ u32 len; u8 flags; u8 pad[3]; payload }
//   payload 是 zlib 流，解压后是 width*height/2 字节（4bit 一像素，两像素一字节）。
//   flags bit0 = 1 表示与上一帧做 XOR。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动播放任务（自动按顺序轮播全部状态，每个状态正放一遍后切换）
void grokbot_start(void);

// 图集信息，供日志和调试用
int grokbot_state_count(void);
const char *grokbot_state_name(int index);
int grokbot_width(void);
int grokbot_height(void);
int grokbot_fps(void);

// 立刻切到指定状态（越界会被忽略）
void grokbot_select(int index);

#ifdef __cplusplus
}
#endif
