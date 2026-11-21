/*
 * 多设备指标解析（S2-e，原 S2-b 单设备版改造）
 *
 * 设计约束：
 *   - 解析在 MQTT 事件回调里做（esp-mqtt 的任务），绝不在 LVGL 线程里；
 *   - UI 只调 device_metrics_get() 拿互斥保护快照；
 *   - 解析失败不破坏上一帧有效数据，只记日志；
 *   - ESP32 无 RTC，新鲜度用「到达时刻 + valid_for_seconds」衡量。
 *
 * S2-e 的改动：快照从「一份」变成「按设备槽位的一份数组」。槽位号来自
 * device_registry（从主题里解析 device_id 得到），所以同一个下标在
 * device_metrics 和 device_status 里指的是同一台设备。
 */

#include "device_metrics.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"

#include "device_registry.h"

static const char *TAG = "metrics";

/* 只认 hub/v1/devices/{device_id}/metrics；device_id 的解析交给 device_registry */
#define TOPIC_SUFFIX     "metrics"

/* 单帧 JSON 最大长度。实际约 400 B，给 4 KB 余量，超了直接丢弃不解析。 */
#define MAX_PAYLOAD      (4096)

static SemaphoreHandle_t s_lock;
static device_metrics_t  s_snapshot[DEVICE_REGISTRY_MAX_DEVICES];
static int64_t           s_last_arrival_us[DEVICE_REGISTRY_MAX_DEVICES];  /* 0 = 从未收到 */
static bool              s_initialized;

/* ------------------------------------------------------------ 小工具 */

/** 取数字字段；缺失或类型不对都返回 false，调用方据此保留 has_ 标志为 false */
static bool get_number(const cJSON *obj, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

/** 取嵌套对象的数字字段：get_number_path(root, "cpu", "usage_percent") */
static bool get_number_in(const cJSON *root, const char *parent, const char *key, double *out)
{
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, parent);
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    return get_number(obj, key, out);
}

/** 取字符串字段；非字符串或超长都返回 false（超长宁可丢弃，不截断成另一个名字） */
static bool get_string(const cJSON *obj, const char *key, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    const size_t n = strlen(item->valuestring);
    if (n == 0 || n >= len) {
        return false;
    }
    memcpy(out, item->valuestring, n + 1);
    return true;
}

/** 取嵌套对象的字符串字段 */
static bool get_string_in(const cJSON *root, const char *parent,
                          const char *key, char *out, size_t len)
{
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, parent);
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    return get_string(obj, key, out, len);
}

/** 统计 temperature.zones 里的温区数量（协议里是个对象，不是数组） */
static int count_zones(const cJSON *root)
{
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    if (!cJSON_IsObject(temp)) {
        return 0;
    }
    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(temp, "zones");
    if (!cJSON_IsObject(zones)) {
        return 0;
    }
    int n = 0;
    for (const cJSON *z = zones->child; z != NULL; z = z->next) {
        if (cJSON_IsNumber(z)) {
            n++;
        }
    }
    return n;
}

/**
 * 把 temperature.zones 这个 { "soc-thermal": 46.5, ... } 对象摊平成数组。
 *
 * 只保留数值项；名字按 thermal_zone 的 type 原样保留（LCD 上会显示成
 * soc-thermal / bigcore-thermal …）。超出 DEVICE_METRICS_MAX_ZONES 的部分丢弃，
 * temp_zone_count 仍然记录真实总数，UI 可以据此提示「还有 N 个温区未显示」。
 */
static int flatten_zones(const cJSON *root, device_metrics_t *dst)
{
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    if (!cJSON_IsObject(temp)) {
        return 0;
    }
    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(temp, "zones");
    if (!cJSON_IsObject(zones)) {
        return 0;
    }
    int stored = 0;
    for (const cJSON *z = zones->child; z != NULL; z = z->next) {
        if (!cJSON_IsNumber(z) || z->string == NULL) {
            continue;
        }
        const size_t n = strlen(z->string);
        if (n == 0 || n >= DEVICE_METRICS_NAME_LEN) {
            continue;
        }
        if (stored >= DEVICE_METRICS_MAX_ZONES) {
            continue;   /* 继续遍历只为数数，写入已经满了 */
        }
        memcpy(dst->zones[stored].name, z->string, n + 1);
        dst->zones[stored].celsius = z->valuedouble;
        stored++;
    }
    return stored;
}

/**
 * 把 services 这个 { "mosquitto": "active", ... } 对象摊平成数组。
 * 只保留字符串值；名字按配置里的单元名原样保留。
 */
static int flatten_services(const cJSON *root, device_metrics_t *dst)
{
    const cJSON *services = cJSON_GetObjectItemCaseSensitive(root, "services");
    if (!cJSON_IsObject(services)) {
        return 0;
    }
    int stored = 0;
    for (const cJSON *s = services->child; s != NULL; s = s->next) {
        if (!cJSON_IsString(s) || s->string == NULL || s->valuestring == NULL) {
            continue;
        }
        const size_t n = strlen(s->string);
        if (n == 0 || n >= DEVICE_METRICS_NAME_LEN) {
            continue;
        }
        if (stored >= DEVICE_METRICS_MAX_SERVICES) {
            continue;
        }
        memcpy(dst->services[stored].name, s->string, n + 1);
        /* 状态只有 active/inactive/failed/… 几个短词；超长说明协议变了，按 unknown 处理 */
        const size_t m = strlen(s->valuestring);
        if (m > 0 && m < sizeof(dst->services[stored].state)) {
            memcpy(dst->services[stored].state, s->valuestring, m + 1);
        } else {
            memcpy(dst->services[stored].state, "unknown", sizeof("unknown"));
        }
        stored++;
    }
    return stored;
}

/**
 * 把 checks 数组摊平成定长数组。返回条数；checks 缺失或不是数组时返回 -1。
 *
 * 每条必须同时有 id 和 label 才算有效（id 给程序看，label 给屏上看，缺一个就
 * 无法定位/显示）；state 缺失或不是 ok/warn/down 时按 UNKNOWN，不猜成 ok。
 * detail 是可选的小字，读不到就留空串。
 */
static int flatten_checks(const cJSON *root, device_metrics_t *dst)
{
    const cJSON *checks = cJSON_GetObjectItemCaseSensitive(root, "checks");
    if (!cJSON_IsArray(checks)) {
        return -1;
    }
    int stored = 0;
    int seen = 0;
    for (const cJSON *c = checks->child; c != NULL; c = c->next) {
        if (!cJSON_IsObject(c)) {
            continue;
        }
        seen++;
        if (stored >= DEVICE_METRICS_MAX_CHECKS) {
            continue;   /* 协议只允许 5 条，多出来的丢弃 */
        }
        device_metrics_check_t *row = &dst->checks[stored];
        if (!get_string(c, "id", row->id, sizeof(row->id)) ||
            !get_string(c, "label", row->label, sizeof(row->label))) {
            continue;   /* 没有 id/label 的项没法上屏 */
        }
        row->state = DEVICE_CHECK_UNKNOWN;
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(c, "state");
        if (cJSON_IsString(state) && state->valuestring != NULL) {
            if (strcmp(state->valuestring, "ok") == 0) {
                row->state = DEVICE_CHECK_OK;
            } else if (strcmp(state->valuestring, "warn") == 0) {
                row->state = DEVICE_CHECK_WARN;
            } else if (strcmp(state->valuestring, "down") == 0) {
                row->state = DEVICE_CHECK_DOWN;
            }
            /* 其余字符串（拼错、协议新增状态）一律 UNKNOWN，UI 显示灰点 */
        }
        get_string(c, "detail", row->detail, sizeof(row->detail));
        stored++;
    }
    /* 数组里确实有条目、却一条都没解析出来 → 按解析失败处理（-1），
     * 和「数组为空」区分开；两种在 UI 上都显示「等待数据」。 */
    if (seen > 0 && stored == 0) {
        return -1;
    }
    return stored;
}

/**
 * 把 network.interfaces 数组摊平成定长数组（v6）。返回存下的条数（0~2）。
 *
 * 每条必须同时有非空的 id 和 type 才算有效（id 用来和 active_id 对齐，type
 * 决定显示有线还是无线图标）；status == "up" 才是 up，其余（含缺失）都按 down，
 * 不猜成在线。link_mbps / 速率是可选数值，null 或缺失时对应的 has_* 保持 false。
 * 不是数组时返回 0（UI 据此回落到旧字段）。
 */
static int flatten_netifs(const cJSON *network, device_metrics_t *dst)
{
    const cJSON *interfaces = cJSON_GetObjectItemCaseSensitive(network, "interfaces");
    if (!cJSON_IsArray(interfaces)) {
        return 0;
    }
    int stored = 0;
    for (const cJSON *it = interfaces->child;
         it != NULL && stored < DEVICE_METRICS_MAX_NETIFS;
         it = it->next) {
        if (!cJSON_IsObject(it)) {
            continue;
        }
        device_metrics_netif_t *row = &dst->netifs[stored];
        if (!get_string(it, "id", row->id, sizeof(row->id)) ||
            !get_string(it, "type", row->type, sizeof(row->type))) {
            continue;   /* 没有 id/type 的项没法定位或选图标，整条丢弃 */
        }
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(it, "status");
        row->up = cJSON_IsString(status) && status->valuestring != NULL &&
                  strcmp(status->valuestring, "up") == 0;

        double v = 0;
        row->has_link = get_number(it, "link_mbps", &v) && v >= 0;
        if (row->has_link) {
            row->link_mbps = v;
        }
        row->has_rx = get_number(it, "rx_bytes_per_sec", &v) && v >= 0;
        if (row->has_rx) {
            row->rx_bytes_per_sec = v;
        }
        row->has_tx = get_number(it, "tx_bytes_per_sec", &v) && v >= 0;
        if (row->has_tx) {
            row->tx_bytes_per_sec = v;
        }
        stored++;
    }
    return stored;
}

/* ------------------------------------------------------------ 解析 */

/** 把一帧 JSON 拆进 dst。返回 false 表示信封不合格，调用方不应采纳。 */
static bool parse_envelope(const cJSON *root, device_metrics_t *dst)
{
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(ver)) {
        ESP_LOGW(TAG, "missing schema_version, frame dropped");
        return false;
    }
    /* v2 加了 payload.gpu，v3 加了 processes/network/disks，v4 加了 containers，
     * v5 加了 checks（任务状态），v6 加了 network.interfaces / active_id / active_type；
     * 旧字段含义一直没动，所以 1~6 都收；再高就不认识了，直接丢。 */
    const int schema = (int)ver->valuedouble;
    if (schema < 1 || schema > 6) {
        ESP_LOGW(TAG, "unsupported schema_version %d, frame dropped", schema);
        return false;
    }

    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "missing payload object, frame dropped");
        return false;
    }

    double valid_for = 0;
    if (get_number(root, "valid_for_seconds", &valid_for) && valid_for > 0) {
        dst->valid_for_seconds = (uint32_t)valid_for;
    } else {
        /* 协议要求必须有；缺失时按最保守的短有效期处理，宁可显示过期 */
        dst->valid_for_seconds = 8;
    }

    /* ---- cpu ---- */
    double v = 0;
    dst->has_cpu = get_number_in(payload, "cpu", "usage_percent", &v);
    if (dst->has_cpu) {
        dst->cpu_percent = v;
    }
    double cores = 0;
    dst->cpu_cores = get_number_in(payload, "cpu", "cores", &cores) ? (int)cores : 0;

    double phys = 0;
    dst->has_cpu_physical = get_number_in(payload, "cpu", "physical_cores", &phys);
    if (dst->has_cpu_physical) {
        dst->cpu_physical_cores = (int)phys;
    }

    dst->has_load = get_number_in(payload, "cpu", "load1", &dst->load1);
    if (dst->has_load) {
        get_number_in(payload, "cpu", "load5", &dst->load5);
        get_number_in(payload, "cpu", "load15", &dst->load15);
    }

    /* ---- memory ---- */
    dst->has_mem = get_number_in(payload, "memory", "used_percent", &v);
    if (dst->has_mem) {
        dst->mem_percent = v;
        get_number_in(payload, "memory", "used_bytes", &dst->mem_used_bytes);
        get_number_in(payload, "memory", "total_bytes", &dst->mem_total_bytes);
    }
    /* swap 单独判定：没有 swap 分区时协议给 null，此时不要显示成 0% */
    dst->has_swap = get_number_in(payload, "memory", "swap_used_percent", &v);
    if (dst->has_swap) {
        dst->swap_used_percent = v;
    }

    /* ---- temperature ---- */
    dst->has_soc = get_number_in(payload, "temperature", "soc_celsius", &v);
    if (dst->has_soc) {
        dst->soc_celsius = v;
    }
    dst->has_soc_max = get_number_in(payload, "temperature", "max_celsius", &v);
    if (dst->has_soc_max) {
        dst->soc_max_celsius = v;
    }
    dst->zones_len = flatten_zones(payload, dst);
    dst->temp_zone_count = count_zones(payload);

    /* ---- disk ---- */
    dst->has_disk = get_number_in(payload, "disk", "used_percent", &v);
    if (dst->has_disk) {
        dst->disk_percent = v;
        get_number_in(payload, "disk", "used_bytes", &dst->disk_used_bytes);
        get_number_in(payload, "disk", "total_bytes", &dst->disk_total_bytes);
    }
    get_string_in(payload, "disk", "path", dst->disk_path, sizeof(dst->disk_path));

    /* ---- systemd 服务 ---- */
    dst->services_len = flatten_services(payload, dst);

    /* ---- gpu（schema v2；v1 帧没有这个对象，整块保持 false） ---- */
    const cJSON *gpu = cJSON_GetObjectItemCaseSensitive(payload, "gpu");
    if (cJSON_IsObject(gpu)) {
        dst->has_gpu = true;
        /* 名字只用于卡片右上角那行小字，取不到就留空，不影响其它字段 */
        get_string(gpu, "name", dst->gpu_name, sizeof(dst->gpu_name));

        dst->has_gpu_usage = get_number(gpu, "usage_percent", &v);
        if (dst->has_gpu_usage) {
            dst->gpu_percent = v;
        }
        dst->has_gpu_temp = get_number(gpu, "temp_celsius", &v);
        if (dst->has_gpu_temp) {
            dst->gpu_celsius = v;
        }
        /* 显存要求 used 与 total 同时可用：只有一半的话没法定百分比，
         * 显示 "0 / 8.0 GB" 比显示 "-- / 8.0 GB" 更容易被误读成「真的空了」 */
        dst->has_gpu_mem = get_number(gpu, "memory_used_bytes", &dst->gpu_mem_used_bytes) &&
                           get_number(gpu, "memory_total_bytes", &dst->gpu_mem_total_bytes) &&
                           dst->gpu_mem_total_bytes > 0;
    }

    /* ---- 进程（v3；数组，只保留前 3 条，process_count 仍是真实总数） ---- */
    const cJSON *processes = cJSON_GetObjectItemCaseSensitive(payload, "processes");
    if (cJSON_IsArray(processes)) {
        for (const cJSON *p = processes->child;
             p != NULL && dst->processes_len < DEVICE_METRICS_MAX_PROCESSES;
             p = p->next) {
            if (!cJSON_IsObject(p)) {
                continue;
            }
            device_metrics_process_t *row = &dst->processes[dst->processes_len];
            if (!get_string(p, "name", row->name, sizeof(row->name))) {
                continue;   /* 没有名字的行没法上屏，整行丢弃 */
            }
            row->has_cpu = get_number(p, "cpu_percent", &row->cpu_percent);
            row->has_mem = get_number(p, "memory_bytes", &row->memory_bytes);
            dst->processes_len++;
        }
    }
    double proc_total = 0;
    dst->has_process_count = get_number(payload, "process_count", &proc_total);
    if (dst->has_process_count) {
        dst->process_count = (int)proc_total;
    }

    /* ---- 网络（v3；v6 起旧字段语义变成「当前上网卡」的单卡速率） ----
     *
     * v6 新增 fields 先清零：整份快照虽然是 memset 出来的，这里仍显式清一遍，
     * 免得以后改成「就地更新 dst」时残留上一帧的网卡明细。 */
    dst->netifs_len = 0;
    memset(dst->netifs, 0, sizeof(dst->netifs));
    dst->has_active = false;
    dst->active_id[0] = '\0';
    dst->active_type[0] = '\0';

    const cJSON *network = cJSON_GetObjectItemCaseSensitive(payload, "network");
    if (cJSON_IsObject(network)) {
        dst->has_net_rx = get_number(network, "rx_bytes_per_sec", &v) && v >= 0;
        if (dst->has_net_rx) {
            dst->net_rx_bytes_per_sec = v;
        }
        dst->has_net_tx = get_number(network, "tx_bytes_per_sec", &v) && v >= 0;
        if (dst->has_net_tx) {
            dst->net_tx_bytes_per_sec = v;
        }

        /* v6：分网卡明细 + 当前上网卡。两者都是可选的：v1–v5 帧里没有，
         * 解析失败时 Agent 也会降级成 interfaces=[] / active_id=null。 */
        dst->netifs_len = flatten_netifs(network, dst);

        /* active_id / active_type 先读到临时缓冲，两者都拿到才一起写入：
         * 缺一个就整体当「没有当前上网卡」（has_active=false、两字段留空串），
         * 只有 id 没有 type 时 UI 也选不出有线 / 无线图标。 */
        char active_id[DEVICE_METRICS_NETIF_ID_LEN];
        char active_type[DEVICE_METRICS_NETIF_TYPE_LEN];
        if (get_string(network, "active_id", active_id, sizeof(active_id)) &&
            get_string(network, "active_type", active_type, sizeof(active_type))) {
            memcpy(dst->active_id, active_id, sizeof(dst->active_id));
            memcpy(dst->active_type, active_type, sizeof(dst->active_type));
            dst->has_active = true;
        }
    }

    /* ---- 分区列表（v3；disk 单对象仍表示主分区） ---- */
    const cJSON *disks = cJSON_GetObjectItemCaseSensitive(payload, "disks");
    if (cJSON_IsArray(disks)) {
        for (const cJSON *d = disks->child;
             d != NULL && dst->disks_len < DEVICE_METRICS_MAX_DISKS;
             d = d->next) {
            if (!cJSON_IsObject(d)) {
                continue;
            }
            device_metrics_disk_t *row = &dst->disks[dst->disks_len];
            if (!get_string(d, "path", row->path, sizeof(row->path))) {
                continue;
            }
            if (!get_number(d, "used_percent", &row->used_percent)) {
                continue;
            }
            get_number(d, "used_bytes", &row->used_bytes);
            get_number(d, "total_bytes", &row->total_bytes);
            dst->disks_len++;
        }
    }

    /* ---- 容器（v4；只保留前 4 条，container_count 是真实总数） ---- */
    const cJSON *containers = cJSON_GetObjectItemCaseSensitive(payload, "containers");
    if (cJSON_IsArray(containers)) {
        for (const cJSON *c = containers->child;
             c != NULL && dst->containers_len < DEVICE_METRICS_MAX_CONTAINERS;
             c = c->next) {
            if (!cJSON_IsObject(c)) {
                continue;
            }
            device_metrics_container_t *row = &dst->containers[dst->containers_len];
            /* 容器名是关键，读不到就整行丢；service 缺失是正常情况（非 compose 起的） */
            if (!get_string(c, "name", row->name, sizeof(row->name))) {
                continue;
            }
            row->service[0] = '\0';
            get_string(c, "service", row->service, sizeof(row->service));
            dst->containers_len++;
        }
    }
    double cont_total = 0;
    dst->has_container_count = get_number(payload, "container_count", &cont_total);
    if (dst->has_container_count) {
        dst->container_count = (int)cont_total;
    }

    /* ---- 任务状态（v5；缺失或不是数组时 checks_count = -1，UI 显示「等待数据」） ---- */
    dst->checks_count = flatten_checks(payload, dst);

    /* ---- uptime ---- */
    dst->has_uptime = get_number(payload, "uptime_seconds", &v);
    if (dst->has_uptime) {
        dst->uptime_seconds = (uint32_t)v;
    }

    return true;
}

/* ------------------------------------------------------------ 对外接口 */

void device_metrics_on_message(const char *topic, int topic_len,
                               const char *data, int data_len)
{
    if (!s_initialized || topic == NULL || data == NULL) {
        return;
    }

    /* 主题过滤 + device_id 解析都交给 device_registry：只有恰好四段的
     * hub/v1/devices/{id}/metrics 才会通过，其余一律忽略。 */
    const int slot = device_registry_slot_from_topic(topic, topic_len, TOPIC_SUFFIX);
    if (slot < 0) {
        return;
    }

    if (data_len <= 0 || data_len > MAX_PAYLOAD) {
        ESP_LOGW(TAG, "payload size %d out of range, dropped", data_len);
        return;
    }

    /* 直接解析 MQTT 缓冲区，不做拷贝：
     *   - 本函数跑在 esp-mqtt 任务里，默认栈只有 6 KB，再开 4 KB 的栈缓冲会溢出；
     *   - cJSON_ParseWithLength() 是按长度解析的有界接口，不要求 NUL 结尾，
     *     也不会越过 data_len 读到下一个报文的字节。 */
    cJSON *root = cJSON_ParseWithLength(data, (size_t)data_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed (%d bytes), frame dropped", data_len);
        return;
    }

    /* 先解析到临时变量，成功后一次性换上去：解析中途失败不会污染上一帧 */
    device_metrics_t next;
    memset(&next, 0, sizeof(next));
    next.valid = true;

    const bool ok = parse_envelope(root, &next);
    cJSON_Delete(root);

    if (!ok) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();

    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        next.frame_count = s_snapshot[slot].frame_count + 1;
        /* 保留上一次的帧数之外，其余整份替换 —— 字段缺失要真的清掉，
         * 不能沿用上一帧的旧值，否则「内存温度没了」会显示成「温度没变」。 */
        s_snapshot[slot] = next;
        s_last_arrival_us[slot] = now_us;
        xSemaphoreGive(s_lock);
    }

    /* 只在首帧和每 30 帧打一次，避免 2 秒一条刷屏盖住其他日志。
     * 缺字段时按协议要求打 n/a，不用 0 冒充。 */
    if (next.frame_count == 1 || next.frame_count % 30 == 0) {
        char cpu[12] = "n/a", mem[12] = "n/a", soc[12] = "n/a", disk[12] = "n/a";
        char gpu[20] = "n/a";
        if (next.has_cpu)  { snprintf(cpu,  sizeof(cpu),  "%.1f%%", next.cpu_percent); }
        if (next.has_mem)  { snprintf(mem,  sizeof(mem),  "%.1f%%", next.mem_percent); }
        if (next.has_soc)  { snprintf(soc,  sizeof(soc),  "%.1fC",  next.soc_celsius); }
        if (next.has_disk) { snprintf(disk, sizeof(disk), "%.1f%%", next.disk_percent); }
        if (next.has_gpu_usage || next.has_gpu_temp) {
            char util[12] = "n/a", temp[12] = "n/a";
            if (next.has_gpu_usage) { snprintf(util, sizeof(util), "%.0f%%", next.gpu_percent); }
            if (next.has_gpu_temp)  { snprintf(temp, sizeof(temp), "%.0fC",  next.gpu_celsius); }
            snprintf(gpu, sizeof(gpu), "%s/%s", util, temp);
        }
        ESP_LOGI(TAG, "device %d frame #%u  cpu %s  gpu %s  mem %s  soc %s  disk %s  proc %d/%d  net %d  disks %d  cont %d/%d  chk %d",
                 slot, (unsigned)next.frame_count, cpu, gpu, mem, soc, disk,
                 next.processes_len,
                 next.has_process_count ? (int)next.process_count : -1,
                 (next.has_net_rx || next.has_net_tx) ? 1 : 0,
                 next.disks_len,
                 next.containers_len,
                 next.has_container_count ? (int)next.container_count : -1,
                 next.checks_count);
    }
}

void device_metrics_get(int slot, device_metrics_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= DEVICE_REGISTRY_MAX_DEVICES || s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    *out = s_snapshot[slot];

    /* 新鲜度现算：到达时刻 + valid_for_seconds */
    if (s_last_arrival_us[slot] > 0) {
        const int64_t age_us = esp_timer_get_time() - s_last_arrival_us[slot];
        out->age_ms = (uint32_t)(age_us / 1000);
        out->expired = age_us > (int64_t)out->valid_for_seconds * 1000000;
    } else {
        out->expired = true;   /* 从没收到过 → 视为过期，UI 显示 "--" */
    }

    xSemaphoreGive(s_lock);
}

esp_err_t device_metrics_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_snapshot, 0, sizeof(s_snapshot));
        memset(s_last_arrival_us, 0, sizeof(s_last_arrival_us));
        xSemaphoreGive(s_lock);
    }
    s_initialized = true;
    return ESP_OK;
}
