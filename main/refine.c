// Refine Frame 的固件版（源自 React Bits 的 Refine Frame 微交互）
//
// 原版是一个「带状态机的媒体画框」：status 在 queued / generating / refining /
// complete / error 之间切换，干活时一条软光带横穿画框，左下角浮一个状态胶囊。
// 图片出来之后再从低分辨率逐级「精修」到全清晰。
//
// 这里先实现「等待」这一段（也就是图片还没下来的时候），它其实是原版里最省的一块：
//   原版 CSS 里 data-sweep 只在 :not([data-mosaic]) 时启用，说明扫光带只在
//   没有图像内容时出现。此时画框是纯色纸面 + 一条软带，不需要任何图片数据。
//
// 实现要点：
//   - 纸面 #27272a，圆角 10（原版 16px@320 宽，等比缩到 240 宽约 12）
//   - 扫光带用两个 60px 宽的对象拼出来：左半 暗→亮、右半 亮→暗，
//     LVGL 8 的渐变只支持两端颜色，拼两段正好得到「软带」的亮度剖面。
//     原版是 115° 斜带，这里做成竖直带横扫 —— 240px 宽的屏上看不出差别，
//     但如果要更还原，可以把带子做成图片再旋转。
//   - 渐变用的是不透明色 #27272a → #45454a，等价于「14% 白叠加在纸面上」。
//     这样就不需要 alpha 通道，绘制更快，而且此时底下本来就是纯纸面。
//   - 状态胶囊：圆角胶囊 + lv_spinner + 16px 中文

#include "app.h"

#include <string.h>

#include "lvgl.h"

extern const lv_font_t font_cn_16;

#define RF_BG          0x27272A     // 纸面
#define RF_INK         0xF5F5F5     // 文字 / 扫光
#define RF_PEAK        0x4A4A50     // 扫光带峰值（≈ 纸面上叠 14% 白）
#define RF_RED         0xEF4444     // 失败
#define RF_SWEEP_W     120          // 软带总宽
#define RF_SWEEP_MS    2200
#define RF_RADIUS      10
#define RF_CHIP_H      30

static lv_obj_t *s_frame;
static lv_obj_t *s_sweep;
static lv_obj_t *s_chip;
static lv_obj_t *s_spin;
static lv_obj_t *s_label;

// 只用现有 16px 字库里已有的字，避免重新生成字模
static const char *LABELS[5] = {
    "等待",      // QUEUED
    "生成中",    // GENERATING
    "下载中",    // REFINING（下载 + 解码）
    "完成",      // COMPLETE
    "失败",      // ERROR
};

static void sweep_exec(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v);
}

static void sweep_run(bool on)
{
    lv_anim_del(s_sweep, NULL);
    if (!on) {
        lv_obj_add_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sweep);
    lv_anim_set_exec_cb(&a, sweep_exec);
    lv_anim_set_values(&a, -RF_SWEEP_W, LCD_W);
    lv_anim_set_time(&a, RF_SWEEP_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

static lv_obj_t *make_plain(lv_obj_t *parent, int w, int h, int x, int y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void rf_init(void)
{
    display_lock();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(RF_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ── 画框（纸面）──
    s_frame = make_plain(scr, LCD_W, LCD_H, 0, 0);
    lv_obj_set_style_bg_color(s_frame, lv_color_hex(RF_BG), 0);
    lv_obj_set_style_bg_opa(s_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_frame, RF_RADIUS, 0);
    lv_obj_set_style_clip_corner(s_frame, true, 0);

    // ── 扫光带：两半拼出软剖面 ──
    s_sweep = make_plain(s_frame, RF_SWEEP_W, LCD_H, -RF_SWEEP_W, 0);

    lv_obj_t *l = make_plain(s_sweep, RF_SWEEP_W / 2, LCD_H, 0, 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(RF_BG), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(l, lv_color_hex(RF_PEAK), 0);
    lv_obj_set_style_bg_grad_dir(l, LV_GRAD_DIR_HORZ, 0);

    lv_obj_t *r = make_plain(s_sweep, RF_SWEEP_W / 2, LCD_H, RF_SWEEP_W / 2, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(RF_PEAK), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(r, lv_color_hex(RF_BG), 0);
    lv_obj_set_style_bg_grad_dir(r, LV_GRAD_DIR_HORZ, 0);

    // ── 状态胶囊：左下角 ──
    s_chip = lv_obj_create(s_frame);
    lv_obj_remove_style_all(s_chip);
    lv_obj_set_size(s_chip, LV_SIZE_CONTENT, RF_CHIP_H);
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(RF_BG), 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_chip, RF_CHIP_H / 2, 0);
    lv_obj_set_style_pad_hor(s_chip, 10, 0);
    lv_obj_set_style_pad_ver(s_chip, 0, 0);
    lv_obj_set_style_pad_column(s_chip, 6, 0);
    lv_obj_set_layout(s_chip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_chip, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_clear_flag(s_chip, LV_OBJ_FLAG_SCROLLABLE);

    s_spin = lv_spinner_create(s_chip, 1100, 60);
    lv_obj_set_size(s_spin, 15, 15);
    lv_obj_set_style_arc_width(s_spin, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(RF_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(RF_INK), LV_PART_INDICATOR);

    s_label = lv_label_create(s_chip);
    lv_obj_set_style_text_font(s_label, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(RF_INK), 0);
    lv_label_set_text(s_label, LABELS[RF_QUEUED]);

    display_unlock();

    rf_set_status(RF_QUEUED);
}

void rf_set_status(rf_status_t st)
{
    if (st < RF_QUEUED || st > RF_ERROR) return;

    display_lock();

    lv_label_set_text(s_label, LABELS[st]);

    if (st == RF_ERROR) {
        lv_obj_set_style_text_color(s_label, lv_color_hex(RF_RED), 0);
    } else {
        lv_obj_set_style_text_color(s_label, lv_color_hex(RF_INK), 0);
    }

    bool active = (st == RF_QUEUED || st == RF_GENERATING || st == RF_REFINING);
    if (active) {
        lv_obj_clear_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
    }

    // 扫光带只在干活时巡游，完成/失败就收起来
    sweep_run(active);

    display_unlock();
}
