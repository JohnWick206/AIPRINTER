// 界面层：一屏只有三样东西
//   上：中文状态（24px 抗锯齿）
//   中：进度条（生成 25% / 下载 60% / 解码 85% / 完成 100%）
//   下：小字提示（提示词或剩余秒数，16px）
// 出图时三者全部隐藏，只留 60x80 的图片居中。

#include "app.h"

#include <string.h>

#include "lvgl.h"

extern const lv_font_t font_cn_24;
extern const lv_font_t font_cn_16;

static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_bar;
static lv_obj_t *s_img;
static lv_img_dsc_t s_dsc;

void ui_init(void)
{
    display_lock();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &font_cn_24, 0);
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_label_set_text(s_status, "启动");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 3);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 116, 6);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 9);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_make(38, 38, 44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_make(70, 160, 250), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);

    s_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hint, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_make(150, 150, 162), 0);
    lv_obj_set_width(s_hint, 156);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_hint, "");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    s_img = lv_img_create(scr);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

    display_unlock();
}

void ui_set_status(const char *cn, int progress)
{
    display_lock();

    lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    if (cn) lv_label_set_text(s_status, cn);

    if (progress < 0) {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar, progress, LV_ANIM_ON);
    }

    display_unlock();
}

void ui_set_hint(const char *text)
{
    display_lock();
    lv_label_set_text(s_hint, text ? text : "");
    display_unlock();
}

void ui_show_image(uint16_t *rgb565, int w, int h)
{
    display_lock();

    s_dsc.header.always_zero = 0;
    s_dsc.header.w = (uint32_t)w;
    s_dsc.header.h = (uint32_t)h;
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.data_size = (uint32_t)w * (uint32_t)h * 2;
    s_dsc.data = (const uint8_t *)rgb565;

    lv_img_set_src(s_img, &s_dsc);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    display_unlock();
}
