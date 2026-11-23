/*
 * 休眠壁纸（屏保）层
 *
 * 挂在 lv_layer_top() 上的全屏图片：息屏时覆盖在页面与状态栏之上，
 * 唤醒 / 切常亮时撤掉。模块不参与任何页面状态，也不拦截触摸。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 在 LVGL 锁区内、页面建好之后调用一次，创建默认隐藏的壁纸层 */
void ui_screensaver_init(void);

/* 显示休眠壁纸（息屏变暗分支调用） */
void ui_screensaver_show(void);

/* 隐藏休眠壁纸（唤醒 / 切常亮分支调用） */
void ui_screensaver_hide(void);

/* 当前壁纸是否处于可见状态 */
bool ui_screensaver_is_visible(void);

#ifdef __cplusplus
}
#endif
