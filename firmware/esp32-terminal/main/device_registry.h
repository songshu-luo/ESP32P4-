/*
 * 设备索引（S2-e）
 *
 * 多设备后，屏幕要能同时显示泰山派、Windows 主机、云端的多份数据。
 * 这层只做一件事：把 device_id 映射成一个固定的槽位号，让指标快照
 * （device_metrics）和在线状态（device_status）能用同一个下标配对。
 *
 * 槽位在「第一次收到该设备的任意一条消息」时分配，之后不变：
 *   - 总览页的卡片不会因为数据到达顺序变化而跳动；
 *   - 监控页翻页用的是同一个下标，指标和状态永远指向同一台设备。
 *
 * 容量只有 4：协议里预定的设备是 taishan-3m / windows-main / cloud-main，
 * 留一个余量。槽位表是纯静态内存（约 4 × 64 B），不涉及堆分配。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DEVICE_REGISTRY_MAX_DEVICES (4)
#define DEVICE_REGISTRY_ID_LEN      (32)   /*!< 协议：device_id ≤ 32 字节 */
#define DEVICE_REGISTRY_NAME_LEN    (24)

typedef struct {
    char id[DEVICE_REGISTRY_ID_LEN];     /*!< 协议键，来自主题 */
    char name[DEVICE_REGISTRY_NAME_LEN]; /*!< 中文显示名；空串表示未知设备，UI 直接显示 id */
} device_registry_entry_t;

/**
 * @brief 初始化槽位表与互斥锁
 *
 * 必须在 device_metrics_init() / device_status_init() 之前调用。
 */
esp_err_t device_registry_init(void);

/**
 * @brief 查找 device_id 对应的槽位，不存在就分配一个新的
 *
 * @param id      主题里解析出来的 device_id（不要求 NUL 结尾）
 * @param id_len  字节长度
 * @return 槽位号（0..count-1）；id 非法或槽位已满时返回 -1
 *
 * 合法性按协议 §1 校验：只允许 [a-z0-9-]，长度 1..31。
 * 校验通过才分配，所以 hub/v1/devices/a/b/metrics 这种多级主题不会被误收。
 */
int device_registry_slot(const char *id, int id_len);

/**
 * @brief 解析 hub/v1/devices/{device_id}/{suffix} 并返回槽位
 *
 * @param suffix  尾部字面量，"metrics" 或 "status"（不含前导斜杠）
 * @return 槽位号；主题不匹配或 device_id 非法时返回 -1
 *
 * 主题必须**恰好**是这四段：多一段（hub/v1/devices/a/b/metrics）或多一个
 * 字符（…/metricsx）都不会被匹配，避免把结构不对的主题当成指标收下。
 */
int device_registry_slot_from_topic(const char *topic, int topic_len,
                                    const char *suffix);

/** 已分配的槽位数（0 表示还没收到过任何设备的消息） */
int device_registry_count(void);

/** 读一个槽位；下标越界时填零（id 为空串） */
void device_registry_get(int slot, device_registry_entry_t *out);