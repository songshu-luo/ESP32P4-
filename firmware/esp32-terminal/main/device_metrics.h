/*
 * 泰山派 3M 指标（S2-b，S2-d 扩展到监控页字段）
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

/* 上屏数量上限：RK3576 实测 6 个温区，服务一般 1~3 个。
 * 上限比实际需求略大，解析层不做静默截断以外的假设（超出的部分会被丢弃并计数）。 */
#define DEVICE_METRICS_MAX_ZONES    (8)
#define DEVICE_METRICS_MAX_SERVICES (8)

/* 名字长度上限：最长的温区类型是 "little-core-thermal"（19 字节），留 1 字节结尾 */
#define DEVICE_METRICS_NAME_LEN     (24)

/** 一个温区：name 是 thermal_zone 的 type 原文，例如 soc-thermal */
typedef struct {
    char   name[DEVICE_METRICS_NAME_LEN];
    double celsius;
} device_metrics_zone_t;

/** 一个 systemd 服务：state 是 ActiveState 原文，例如 active / failed */
typedef struct {
    char name[DEVICE_METRICS_NAME_LEN];
    char state[16];
} device_metrics_service_t;

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
    bool     has_swap;
    double   swap_used_percent;  /*!< 没有 swap 分区时协议给 null → has_swap=false */

    /* temperature */
    bool     has_soc;
    double   soc_celsius;
    bool     has_soc_max;
    double   soc_max_celsius;
    int      temp_zone_count;    /*!< zones 对象里数值项的实际个数（可能大于 zones_len） */
    int      zones_len;          /*!< zones[] 里实际存下的条数，最多 DEVICE_METRICS_MAX_ZONES */
    device_metrics_zone_t zones[DEVICE_METRICS_MAX_ZONES];

    /* disk */
    bool     has_disk;
    double   disk_percent;
    double   disk_used_bytes;
    double   disk_total_bytes;
    char     disk_path[DEVICE_METRICS_NAME_LEN];  /*!< 空串 = 未上报 */

    /* systemd 服务 */
    int      services_len;       /*!< services[] 里实际存下的条数 */
    device_metrics_service_t services[DEVICE_METRICS_MAX_SERVICES];

    /* 其他 */
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
