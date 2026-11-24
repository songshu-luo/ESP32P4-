/*
 * S0 基线自检界面：点屏 / 触摸 / 背光 三项独立可见验证
 */
#pragma once

#include "lvgl.h"

/** 在指定屏幕上构建自检界面。调用前必须已持有 LVGL 锁。 */
void app_ui_build(lv_display_t *disp);

/**
 * 背景图解码完成（background_init）之后调用一次：把图挂到 app_ui_build()
 * 已建好的全屏背景层上。SD 挂载在 UI 建好之后才完成，所以需要这一次补挂。
 * 没有背景图时什么也不做（保持纯色底）。调用前必须已持有 LVGL 锁。
 */
void app_ui_background_refresh(void);
