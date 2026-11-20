/*
 * 多设备指标（S2-b 单设备版 → S2-e 多设备）
 *
 * 只负责一件事：把 hub/v1/devices/{device_id}/metrics 的 JSON 解析成
 * 按「设备槽位」排布的只读快照。槽位由 device_registry 分配，
 * 指标和在线状态共用同一套下标，所以两边永远指向同一台设备。
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

/* GPU 名字比温区名长得多：nvidia-smi 报的是
 * "NVIDIA GeForce RTX 4060 Laptop GPU"（37 字节），单独给一个上限 */
#define DEVICE_METRICS_GPU_NAME_LEN (48)

/* v3：屏幕上的进程卡只放 3 行，磁盘卡只列主分区 + 2 个别的小字 */
#define DEVICE_METRICS_MAX_PROCESSES (3)
#define DEVICE_METRICS_MAX_DISKS     (4)
/* v4：容器名可能带 compose 前缀，最长见到 27 字符，给 40 字节 */
#define DEVICE_METRICS_MAX_CONTAINERS   (4)
#define DEVICE_METRICS_CONTAINER_NAME_LEN (40)

/** 一条进程：name 是 Get-Process 的 ProcessName（不含 .exe） */
typedef struct {
    char   name[DEVICE_METRICS_NAME_LEN];
    bool   has_cpu;
    double cpu_percent;
    bool   has_mem;
    double memory_bytes;
} device_metrics_process_t;

/** 一个分区：path 形如 "C:\\"（3 字节），留 8 字节够用 */
typedef struct {
    char   path[8];
    double used_percent;
    double used_bytes;
    double total_bytes;
} device_metrics_disk_t;

/** 一个运行中的容器：service 是 compose 的服务名（没有则为空） */
typedef struct {
    char name[DEVICE_METRICS_CONTAINER_NAME_LEN];
    char service[DEVICE_METRICS_NAME_LEN];
} device_metrics_container_t;

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
    bool     has_cpu_physical;
    int      cpu_physical_cores;  /*!< v3；没有时 has_cpu_physical=false */
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

    /* gpu（schema v2；v1 帧里没有这个对象，全部 has_* 保持 false）
     *
     * 每个字段单独带 has_ 标志，而不是只看 has_gpu：笔记本的 nvidia-smi
     * 可能只给出利用率、不给温度（或显存读数为 N/A），这时 UI 该逐项显示
     * "--"，不能把整块 GPU 说成「未上报」。 */
    bool     has_gpu;
    char     gpu_name[DEVICE_METRICS_GPU_NAME_LEN];  /*!< 空串 = 未上报 */
    bool     has_gpu_usage;
    double   gpu_percent;
    bool     has_gpu_temp;
    double   gpu_celsius;
    bool     has_gpu_mem;
    double   gpu_mem_used_bytes;
    double   gpu_mem_total_bytes;

    /* 进程（v3；只存前 3 条，process_count 仍是真实总数） */
    int      processes_len;
    device_metrics_process_t processes[DEVICE_METRICS_MAX_PROCESSES];
    bool     has_process_count;
    int      process_count;

    /* 网络速率（v3；两次采样差分，首帧可能为 null） */
    bool     has_net_rx;
    double   net_rx_bytes_per_sec;
    bool     has_net_tx;
    double   net_tx_bytes_per_sec;

    /* 磁盘分区列表（v3；disk 单对象仍是主分区） */
    int      disks_len;
    device_metrics_disk_t disks[DEVICE_METRICS_MAX_DISKS];

    /* 容器（v4；只存前 4 条，container_count 仍是真实总数） */
    int      containers_len;
    device_metrics_container_t containers[DEVICE_METRICS_MAX_CONTAINERS];
    bool     has_container_count;
    int      container_count;

    /* 其他 */
    bool     has_uptime;
    uint32_t uptime_seconds;

    /* 新鲜度：由 device_metrics_get() 现算，不存进缓存 */
    uint32_t valid_for_seconds;
    uint32_t age_ms;
    bool     expired;
} device_metrics_t;

/**
 * @brief 初始化互斥锁与全部空快照
 *
 * 必须在注册 MQTT 数据回调之前调用（device_metrics_on_message 在未初始化时直接返回）。
 */
esp_err_t device_metrics_init(void);

/**
 * @brief 收到一条 MQTT 消息时调用
 *
 * 内部自己过滤主题（只认 hub/v1/devices/{id}/metrics），其余消息直接忽略，
 * 所以 mqtt_link 可以无脑把所有消息都转发进来。
 */
void device_metrics_on_message(const char *topic, int topic_len,
                               const char *data, int data_len);

/**
 * @brief 取某个设备槽位的快照（已含 age_ms / expired 现算字段）
 *
 * @param slot  device_registry 分配的槽位号；越界时返回全零（valid=false）
 */
void device_metrics_get(int slot, device_metrics_t *out);