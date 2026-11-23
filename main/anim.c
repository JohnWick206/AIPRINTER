// DotField 的固件移植版（源自 React Bits 的 DotField 组件）
//
// 原版是 WebGL：每颗点一个 instance quad，顶点着色器算镜头形变、片元着色器画圆并做 AA。
// 这里没有 GPU，所以把它的模型拆成「每帧每点的少量浮点」+「圆的光栅化」两段：
//
//   1. 点在网格上固定不动，但镜头（lens）会把附近的点向外推开并放大 —— 这是 DotField
//      最核心的特征，不是简单的亮度变化（之前那版只改亮度，所以看着像格子屏保）。
//   2. 亮度由 glow 池决定：pool = (1-g²)² * intensity，落在池里的点向 glowColor 靠拢。
//   3. 底色渐变沿 gradientAngle 方向，在 OKLab 空间插值（原版就是这么做的，
//      直接对 sRGB 插值会在中间出现脏灰）。
//   4. 入场时整场从 1.12 倍收拢 + 散焦变亮，1.8 s 内拉实。
//
// 为什么必须每帧整幅重绘：点会移动和缩放，不能像静态点阵那样只改亮度。
// 但整屏 153.6 KB 在 PSRAM 上重绘太慢，所以改成 8 条 240x40 的条带：
// 绘制下一条时上一条正在 DMA，绘制时间被完全藏起来，帧率只由 SPI 决定。

#include "app.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "anim";

// ───────── DotField 参数（按 240x320 等比缩放）─────────
#define DOT_R      1.7f      // dotRadius 1.5
#define STEP       16.0f     // dotSpacing 14 + 2*radius，缩到 16 刚好整除 240/320
#define LENS_R     92.0f     // cursorRadius 240 → 屏幕太小，缩到 92
#define BULGE      0.60f     // bulgeStrength，原版会再乘 1.2
#define GLOW_R     86.0f     // glowRadius 160
#define GLOW_I     0.95f     // glowIntensity 0.8
#define BASE_OP    0.55f     // baseOpacity
#define FOCUS_MS   1800.0f   // intro 时长
#define ORBIT_MS   9000.0f   // 镜头绕一圈的时间（原版跟鼠标走，这里自动巡游）

#define BAND_H     40
#define BAND_N     (LCD_H / BAND_H)
#define MAX_DOTS   512
#define LUT_N      64
#define WAYPTS     6

// 渐变：原版默认 #5227FF → #FF9FFC，135°
#define GRAD_ANGLE 135.0f

static volatile bool s_on;
static int64_t       s_t0;
static uint32_t      s_frames;
static uint32_t      s_log_frames;
static int64_t       s_log_ms;

static int      s_n;                       // 实际点数
static float    s_ox[MAX_DOTS];            // 点的原始位置
static float    s_oy[MAX_DOTS];
static uint8_t  s_r[MAX_DOTS];             // 点的基础色（渐变采样结果）
static uint8_t  s_g[MAX_DOTS];
static uint8_t  s_b[MAX_DOTS];
static uint32_t s_lut[LUT_N];              // OKLab 渐变 LUT（0xRRGGBB）

static float    s_wp[WAYPTS][2];           // 镜头路径（闭合 Catmull-Rom）

static uint16_t *s_band[2];                // 双条带缓冲

// ───────── sRGB / OKLab ─────────
static float srgb_to_lin(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float lin_to_srgb(float c)
{
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static void rgb_to_oklab(float r, float g, float b, float *L, float *A, float *B)
{
    float lr = srgb_to_lin(r), lg = srgb_to_lin(g), lb = srgb_to_lin(b);
    float l = cbrtf(0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb);
    float m = cbrtf(0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb);
    float s = cbrtf(0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb);
    *L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    *A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    *B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static void oklab_to_rgb(float L, float A, float B, float *r, float *g, float *b)
{
    float l = L + 0.3963377774f * A + 0.2158037573f * B;
    float m = L - 0.1055613458f * A - 0.0638541728f * B;
    float s = L - 0.0894841775f * A - 1.2914855480f * B;
    float l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    *r = lin_to_srgb(4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3);
    *g = lin_to_srgb(-1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3);
    *b = lin_to_srgb(-0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3);
}

static void build_lut(void)
{
    // #5227FF
    float L0, A0, B0, L1, A1, B1;
    rgb_to_oklab(0x52 / 255.0f, 0x27 / 255.0f, 1.0f, &L0, &A0, &B0);
    // #FF9FFC
    rgb_to_oklab(1.0f, 0x9F / 255.0f, 0xFC / 255.0f, &L1, &A1, &B1);

    float c0 = sqrtf(A0 * A0 + B0 * B0);
    float c1 = sqrtf(A1 * A1 + B1 * B1);

    for (int i = 0; i < LUT_N; i++) {
        float t = (float)i / (LUT_N - 1);
        // 原版的 blend()：色相方向按向量插值，饱和度按长度插值，避免中间发灰
        float abx = A0 + (A1 - A0) * t;
        float aby = B0 + (B1 - B0) * t;
        float len = sqrtf(abx * abx + aby * aby);
        float ch  = c0 + (c1 - c0) * t;
        if (len > 1e-5f) {
            float k = ch / len;
            abx *= k;
            aby *= k;
        }
        float L = L0 + (L1 - L0) * t;
        float r, g, b;
        oklab_to_rgb(L, abx, aby, &r, &g, &b);
        s_lut[i] = ((uint32_t)(r * 255.0f + 0.5f) << 16) |
                   ((uint32_t)(g * 255.0f + 0.5f) << 8) |
                    (uint32_t)(b * 255.0f + 0.5f);
    }
}

static void lut_sample(float t, int *r, int *g, int *b)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float f = t * (LUT_N - 1);
    int   i = (int)f;
    if (i > LUT_N - 2) i = LUT_N - 2;
    f -= (float)i;
    uint32_t c0 = s_lut[i], c1 = s_lut[i + 1];
    int r0 = (c0 >> 16) & 255, r1 = (c1 >> 16) & 255;
    int g0 = (c0 >> 8) & 255,  g1 = (c1 >> 8) & 255;
    int b0 = c0 & 255,         b1 = c1 & 255;
    *r = (int)(r0 + (r1 - r0) * f + 0.5f);
    *g = (int)(g0 + (g1 - g0) * f + 0.5f);
    *b = (int)(b0 + (b1 - b0) * f + 0.5f);
}

static void build_field(void)
{
    build_lut();

    float ang = GRAD_ANGLE * 3.14159265358979f / 180.0f;
    float dirx = sinf(ang), diry = -cosf(ang);      // 与 CSS linear-gradient 一致
    float cx = LCD_W * 0.5f, cy = LCD_H * 0.5f;
    float extent = fabsf(LCD_W * dirx) + fabsf(LCD_H * diry);

    // 网格从画面外一格开始，镜头把点推到边缘时不会露出空档
    int n = 0;
    for (float y = -STEP; y <= LCD_H + STEP && n < MAX_DOTS; y += STEP) {
        for (float x = -STEP; x <= LCD_W + STEP && n < MAX_DOTS; x += STEP) {
            s_ox[n] = x;
            s_oy[n] = y;
            float t = ((x - cx) * dirx + (y - cy) * diry) / extent + 0.5f;
            int r, g, b;
            lut_sample(t, &r, &g, &b);
            s_r[n] = (uint8_t)r;
            s_g[n] = (uint8_t)g;
            s_b[n] = (uint8_t)b;
            n++;
        }
    }
    s_n = n;

    // 镜头路径：闭合样条，黄金角分布，扫过中间也蹭到边缘
    for (int k = 0; k < WAYPTS; k++) {
        float a = (float)k * 2.39996323f;
        float rr = 0.18f + 0.24f * sqrtf(((float)k + 0.5f) / WAYPTS);
        s_wp[k][0] = cx + (float)LCD_W * rr * cosf(a);
        s_wp[k][1] = cy + (float)LCD_H * rr * sinf(a);
    }
}

static inline float cr(float p0, float p1, float p2, float p3, float f)
{
    float f2 = f * f, f3 = f2 * f;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * f +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f3);
}

// 把一条 240 x h 的横带画进 buf（黑底 + 若干抗锯齿圆点）
static void render_band(int y0, int h, uint16_t *buf, float lx, float ly, float focus)
{
    memset(buf, 0, (size_t)LCD_W * h * 2);       // RGB565 的黑在大端小端都是 0

    const float k     = BULGE * 1.2f;
    const float blur  = (1.0f - focus) * STEP * 0.42f;
    const float m     = 1.12f + (1.0f - 1.12f) * focus;   // 入场收拢
    const float cx    = LCD_W * 0.5f, cy = LCD_H * 0.5f;
    const int   yEnd  = y0 + h - 1;

    for (int i = 0; i < s_n; i++) {
        // 入场：整场从 1.12 倍缩回原位
        float sx = cx + (s_ox[i] - cx) * m;
        float sy = cy + (s_oy[i] - cy) * m;

        float rx = sx - lx, ry = sy - ly;
        float d2 = rx * rx + ry * ry;
        float dist = sqrtf(d2);
        float x = dist / LENS_R;

        float grow = 1.0f, spread = 1.0f, pool = 0.0f;
        float dx = sx, dy = sy;

        if (x < 1.0f) {
            // 原版核心：w = (1-x²)³ 作为权重，把点沿径向推开并放大
            float y  = 1.0f - x * x;
            float w  = y * y * y;
            grow   = 1.0f + k * w;
            spread = 1.0f + k * (w - 6.0f * x * x * y * y);
            dx = lx + rx * grow;
            dy = ly + ry * grow;

            float g = dist * grow / GLOW_R;       // 到镜头中心的距离 / glow 半径
            if (g < 1.0f) {
                float q = 1.0f - g * g;
                pool = q * q * GLOW_I;
                if (pool > 1.0f) pool = 1.0f;
            }
        }

        float scale = sqrtf(grow * spread);
        if (scale < 0.14f) scale = 0.14f;

        // 原版在局部空间里画圆，再由顶点着色器乘 scale 投到屏幕，
        // 所以屏幕上的半径是 (uRadius + blur) * scale
        float rr     = (DOT_R + blur) * scale;
        float vrad   = rr;
        float edge   = (1.0f + blur * 0.7f) * scale;
        float energy = powf(DOT_R / (DOT_R + blur), 1.15f);

        float alpha = (BASE_OP + (1.0f - BASE_OP) * pool) * energy;
        if (alpha <= 0.004f) continue;

        // 颜色：基础渐变 → glow 白
        float mixw = pool * 0.8f;
        int   R = (int)(s_r[i] + (255.0f - s_r[i]) * mixw);
        int   G = (int)(s_g[i] + (255.0f - s_g[i]) * mixw);
        int   B = (int)(s_b[i] + (255.0f - s_b[i]) * mixw);
        int   A = (int)(alpha * 255.0f + 0.5f);
        if (A <= 1) continue;

        float ext = vrad + edge * 0.5f + 1.0f;
        int px0 = (int)floorf(dx - ext), px1 = (int)ceilf(dx + ext);
        int py0 = (int)floorf(dy - ext), py1 = (int)ceilf(dy + ext);
        if (px0 < 0) px0 = 0;
        if (px1 > LCD_W - 1) px1 = LCD_W - 1;
        if (py0 < y0) py0 = y0;
        if (py1 > yEnd) py1 = yEnd;
        if (px0 > px1 || py0 > py1) continue;

        // 三段处理：内圈整覆盖（不算 sqrt）、外圈直接丢、只有边缘带做平滑
        float r_in  = vrad - edge * 0.5f;
        float r_out = vrad + edge * 0.5f;
        float in2 = r_in > 0.0f ? r_in * r_in : 0.0f;
        float out2 = r_out * r_out;
        float inv_edge = 1.0f / edge;

        for (int py = py0; py <= py1; py++) {
            float fy = (float)py + 0.5f - dy;
            float fy2 = fy * fy;
            uint16_t *row = buf + (size_t)(py - y0) * LCD_W;
            for (int px = px0; px <= px1; px++) {
                float fx = (float)px + 0.5f - dx;
                float dd = fx * fx + fy2;
                if (dd > out2) continue;

                int aa;
                if (dd <= in2) {
                    aa = A;
                } else {
                    float d = sqrtf(dd);
                    float u = (d - vrad) * inv_edge + 0.5f;
                    if (u >= 1.0f) continue;
                    if (u < 0.0f) u = 0.0f;
                    float cov = 1.0f - u * u * (3.0f - 2.0f * u);
                    aa = (int)(A * cov + 0.5f);
                    if (aa <= 0) continue;
                }
                // 黑底合成 = 颜色乘 alpha
                int r8 = (R * aa) >> 8, g8 = (G * aa) >> 8, b8 = (B * aa) >> 8;
                uint16_t c = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
                row[px] = (uint16_t)((c >> 8) | (c << 8));
            }
        }
    }
}

static void anim_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        if (!s_on) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wake = xTaskGetTickCount();
            continue;
        }

        float t_ms = (float)((esp_timer_get_time() - s_t0) / 1000.0);

        // intro：0 → 1，缓动用三次曲线，和原版一致
        float focus = t_ms / FOCUS_MS;
        if (focus > 1.0f) focus = 1.0f;
        float eased = focus < 0.5f ? 4.0f * focus * focus * focus
                                   : 1.0f - powf(-2.0f * focus + 2.0f, 3.0f) / 2.0f;

        // 镜头位置：闭合 Catmull-Rom
        float u = fmodf(t_ms / ORBIT_MS * WAYPTS, (float)WAYPTS);
        int   i = (int)u;
        float f = u - (float)i;
        int i0 = (i + WAYPTS - 1) % WAYPTS, i1 = i % WAYPTS;
        int i2 = (i + 1) % WAYPTS,         i3 = (i + 2) % WAYPTS;
        float lx = cr(s_wp[i0][0], s_wp[i1][0], s_wp[i2][0], s_wp[i3][0], f);
        float ly = cr(s_wp[i0][1], s_wp[i1][1], s_wp[i2][1], s_wp[i3][1], f);

        display_lock();
        // 流水线：画下一条带时，上一条正在 DMA
        render_band(0, BAND_H, s_band[0], lx, ly, eased);
        display_push_band(0, BAND_H, s_band[0]);
        for (int b = 1; b < BAND_N; b++) {
            uint16_t *buf = s_band[b & 1];
            render_band(b * BAND_H, BAND_H, buf, lx, ly, eased);
            display_wait_flush();
            display_push_band(b * BAND_H, BAND_H, buf);
        }
        display_wait_flush();
        display_unlock();

        s_frames++;

        if (s_frames - s_log_frames >= 90) {
            int64_t now_ms = (esp_timer_get_time() - s_t0) / 1000;
            int64_t dt = now_ms - s_log_ms;
            if (dt > 0) {
                ESP_LOGI(TAG, "fps %.1f  (%u frames, %.1f ms/frame)",
                         (s_frames - s_log_frames) * 1000.0 / dt,
                         (unsigned)(s_frames - s_log_frames),
                         (float)dt / (s_frames - s_log_frames));
            }
            s_log_frames = s_frames;
            s_log_ms = now_ms;
        }

        vTaskDelayUntil(&wake, 1);   // 一帧装不进 10 ms，1 tick 是最小节拍
    }
}

void anim_init(void)
{
    build_field();

    for (int i = 0; i < 2; i++) {
        s_band[i] = heap_caps_malloc((size_t)LCD_W * BAND_H * 2,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_band[i]) {
            // 内部 RAM 不够就退到 PSRAM（慢一点，但不至于起不来）
            s_band[i] = heap_caps_malloc((size_t)LCD_W * BAND_H * 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!s_band[i]) {
            ESP_LOGE(TAG, "band buffer alloc failed");
            abort();
        }
    }

    xTaskCreate(anim_task, "anim", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "dotfield: %d dots, step %.0f, r %.1f, lens %.0f, glow %.0f, grad 135deg",
             s_n, STEP, DOT_R, LENS_R, GLOW_R);
}

void anim_start(void)
{
    display_lock();
    if (!s_on) {
        s_t0 = esp_timer_get_time();
        s_frames = 0;
        s_log_frames = 0;
        s_log_ms = 0;
        s_on = true;
        ESP_LOGI(TAG, "dotfield on");
    }
    display_unlock();
}

void anim_stop(void)
{
    display_lock();
    bool was = s_on;
    s_on = false;
    display_unlock();

    if (!was) return;

    int64_t ms = (esp_timer_get_time() - s_t0) / 1000;
    if (ms > 0) {
        ESP_LOGI(TAG, "dotfield off: %u frames in %d ms = %.1f fps",
                 (unsigned)s_frames, (int)ms, s_frames * 1000.0 / ms);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    display_wait_flush();
    ui_redraw();
}

bool anim_active(void)
{
    return s_on;
}
