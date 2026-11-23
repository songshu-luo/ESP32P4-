/*
 * 休眠壁纸（屏保）层实现
 *
 * 图层挂在 lv_layer_top()，位于所有页面和状态栏之上；默认隐藏。
 * 交互约定：休眠时这一层是「事件吞噬层」——可点击，但不做任何 UI 动作，
 * 只在 LV_EVENT_PRESSED 里调用 app_settings_wake_now() 立即唤醒。
 * 这样触摸不会穿透到底层页面（避免休眠中被误点滑块或切换页签），
 * 唤醒也不依赖 500 ms 的 inactive 轮询，按下即恢复。
 * 其它约束：只操作自己这一个对象，不清屏、不重建页面、不动 status bar；
 * 图片 1024x600 与屏幕同分辨率，左上角对齐铺满，不变形。
 */

#include "ui_screensaver.h"

#include "lvgl.h"

#include "app_settings.h"
#include "assets/sleep_wall.h"

#define SCREEN_W   1024
#define SCREEN_H   600

static lv_obj_t *s_wall    = NULL;
static bool      s_visible = false;

/* 按下即唤醒：壁纸层吞掉事件，不向页面传递 */
static void wall_pressed_cb(lv_event_t *e)
{
    (void)e;
    app_settings_wake_now();
}

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

    /* 关键：可点击但只负责唤醒。LVGL 命中测试只把事件交给最上层可点击
     * 对象，因此下层页面收不到触摸；本层不响应任何控件动作，也不滚动。 */
    lv_obj_set_clickable(s_wall, true);
    lv_obj_set_scrollable(s_wall, false);
    lv_obj_add_event_cb(s_wall, wall_pressed_cb, LV_EVENT_PRESSED, NULL);

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

void ui_screensaver_set_src(const lv_image_dsc_t *dsc)
{
    if (s_wall == NULL) {
        return;
    }
    lv_image_set_src(s_wall, dsc ? dsc : &sleep_wall);
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
