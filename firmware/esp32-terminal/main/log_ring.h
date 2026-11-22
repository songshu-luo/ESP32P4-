/*
 * 运行日志环形缓冲（诊断日志卡）
 *
 * 目的：把最近若干条 ESP_LOG 留在内存里，设置页可以直接显示，
 * 现场没有串口时也能看到「刚才哪一步失败」。
 *
 * 用法约束：
 *   - log_ring_init() 必须在其它模块初始化**之前**调用，否则会漏掉
 *     启动早期最关键的几条日志（板级信息、屏驱初始化）；
 *   - 钩子内部调用的是保存下来的原始 vprintf，串口输出保持原样；
 *   - 环形缓冲固定 16 行 × 160 字节，静态分配，不占堆。
 */

#pragma once

#include <stddef.h>

/**
 * @brief 安装日志钩子（幂等，重复调用无副作用）
 *
 * 必须在其它初始化之前调用。安装后每条 ESP_LOG 会同时：
 *   1. 原样输出到串口；
 *   2. 按行切分后进入 16 行环形缓冲。
 */
void log_ring_init(void);

/**
 * @brief 当前环形缓冲里已有的行数（0 表示确实还没有日志）
 *
 * 只读计数、不加锁，供 UI 区分「没有日志」和「这一拍 snapshot 拿锁失败」。
 */
size_t log_ring_count(void);

/**
 * @brief 取最近 max_lines 行日志（最新一行在最后，行间用 '\n' 分隔）
 *
 * @param out       输出缓冲，调用方负责提供
 * @param out_len   缓冲大小；放不下的行会被舍弃，不会写出越界
 * @param max_lines 最多取几行
 *
 * @return 实际写入的行数（不是字节数）；0 表示没有日志
 */
size_t log_ring_snapshot(char *out, size_t out_len, int max_lines);