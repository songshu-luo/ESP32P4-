/*
 * 休眠壁纸（屏保）层
 *
 * 挂在 lv_layer_top() 上的全屏图片：息屏时覆盖在页面与状态栏之上，
 * 唤醒 / 切常亮时撤掉。模块不参与任何页面状态。
 *
 * 触摸语义：休眠期间该层是「事件吞噬层」——只吃掉触摸并立刻唤醒
 * （内部调用 app_settings_wake_now），不把事件透传给下层页面，
 * 避免休眠中被误点滑块或切换页签。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在 LVGL 锁区内、页面建好之后调用一次，创建默认隐藏的壁纸层 */
void ui_screensaver_init(void);

/* 显示休眠壁纸（息屏变暗分支调用） */
void ui_screensaver_show(void);

/* 切换壁纸图源；dsc 为 NULL 时回落内置 sleep_wall。必须在 LVGL 锁内调用。 */
void ui_screensaver_set_src(const lv_image_dsc_t *dsc);

/* 带 300 ms 交叉淡入切换壁纸；动画进行中再次调用不会叠加第二段动画。
 * dsc 为 NULL 或未初始化时回落硬切到底图。必须在 LVGL 锁内调用。 */
void ui_screensaver_transition_to(const lv_image_dsc_t *dsc);

/* 隐藏休眠壁纸（唤醒 / 切常亮分支调用） */
void ui_screensaver_hide(void);

/* 当前壁纸是否处于可见状态 */
bool ui_screensaver_is_visible(void);

#ifdef __cplusplus
}
#endif
