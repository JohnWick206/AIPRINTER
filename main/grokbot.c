// GrokBot 表情动画播放器实现。
//
// 关键约束：屏幕是 240x320 的 SPI ST7789，40 MHz 下整屏要 31 ms。
// 角色只占 240x240，所以只推中间那 240 行（23 ms），上下各留 40 px 黑边。
//
// 每帧流程：zlib 解压 → XOR 还原 → 4bit 展开成 RGB565（字节交换）→ 条带推屏。
// 解压用 ROM 里的 miniz，不占 flash、不占 RAM。

#include "grokbot.h"
#include "app.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"

static const char *TAG = "grokbot";

// 图集放在独立的 data 分区，烧录时用 esptool 单独写，不参与编译。
// 用 mmap 映射进数据地址空间，之后就能当普通指针读。
#define GB_ATLAS_SUBTYPE 0x40
static esp_partition_mmap_handle_t s_mmap;

#define GB_MAX_CLIPS 64
#define GB_HEADER_SIZE 16
#define GB_CLIP_SIZE 32
#define GB_FRAME_INDEX_SIZE 8

typedef struct {
    char name[16];
    uint16_t frames;
    uint16_t start;
    uint32_t offset;
    uint32_t size;
    uint32_t packed;
} gb_clip_t;

static const uint8_t *s_atlas;
static size_t s_atlas_size;
static uint16_t s_w, s_h;
static uint8_t s_fps;
static uint8_t s_clip_count;
static gb_clip_t s_clips[GB_MAX_CLIPS];
static uint32_t s_packed_size;

// PSRAM：一帧 4bit 数据 + 一份解压出来的 delta + 展开后的 RGB565 条带
static uint8_t *s_packed;
static uint8_t *s_delta;
static uint16_t *s_band;

static uint16_t s_pal[16];

static int s_index;             // 当前状态
static volatile int s_want = -1; // 外部请求切换

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 16 级灰度 → RGB565，并做字节交换（LV_COLOR_16_SWAP=1 的内存布局）
static void build_palette(void)
{
    for (int i = 0; i < 16; ++i) {
        uint8_t v = (uint8_t)(i * 17);
        uint16_t c = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
        s_pal[i] = (uint16_t)((c >> 8) | (c << 8));
    }
}

static bool map_atlas(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           GB_ATLAS_SUBTYPE, NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "atlas partition not found (subtype 0x%02x)", GB_ATLAS_SUBTYPE);
        return false;
    }
    const void *ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                       &ptr, &s_mmap);
    if (err != ESP_OK || ptr == NULL) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return false;
    }
    s_atlas = (const uint8_t *)ptr;
    s_atlas_size = part->size;
    ESP_LOGI(TAG, "atlas mmap @0x%08x %u KB", (unsigned)part->address,
             (unsigned)(part->size / 1024));
    return true;
}

static bool parse_atlas(void)
{
    if (!map_atlas()) return false;
    if (s_atlas_size < GB_HEADER_SIZE || memcmp(s_atlas, "GBZ1", 4) != 0) {
        ESP_LOGE(TAG, "atlas magic mismatch (%u bytes)", (unsigned)s_atlas_size);
        return false;
    }
    uint16_t version = rd16(s_atlas + 4);
    s_w = rd16(s_atlas + 6);
    s_h = rd16(s_atlas + 8);
    s_fps = s_atlas[10];
    s_clip_count = s_atlas[11];
    if (version != 1 || s_w == 0 || s_h == 0 || s_clip_count == 0 || s_clip_count > GB_MAX_CLIPS) {
        ESP_LOGE(TAG, "atlas header invalid v%u %ux%u clips=%u", version, s_w, s_h, s_clip_count);
        return false;
    }
    if (s_w > LCD_W || s_h > LCD_H) {
        ESP_LOGE(TAG, "atlas %ux%u exceeds panel %dx%d", s_w, s_h, LCD_W, LCD_H);
        return false;
    }
    for (int i = 0; i < s_clip_count; ++i) {
        const uint8_t *p = s_atlas + GB_HEADER_SIZE + (size_t)i * GB_CLIP_SIZE;
        memcpy(s_clips[i].name, p, 16);
        s_clips[i].name[15] = '\0';
        s_clips[i].frames = rd16(p + 16);
        s_clips[i].start = rd16(p + 18);
        s_clips[i].offset = rd32(p + 20);
        s_clips[i].size = rd32(p + 24);
        s_clips[i].packed = rd32(p + 28);
    }
    s_packed_size = (uint32_t)s_w * s_h / 2;
    ESP_LOGI(TAG, "atlas %ux%u %u states %u fps %u KB", s_w, s_h, s_clip_count, s_fps,
             (unsigned)(s_atlas_size / 1024));
    return true;
}

static bool alloc_buffers(void)
{
    s_packed = heap_caps_malloc(s_packed_size, MALLOC_CAP_SPIRAM);
    s_delta = heap_caps_malloc(s_packed_size, MALLOC_CAP_SPIRAM);
    s_band = heap_caps_malloc((size_t)s_w * s_h * 2, MALLOC_CAP_SPIRAM);
    if (!s_packed || !s_delta || !s_band) {
        ESP_LOGE(TAG, "psram alloc failed (%u + %u + %u)",
                 (unsigned)s_packed_size, (unsigned)s_packed_size, (unsigned)(s_w * s_h * 2));
        return false;
    }
    memset(s_packed, 0, s_packed_size);
    return true;
}

// 取第 idx 帧的压缩数据位置
static bool frame_at(int clip, int idx, const uint8_t **payload, uint32_t *len, bool *delta)
{
    if (clip < 0 || clip >= s_clip_count) return false;
    const gb_clip_t *c = &s_clips[clip];
    if (idx < 0 || idx >= c->frames) return false;

    const uint8_t *e = s_atlas + c->offset + (size_t)idx * GB_FRAME_INDEX_SIZE;
    uint32_t frame_off = rd32(e);
    uint32_t lf = rd32(e + 4);
    uint32_t plen = lf & 0x7FFFFFFFu;
    bool is_delta = (lf >> 31) != 0;

    const uint8_t *f = s_atlas + c->offset + frame_off;
    if (f + 8 > s_atlas + s_atlas_size) return false;
    *len = rd32(f);
    *delta = is_delta;
    *payload = f + 8;
    if ((size_t)(*payload - s_atlas) + *len > s_atlas_size) return false;
    (void)plen;
    return true;
}

// 解压一帧；delta 时异或到 s_packed 上
static bool decode_frame(const uint8_t *payload, uint32_t len, bool delta, bool backward)
{
    size_t got = tinfl_decompress_mem_to_mem(s_delta, s_packed_size, payload, len,
                                             TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (got != s_packed_size) {
        ESP_LOGE(TAG, "inflate %u/%u", (unsigned)got, (unsigned)s_packed_size);
        return false;
    }
    if (backward) {
        for (uint32_t i = 0; i < s_packed_size; ++i) s_packed[i] ^= s_delta[i];
    } else {
        for (uint32_t i = 0; i < s_packed_size; ++i)
            s_packed[i] = delta ? (s_packed[i] ^ s_delta[i]) : s_delta[i];
    }
    return true;
}

// 4bit → RGB565 条带
static void expand_band(void)
{
    uint32_t n = s_packed_size;
    uint16_t *o = s_band;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t b = s_packed[i];
        o[2 * i] = s_pal[b >> 4];
        o[2 * i + 1] = s_pal[b & 0x0F];
    }
}

// 从第 0 帧重建到 target（XOR 基准必须连续）
static bool rebuild_to(int clip, int target)
{
    for (int i = 0; i <= target; ++i) {
        const uint8_t *d;
        uint32_t len;
        bool delta;
        if (!frame_at(clip, i, &d, &len, &delta)) return false;
        if (!decode_frame(d, len, delta, false)) return false;
    }
    return true;
}

static void push_frame(void)
{
    int y0 = (LCD_H - s_h) / 2;
    display_lock();
    display_push_band(y0, s_h, s_band);
    display_unlock();
    display_wait_flush();
}

static void clear_screen(void)
{
    display_lock();
    uint16_t *fb = display_fb();
    if (fb) {
        memset(fb, 0, (size_t)LCD_W * LCD_H * 2);
        display_present();
    }
    display_unlock();
}

void grokbot_select(int index)
{
    if (index < 0 || index >= s_clip_count) return;
    s_want = index;
}

int grokbot_state_count(void) { return s_clip_count; }
int grokbot_width(void) { return s_w; }
int grokbot_height(void) { return s_h; }
int grokbot_fps(void) { return s_fps; }
const char *grokbot_state_name(int index)
{
    if (index < 0 || index >= s_clip_count) return "";
    return s_clips[index].name;
}

static void grokbot_task(void *arg)
{
    (void)arg;
    const int64_t frame_us = 1000000LL / (s_fps ? s_fps : 30);
    int clip = 0;
    int64_t next = esp_timer_get_time();

    clear_screen();
    // 先把第一个状态的第 0 帧建好
    if (!rebuild_to(clip, s_clips[clip].start)) {
        ESP_LOGE(TAG, "initial rebuild failed");
        vTaskDelete(NULL);
        return;
    }
    int fi = s_clips[clip].start;
    ESP_LOGI(TAG, "GROKBOT_READY states=%d fps=%u size=%ux%u", s_clip_count, s_fps, s_w, s_h);

    for (;;) {
        // 外部切换
        if (s_want >= 0) {
            int want = s_want;
            s_want = -1;
            if (want != clip) {
                clip = want;
                if (!rebuild_to(clip, s_clips[clip].start)) {
                    ESP_LOGE(TAG, "rebuild %d failed", clip);
                }
                fi = s_clips[clip].start;
                continue;
            }
        }

        const uint8_t *d;
        uint32_t len;
        bool delta;
        if (!frame_at(clip, fi, &d, &len, &delta)) {
            fi = s_clips[clip].start;
            continue;
        }
        if (decode_frame(d, len, delta, false)) {
            expand_band();
            push_frame();
        }

        fi++;
        if (fi >= s_clips[clip].frames) {
            // 一个状态播完，切下一个；全部播完回到第一个
            clip = (clip + 1) % s_clip_count;
            if (!rebuild_to(clip, s_clips[clip].start)) {
                ESP_LOGE(TAG, "rebuild %d failed", clip);
            }
            fi = s_clips[clip].start;
            ESP_LOGI(TAG, "state -> %s (%d/%d)", s_clips[clip].name, clip + 1, s_clip_count);
        }

        next += frame_us;
        int64_t now = esp_timer_get_time();
        if (next < now) next = now;
        int64_t wait_us = next - now;
        if (wait_us > 0) {
            int wait_ms = (int)((wait_us + 500) / 1000);
            if (wait_ms < 1) wait_ms = 1;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }
}

void grokbot_start(void)
{
    if (!parse_atlas()) return;
    if (!alloc_buffers()) return;
    build_palette();
    ESP_LOGI(TAG, "starting player, atlas %u KB", (unsigned)(s_atlas_size / 1024));
    // tinfl 的解压器结构本身约 11 KB，放在栈上，栈必须给够
    xTaskCreatePinnedToCore(grokbot_task, "grokbot", 16384, NULL, 5, NULL, 1);
}
