/*
 * S0 基线自检界面：点屏 / 触摸 / 背光 三项独立可见验证
 */
#pragma once

#include "lvgl.h"

/** 在指定屏幕上构建自检界面。调用前必须已持有 LVGL 锁。 */
void app_ui_build(lv_display_t *disp);
