/*
 * 休眠壁纸资源
 *
 * 数据在 assets/sleep_wall.c（1024x600 RGB565，由图像脚本生成，请勿手改）。
 * 声明单独放这里，避免页面/设置模块直接包含 4.9 MB 的数组定义。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t sleep_wall;

#ifdef __cplusplus
}
#endif
