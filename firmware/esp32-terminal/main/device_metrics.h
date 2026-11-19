/*
 * 泰山派 3M 指标（S2-b）
 *
 * 只负责一件事：把 hub/v1/devices/taishan-3m/metrics 的 JSON 解析成一份
 * 供 UI 读的只读快照。不做任何显示逻辑，也不做缓存归档。
 *
 * 新鲜度判定用「到达时刻 + valid_for_seconds」，而不是和 timestamp_utc 比。
 * 原因：ESP32 没有 RTC、也没有接 SNTP，time() 停在 1970；而 LAN 单程延迟
 * 远小于 1 秒，用到达时刻衡量新鲜度与协议里定义的语义等价，还免疫两端时钟漂移。
 *
 * 协议要求「字段缺失用 null 或不出现，不要用 0 冒充」。因此每个字段都带
 * has_* 标志：UI 必须在 has_==false 时显示 "--"，不能显示 0。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool     valid;              /*!< 至少成功解析过一帧 */
    uint32_t frame_count;        /*!< 成功解析的帧数，用于判断是否在刷新 */

    /* cpu */
    bool     has_cpu;
    double   cpu_percent;
    int      cpu_cores;
    bool     has_load;
    double   load1;
    double   load5;
    double   load15;

    /* memory */
    bool     has_mem;
    double   mem_percent;
    double   mem_used_bytes;
    double   mem_total_bytes;

    /* temperature */
    bool     has_soc;
    double   soc_celsius;
    bool     has_soc_max;
    double   soc_max_celsius;
    int      temp_zone_count;

    /* disk */
    bool     has_disk;
    double   disk_percent;
    double   disk_used_bytes;
    double   disk_total_bytes;

    /* 其他（S2-b 暂不上屏，留给后面的监控页） */
    bool     has_uptime;
    uint32_t uptime_seconds;

    /* 新鲜度：由 device_metrics_get() 现算，不存进缓存 */
    uint32_t valid_for_seconds;
    uint32_t age_ms;
    bool     expired;
} device_metrics_t;

/**
 * @brief 初始化互斥锁与空快照
 *
 * 必须在注册 MQTT 数据回调之前调用（device_metrics_on_message 在未初始化时直接返回）。
 */
esp_err_t device_metrics_init(void);

/**
 * @brief 收到一条 MQTT 消息时调用
 *
 * 内部会自己过滤主题，非 taishan-3m/metrics 的消息直接忽略，所以
 * mqtt_link 可以无脑把所有消息都转发进来。
 */
void device_metrics_on_message(const char *topic, int topic_len,
                               const char *data, int data_len);

/**
 * @brief 取一份快照（已含 age_ms / expired 现算字段）
 */
void device_metrics_get(device_metrics_t *out);
