/*
 * 主界面背景图（/sdcard/bg.jpg）
 *
 * 只在启动时解码一次，之后像素缓冲常驻 PSRAM，由 UI 层的全屏 lv_image
 * 直接引用，不再复制。没有卡 / 没有图 / 解码失败都只是「没有背景」，
 * 界面回落到纯色底，绝不 abort。
 */
#pragma once

#include "lvgl.h"

/**
 * 读取并解码 /sdcard/bg.jpg，填充常驻的 lv_image_dsc_t。
 *
 * 必须由已经初始化过 SD 卡之后的任务调用（main.c 里在 wall_sd_init() 之后），
 * 本函数不会自己挂载 SD，避免和 wall_sd 抢 BSP 的挂载点 / LDO。
 * 可重复调用：已经解码成功时直接返回。
 */
void background_init(void);

/** 已解码的背景图描述符；没有背景时返回 NULL。 */
const lv_image_dsc_t *background_dsc(void);