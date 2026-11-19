/*
 * 占位页（S2-d）
 *
 * 「控制」「设置」在 S2-d 只有页面骨架，没有功能。按交接文档 §9 的要求，
 * 必须明确写成「S3 开放」——**不做假按钮**，避免出现「点了没反应」的死控件，
 * 那会让人误以为设备卡死或者功能已存在。
 */

#include "app_ui_internal.h"

#include "fonts/cjk_fonts.h"

void ui_page_placeholder_build(lv_obj_t *page, const char *title, const char *note)
{
    /* 居中一块卡片，整页只有三行：标题 / 说明 / 开放阶段 */
    lv_obj_t *card = ui_card(page, 640, 260);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *t = ui_label(card, &lv_font_cjk_28, COLOR_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *n = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, note);
    lv_obj_set_width(n, 580);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 56);

    /* 阶段标签：明确告诉使用者「这里还没有东西」，而不是做成可点的按钮 */
    lv_obj_t *badge = ui_label(card, &lv_font_cjk_20, COLOR_WARN, "S3 开放");
    lv_obj_align(badge, LV_ALIGN_BOTTOM_MID, 0, -4);
}