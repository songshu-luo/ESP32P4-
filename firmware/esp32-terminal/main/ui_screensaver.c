/*
 * 休眠壁纸（屏保）层实现
 *
 * 图层挂在 lv_layer_top()，位于所有页面和状态栏之上；默认隐藏。
 * 约束：
 *   - 只创建并操作自己这一个对象，不清屏、不重建页面、不动 status bar；
 *   - 不可点击 / 不可滚动：休眠后触摸事件必须穿透到底层页面，
 *     由 app_settings 的 inactive 回落检测完成唤醒；show() 置顶时也
 *     只调整 z 序，不改动可交互性；
 *   - 图片 1024x600 与屏幕同分辨率，左上角对齐铺满，不变形。
 */

#include "ui_screensaver.h"

#include "lvgl.h"

#include "assets/sleep_wall.h"

#define SCREEN_W   1024
#define SCREEN_H   600

static lv_obj_t *s_wall    = NULL;
static bool      s_visible = false;

void ui_screensaver_init(void)
{
    lv_obj_t *layer = lv_layer_top();

    s_wall = lv_image_create(layer);
    lv_image_set_src(s_wall, &sleep_wall);
    lv_obj_set_size(s_wall, SCREEN_W, SCREEN_H);
    lv_obj_align(s_wall, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 兜底纯黑背景：RGB565 图片本身不透明，背景只在边界对不齐时露出 */
    lv_obj_set_style_bg_color(s_wall, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wall, LV_OPA_COVER, LV_PART_MAIN);

    /* 关键：不拦截触摸。显式关掉可点击 / 可滚动；
     * 后续 show() 置顶只改 z 序，不恢复可交互性。 */
    lv_obj_set_clickable(s_wall, false);
    lv_obj_set_scrollable(s_wall, false);

    /* 默认隐藏 */
    lv_obj_set_hidden(s_wall, true);

    s_visible = false;
}

void ui_screensaver_show(void)
{
    if (s_wall == NULL) {
        return;
    }
    lv_obj_set_hidden(s_wall, false);
    /* 置顶只调整兄弟节点间的 z 序，不触碰 clickable / scrollable */
    lv_obj_move_foreground(s_wall);
    s_visible = true;
}

void ui_screensaver_hide(void)
{
    if (s_wall == NULL) {
        return;
    }
    lv_obj_set_hidden(s_wall, true);
    s_visible = false;
}

bool ui_screensaver_is_visible(void)
{
    return s_visible;
}
