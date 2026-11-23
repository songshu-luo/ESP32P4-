/*
 * SD 卡休眠壁纸模块
 *
 * 从 /sdcard/wall/wNN.jpg 读取 1024x600 baseline JPEG，用 ESP32-P4 的
 * 硬件 JPEG 解码器解成 RGB565，后台双缓冲预解码，息屏时随机换一张。
 *
 * 约束：
 *   - 本模块不碰任何 LVGL 对象，只在任务里做文件 IO / 硬件解码；
 *   - wall_sd_take_ready() 可以在 LVGL 任务里调用，内部不加 LVGL 锁；
 *   - 卡不在位时不阻塞调用者，后台任务按 >=5 s 的间隔重试挂载。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化模块（创建锁 / 任务，尝试挂载 + 扫描，并同步解码第一张）
 *
 * 必须在 LVGL 初始化完成之后调用；本函数不碰 LVGL 对象。
 * 挂载和解码可能耗时几百毫秒，所以 main.c 把它放在 bsp_display_unlock() 之后。
 */
void wall_sd_init(void);

/**
 * @brief 请求后台预解码“下一张”到 back buffer（异步、幂等、可高频调用）
 *
 * 绝不阻塞调用者；卡未挂载时由任务限频重试挂载。
 */
void wall_sd_prime(void);

/**
 * @brief 取出已就绪的图描述符；没有就绪图返回 NULL
 *
 * 取出后描述符归调用方使用，内部把 ready 清零并交换 front/back。
 * 不碰 LVGL；返回值只在下次调用本函数前保持有效（缓冲常驻，不会释放）。
 */
const lv_image_dsc_t *wall_sd_take_ready(void);

/** @brief 当前是否有可用图源（卡已挂载且目录列表非空） */
bool wall_sd_available(void);

#ifdef __cplusplus
}
#endif
