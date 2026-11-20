/*
 * 设备在线状态（S2-c 单设备版 → S2-e 多设备）
 *
 * 解析 hub/v1/devices/{device_id}/status（含 LWT 的 offline 帧），按设备槽位
 * 提供互斥保护的只读快照。槽位由 device_registry 分配，和 device_metrics
 * 共用同一套下标。在线判定规则见 .c 文件头与 protocol/topics-v1.md §3。
 *
 * state 字段已经是「综合判定过」的结果：
 *   - ONLINE   在线（心跳新鲜）
 *   - OFFLINE  离线（收到 LWT 或显式 offline）
 *   - UNKNOWN  未知（从没收到 / 心跳过期 / 状态字符串不认识）
 *
 * 注意：UNKNOWN 不一定代表设备真的不可用——还可能只是数据过期。
 * UI 通过 expired 区分「过期」和「从未收到」两种文案。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DEVICE_STATUS_UNKNOWN = 0,
    DEVICE_STATUS_ONLINE,
    DEVICE_STATUS_OFFLINE,
} device_status_state_t;

typedef struct {
    device_status_state_t state;
    bool     ever_received;      /*!< 至少收到过一帧 */
    uint32_t frame_count;

    char     hostname[32];
    char     ip[16];
    bool     has_clock_flag;     /*!< 帧里是否带了 clock_synced */
    bool     clock_synced;       /*!< 设备自己报的时间是否已同步 */
    bool     has_uptime;
    uint32_t uptime_seconds;

    /* 新鲜度：由 device_status_get() 现算 */
    uint32_t valid_for_seconds;
    uint32_t age_ms;
    bool     expired;            /*!< 仅对「曾经 online 但超时」为 true */
} device_status_t;

/**
 * @brief 初始化互斥锁与全部空快照
 *
 * 必须在注册 MQTT 数据回调之前调用（未初始化时 on_message 直接返回）。
 */
esp_err_t device_status_init(void);

/** 收到一条 MQTT 消息时调用；非 status 主题会被忽略 */
void device_status_on_message(const char *topic, int topic_len,
                              const char *data, int data_len);

/**
 * @brief 取某个设备槽位的状态快照（已含 age_ms / expired 现算字段）
 *
 * @param slot  device_registry 分配的槽位号；越界时返回全零（UNKNOWN）
 */
void device_status_get(int slot, device_status_t *out);