/*
 * 休眠壁纸（屏保）层实现
 *
 * 图层挂在 lv_layer_top()，位于所有页面和状态栏之上；默认隐藏。
 * 交互约定：休眠时这一层是「事件吞噬层」——可点击，但不做任何 UI 动作，
 * 只在 LV_EVENT_PRESSED 里调用 app_settings_wake_now() 立即唤醒。
 * 这样触摸不会穿透到底层页面（避免休眠中被误点滑块或切换页签），
 * 唤醒也不依赖 500 ms 的 inactive 轮询，按下即恢复。
 *
 * 图层结构（均挂在 s_root 之下，创建顺序即 z 序，时钟永远最上）：
 *   s_root  全屏透明容器，唯一可点击，PRESSED 触发唤醒，吞掉所有触摸
 *   s_wall  底图 lv_image，显示当前壁纸
 *   s_wall2 overlay lv_image，交叉淡入时承载新图，默认隐藏
 *   s_clock 时钟 label，透明不可点击
 *   s_btn_next 右上角「切换壁纸」按钮，唯一可点击的子对象：接管命中测试，
 *           点了只切下一张壁纸、不唤醒（还额外 stop_bubbling 兜底）
 *
 * 轮播切换走 300 ms 交叉淡入：新图先挂到 s_wall2 从全透明淡到不透明，
 * 动画结束再把新图落到 s_wall、收起 overlay。淡入期间不预解码下一张，
 * 避免后台覆写仍在显示的旧缓冲造成撕裂。
 * 图片 1024x600 与屏幕同分辨率，左上角对齐铺满，不变形。
 */

#include "ui_screensaver.h"

#include <stdio.h>
#include <time.h>

#include "lvgl.h"

#include "app_settings.h"
#include "wall_sd.h"
#include "fonts/cjk_fonts.h"

#define SCREEN_W   1024
#define SCREEN_H   600

#define CLOCK_TOP_Y        88                    /* 状态栏 56 之下，视觉中间偏上 */
#define CLOCK_VALID_EPOCH  ((time_t)1700000000)  /* 早于 2023-11 视为未对时 */
#define APP_UI_FADE_MS     300                   /* 壁纸交叉淡入时长（毫秒） */

static lv_obj_t  *s_root       = NULL;
static lv_obj_t  *s_wall       = NULL;
static lv_obj_t  *s_wall2      = NULL;
static lv_obj_t  *s_clock      = NULL;
static lv_obj_t  *s_btn_next   = NULL;   /* 右上角「切换壁纸」按钮 */
static lv_obj_t  *s_hint       = NULL;   /* 没有任何壁纸时的提示文字 */
static lv_timer_t *s_clock_tmr = NULL;
static int        s_last_min   = -1;   /* 上次渲染的分钟；-1 = 尚未有效对时 */

static bool       s_visible    = false;
static bool       s_has_wall   = false;                 /* 底图当前是否真的有图 */
static bool       s_fading     = false;                 /* overlay 淡入动画进行中 */
static const lv_image_dsc_t *s_pending_dsc = NULL;      /* 待落到 s_wall 的新图 */

/* 只在「底图真的没有图」时显示提示；有图（哪怕卡已拔出）就不打扰。
 * 内置壁纸已移除，没有 SD 图时 s_wall 无源，只露出黑底 + 这行提示。 */
static void update_fallback_hint(void)
{
    if (s_hint == NULL) {
        return;
    }
    lv_obj_set_hidden(s_hint, s_has_wall);
}

/* 按下即唤醒：root 容器吞掉事件，不向页面传递 */
static void root_pressed_cb(lv_event_t *e)
{
    (void)e;
    app_settings_wake_now();
}

/* 右上角按钮：手动切下一张壁纸，且绝不能唤醒。
 * 两道保险：
 *   1) 按钮是 clickable 子对象，命中测试由它自己接管，父对象 s_root
 *      收不到 PRESSED，因此不会走到 root_pressed_cb；
 *   2) 这里再 stop_bubbling，事件不会冒泡给 s_root（双保险）。
 * 本回调里不得调用 app_settings_wake_now() / ui_screensaver_hide()。 */
static void next_wall_cb(lv_event_t *e)
{
    lv_event_stop_bubbling(e);

    /* 同一次触摸会先来 PRESSED、松手再来 CLICKED。这里只在 PRESSED 那一刻切图：
     * 若 CLICKED 再切一次，第二次必然「没有就绪图」而调用 wall_sd_prime()，
     * 于是下一张会在 300 ms 淡入进行中被解到「仍在屏上显示的旧缓冲」里，
     * 与 wall_sd.c / 本文件淡入期间不预解码的约定冲突，会造成画面撕裂。
     * 两条路径都不唤醒，CLICKED 只负责把事件截住。 */
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        return;
    }

    app_settings_sleep_advance();
}

/* LVGL 9 的 anim exec_cb 签名是 (void *, int32_t)，与 lv_obj_set_style_opa
 * 的 (lv_obj_t *, lv_opa_t, selector) 不匹配，这里用包装函数做类型转换。 */
static void fade_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* 淡入结束：把新图落到底图，收起 overlay，再预解码下一张。 */
static void fade_done_cb(lv_anim_t *anim)
{
    (void)anim;

    if (s_wall != NULL && s_pending_dsc != NULL) {
        lv_image_set_src(s_wall, s_pending_dsc);
        s_has_wall = true;
        update_fallback_hint();
    }
    if (s_wall2 != NULL) {
        lv_obj_set_style_opa(s_wall2, LV_OPA_COVER, 0);
        lv_obj_set_hidden(s_wall2, true);
    }
    s_pending_dsc = NULL;
    s_fading = false;

    /* 此刻旧缓冲已不在屏上显示，预解码下一张才不会撕裂当前画面 */
    wall_sd_prime();
}

/* 只在分钟变化（或 force）时真正 set_text，避免每秒一次无谓的重排与重绘。
 * 未对时（SNTP 未同步，time() 从 1970 起算）显示占位 "--:--"。 */
static void clock_refresh(bool force)
{
    if (s_clock == NULL) {
        return;
    }

    const time_t now = time(NULL);
    if (now < CLOCK_VALID_EPOCH) {
        if (force || s_last_min != -1) {
            lv_label_set_text(s_clock, "--:--");
            lv_obj_set_style_text_color(s_clock, lv_color_hex(0x8A8F98), 0);
            s_last_min = -1;
        }
        return;
    }

    struct tm t;
    localtime_r(&now, &t);          /* 时区已由 main.c 的 setenv(TZ)/tzset() 定好 */
    if (!force && t.tm_min == s_last_min) {
        return;                     /* 一分钟只重绘一次 */
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(s_clock, buf);
    lv_obj_set_style_text_color(s_clock, lv_color_white(), 0);
    s_last_min = t.tm_min;
}

static void clock_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    if (!s_visible) {
        return;                     /* 亮屏时不刷新，省一点 */
    }
    clock_refresh(false);
}

void ui_screensaver_init(void)
{
    lv_obj_t *layer = lv_layer_top();

    /* 根容器：透明、不可滚动，唯一负责吞触摸并唤醒 */
    s_root = lv_obj_create(layer);
    lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_shadow_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_clickable(s_root, true);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_add_event_cb(s_root, root_pressed_cb, LV_EVENT_PRESSED, NULL);

    /* 底图：不再内置壁纸，没有 SD 图时无源，露出下面的纯黑背景 */
    s_wall = lv_image_create(s_root);
    lv_image_set_src(s_wall, NULL);
    lv_obj_set_size(s_wall, SCREEN_W, SCREEN_H);
    lv_obj_align(s_wall, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 兜底纯黑背景：RGB565 图片本身不透明，背景只在无图/边界对不齐时露出 */
    lv_obj_set_style_bg_color(s_wall, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wall, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_clickable(s_wall, false);
    lv_obj_set_scrollable(s_wall, false);

    /* overlay：只引用 wall_sd 已解码好的像素缓冲，不额外分配 / 复制帧；
     * 默认隐藏，样式不透明度为全不透明，仅在淡入动画期间被改到 0。 */
    s_wall2 = lv_image_create(s_root);
    lv_obj_set_size(s_wall2, SCREEN_W, SCREEN_H);
    lv_obj_align(s_wall2, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_opa(s_wall2, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_wall2, false);
    lv_obj_set_scrollable(s_wall2, false);
    lv_obj_set_hidden(s_wall2, true);

    /* 透明时钟：label 自身无背景、无边框，只有文字（数字之外全透明）。
     * 最后创建，保证 z 序永远在两张壁纸之上。 */
    s_clock = lv_label_create(s_root);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_clock, LV_OPA_90, 0);
    lv_obj_set_style_bg_opa(s_clock, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_clock, 0, 0);
    lv_obj_set_style_pad_all(s_clock, 0, 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, CLOCK_TOP_Y);
    /* 不可点击：触摸从 label 冒泡到父对象 s_root，由 s_root 的 PRESSED 唤醒 */
    lv_obj_set_clickable(s_clock, false);

    /* 右上角「切换壁纸」按钮：建在 s_clock 之后，保证 z 序在最上层。
     * 半透明黑底 + 白色刷新图标，不遮壁纸；跟随 s_root 一起显示/隐藏。 */
    s_btn_next = lv_button_create(s_root);
    lv_obj_set_size(s_btn_next, 56, 56);
    lv_obj_set_style_radius(s_btn_next, 28, 0);              /* 正圆 */
    lv_obj_set_style_bg_color(s_btn_next, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_btn_next, LV_OPA_30, 0);       /* 半透明，不遮壁纸 */
    lv_obj_set_style_border_width(s_btn_next, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_next, 0, 0);
    lv_obj_set_style_outline_width(s_btn_next, 0, 0);
    lv_obj_set_style_pad_all(s_btn_next, 0, 0);
    lv_obj_align(s_btn_next, LV_ALIGN_TOP_RIGHT, -20, 20);
    /* 视觉仍是 56×56 的圆，但点击热区向外扩 44 px（有效约 144×144）：
     * 手指点在圆外一圈也会命中按钮，而不是落到 s_root 唤醒屏保。
     * 只用 ext_click_area 扩热区，不改尺寸/圆角/图标字号。 */
    lv_obj_set_ext_click_area(s_btn_next, 44);

    lv_obj_t *btn_icon = lv_label_create(s_btn_next);
    lv_label_set_text(btn_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(btn_icon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(btn_icon, lv_color_white(), 0);
    lv_obj_set_style_text_opa(btn_icon, LV_OPA_90, 0);
    lv_obj_center(btn_icon);
    lv_obj_set_clickable(btn_icon, false);                   /* 点击交给按钮 */

    /* PRESSED + CLICKED 都挂：按下瞬间就切图，两条路径都不唤醒 */
    lv_obj_add_event_cb(s_btn_next, next_wall_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn_next, next_wall_cb, LV_EVENT_CLICKED, NULL);

    /* 无壁纸提示：只有「一张图都拿不到」时显示，居中一行灰字。
     * 内置壁纸已删除，所以没插卡 / 卡里没图时就是这块黑底 + 这行字。 */
    s_hint = lv_label_create(s_root);
    lv_label_set_text(s_hint, "请插入 SD 设备");
    lv_obj_set_style_text_font(s_hint, &lv_font_cjk_20, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xC8CCD2), 0);
    lv_obj_set_style_text_opa(s_hint, LV_OPA_80, 0);
    lv_obj_set_style_bg_opa(s_hint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_hint, 0, 0);
    lv_obj_set_style_pad_all(s_hint, 0, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_clickable(s_hint, false);   /* 触摸照样冒泡到 s_root 唤醒 */
    lv_obj_set_hidden(s_hint, true);

    s_clock_tmr = lv_timer_create(clock_timer_cb, 1000, NULL);

    /* 默认隐藏 */
    lv_obj_set_hidden(s_root, true);

    s_visible  = false;
    s_has_wall = false;
    s_fading   = false;
    s_pending_dsc = NULL;
    update_fallback_hint();   /* 初始无图 → 提示显示（root 隐藏时看不见） */
}

void ui_screensaver_show(void)
{
    if (s_root == NULL) {
        return;
    }

    /* 兜底复位：上次隐藏时若 overlay 没收干净，这里一并清掉，避免残留半透明。 */
    if (s_wall2 != NULL) {
        lv_anim_delete(s_wall2, NULL);
        lv_obj_set_style_opa(s_wall2, LV_OPA_COVER, 0);
        lv_obj_set_hidden(s_wall2, true);
    }
    s_pending_dsc = NULL;
    s_fading = false;

    lv_obj_set_hidden(s_root, false);
    /* 置顶只调整兄弟节点间的 z 序，不触碰 clickable / scrollable */
    lv_obj_move_foreground(s_root);
    s_visible = true;
    /* 强制刷一次，避免显示出上一次残留的旧分钟 */
    clock_refresh(true);
    update_fallback_hint();   /* 没图时把提示顶上来 */
}

void ui_screensaver_set_src(const lv_image_dsc_t *dsc)
{
    if (s_wall == NULL) {
        return;
    }
    /* dsc 为 NULL：清空图源，露出纯黑底 + 「请插入 SD 卡」提示 */
    lv_image_set_src(s_wall, dsc);
    s_has_wall = (dsc != NULL);
    update_fallback_hint();
}

void ui_screensaver_transition_to(const lv_image_dsc_t *dsc)
{
    /* 空图源 / 未初始化 / 动画进行中：都不叠加第二段动画，直接落到底图。 */
    if (dsc == NULL || s_root == NULL || s_wall2 == NULL || s_fading) {
        ui_screensaver_set_src(dsc);
        return;
    }

    s_pending_dsc = dsc;
    lv_image_set_src(s_wall2, dsc);
    lv_obj_set_hidden(s_wall2, false);
    lv_obj_set_style_opa(s_wall2, LV_OPA_TRANSP, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_wall2);
    lv_anim_set_exec_cb(&a, fade_opa_cb);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_time(&a, APP_UI_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, fade_done_cb);
    lv_anim_start(&a);

    s_fading = true;
}

void ui_screensaver_hide(void)
{
    if (s_root == NULL) {
        return;
    }

    /* 先停掉尚未完成的 overlay 动画，避免回调在隐藏后再改样式。 */
    if (s_wall2 != NULL) {
        lv_anim_delete(s_wall2, NULL);
    }

    if (s_fading) {
        /* 动画被打断：把还没落地的 pending 先落到底图（pending 为空则跳过），
         * 否则下次显示会看到 overlay 上残留的半透明新图。 */
        if (s_pending_dsc != NULL && s_wall != NULL) {
            lv_image_set_src(s_wall, s_pending_dsc);
        }
    }

    if (s_wall2 != NULL) {
        lv_obj_set_style_opa(s_wall2, LV_OPA_COVER, 0);
        lv_obj_set_hidden(s_wall2, true);
    }
    s_pending_dsc = NULL;
    s_fading = false;

    lv_obj_set_hidden(s_root, true);
    s_visible = false;
}

bool ui_screensaver_is_visible(void)
{
    return s_visible;
}
