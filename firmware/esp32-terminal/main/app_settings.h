/*
 * 屏幕设置状态（亮度 / 息屏）
 *
 * 设计理由：设置页需要「改了立刻生效」的亮度，以及「长时间没人碰就变暗」的
 * 省电行为，但这两件事都不属于 UI。把状态放在这里，UI 只负责画控件，
 * 定时器只调用 app_settings_tick()，换页面或重建界面都不会丢设置。
 *
 * 依赖方向：本模块只依赖 BSP 背光接口和 LVGL 的「无操作时长」查询，
 * 不包含任何页面代码，也不碰 s_ctx。
 *
 * 亮度与息屏超时会被 UI 线程和（未来可能增加的）其它任务读取，
 * 所以内部统一用互斥量保护。app_settings_tick() 由 LVGL 定时器调用，
 * 取锁用 0 超时，宁可这一拍不处理，也不在 UI 回调里等待。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 默认亮度（%）：与 main.c 的初始背光一致 */
#define APP_SETTINGS_DEFAULT_BRIGHTNESS   (80)

/** 息屏时的背光（%）。不用 0：全黑会被当成死机，30% 配合休眠壁纸仍能看见提示 */
#define APP_SETTINGS_DIM_BRIGHTNESS       (30)

/* 休眠壁纸自动轮播间隔（毫秒）。0 = 关闭轮播，只显示进入息屏时那一张 */
#define APP_SETTINGS_SLEEP_ROTATE_MS      (180000)

/**
 * @brief 初始化设置模块（创建互斥量、恢复默认值）
 *
 * 必须在 app_ui_build() 之前调用，否则设置页读到的是未初始化状态。
 * 只改软件状态，不主动点亮背光——main.c 仍然按原顺序在界面建好后再开背光。
 */
esp_err_t app_settings_init(void);

/** 设置亮度，夹紧到 10..100，立即写背光（如果当前处于息屏状态则同时唤醒） */
void app_settings_set_brightness(int percent);

/** 当前亮度（%），未初始化时返回默认值 80 */
int app_settings_get_brightness(void);

/** 设置息屏超时（ms）；0 = 常亮。设为 0 时若已息屏会立即恢复亮度。 */
void app_settings_set_dim_timeout(uint32_t ms);

/** 当前息屏超时（ms） */
uint32_t app_settings_get_dim_timeout(void);

/** 当前是否处于息屏（低亮度）状态 */
bool app_settings_is_dimmed(void);

/**
 * @brief 立即从息屏状态唤醒（幂等；未息屏时什么都不做）
 *
 * 供休眠壁纸层在 LV_EVENT_PRESSED 里调用：壁纸层吞掉触摸后不会再让页面
 * 收到事件，但唤醒不能等 500 ms 的 tick，所以这里做成同步接口。
 * 唤醒动作：隐藏壁纸、恢复用户亮度、清掉 s_dimmed。
 */
void app_settings_wake_now(void);

/**
 * @brief 由 UI 的 500 ms 定时器调用
 *
 * 逻辑见 app_settings.c：超过 timeout 没有触摸就显示休眠壁纸并把背光降到
 * 30%，息屏后检测到无操作时长回落（有新输入把它归零）就恢复到用户亮度。
 */
void app_settings_tick(void);
