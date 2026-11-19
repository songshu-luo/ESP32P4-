/*
 * 泰山派 3M 指标解析（S2-b）
 *
 * 设计约束：
 *   - 解析在 MQTT 事件回调里做（esp-mqtt 的任务），绝不在 LVGL 线程里；
 *   - UI 只调 device_metrics_get() 拿互斥保护快照；
 *   - 解析失败不破坏上一帧有效数据，只记日志；
 *   - ESP32 无 RTC，新鲜度用「到达时刻 + valid_for_seconds」衡量。
 */

#include "device_metrics.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"

static const char *TAG = "metrics";

/* 只认这一个主题：hub/v1/devices/taishan-3m/metrics */
#define TOPIC_PREFIX     "hub/v1/devices/taishan-3m/"
#define TOPIC_SUFFIX     "metrics"

/* 单帧 JSON 最大长度。实际约 400 B，给 4 KB 余量，超了直接丢弃不解析。 */
#define MAX_PAYLOAD      (4096)

static SemaphoreHandle_t s_lock;
static device_metrics_t  s_snapshot;
static int64_t           s_last_arrival_us;   /* 0 = 从未收到 */
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

/* ------------------------------------------------------------ 解析 */

/** 把一帧 JSON 拆进 dst。返回 false 表示信封不合格，调用方不应采纳。 */
static bool parse_envelope(const cJSON *root, device_metrics_t *dst)
{
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(ver) || (int)ver->valuedouble != 1) {
        ESP_LOGW(TAG, "unsupported schema_version, frame dropped");
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

    /* ---- temperature ---- */
    dst->has_soc = get_number_in(payload, "temperature", "soc_celsius", &v);
    if (dst->has_soc) {
        dst->soc_celsius = v;
    }
    dst->has_soc_max = get_number_in(payload, "temperature", "max_celsius", &v);
    if (dst->has_soc_max) {
        dst->soc_max_celsius = v;
    }
    dst->temp_zone_count = count_zones(payload);

    /* ---- disk ---- */
    dst->has_disk = get_number_in(payload, "disk", "used_percent", &v);
    if (dst->has_disk) {
        dst->disk_percent = v;
        get_number_in(payload, "disk", "used_bytes", &dst->disk_used_bytes);
        get_number_in(payload, "disk", "total_bytes", &dst->disk_total_bytes);
    }

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

    /* 主题过滤：只处理 hub/v1/devices/taishan-3m/metrics */
    const int prefix_len = (int)strlen(TOPIC_PREFIX);
    const int suffix_len = (int)strlen(TOPIC_SUFFIX);
    if (topic_len != prefix_len + suffix_len ||
        memcmp(topic, TOPIC_PREFIX, prefix_len) != 0 ||
        memcmp(topic + prefix_len, TOPIC_SUFFIX, suffix_len) != 0) {
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
        next.frame_count = s_snapshot.frame_count + 1;
        /* 保留上一次的帧数之外，其余整份替换 —— 字段缺失要真的清掉，
         * 不能沿用上一帧的旧值，否则「内存温度没了」会显示成「温度没变」。 */
        s_snapshot = next;
        s_last_arrival_us = now_us;
        xSemaphoreGive(s_lock);
    }

    /* 只在首帧和每 30 帧打一次，避免 2 秒一条刷屏盖住其他日志。
     * 缺字段时按协议要求打 n/a，不用 0 冒充。 */
    if (next.frame_count == 1 || next.frame_count % 30 == 0) {
        char cpu[12] = "n/a", mem[12] = "n/a", soc[12] = "n/a", disk[12] = "n/a";
        if (next.has_cpu)  { snprintf(cpu,  sizeof(cpu),  "%.1f%%", next.cpu_percent); }
        if (next.has_mem)  { snprintf(mem,  sizeof(mem),  "%.1f%%", next.mem_percent); }
        if (next.has_soc)  { snprintf(soc,  sizeof(soc),  "%.1fC",  next.soc_celsius); }
        if (next.has_disk) { snprintf(disk, sizeof(disk), "%.1f%%", next.disk_percent); }
        ESP_LOGI(TAG, "frame #%u  cpu %s  mem %s  soc %s  disk %s",
                 (unsigned)next.frame_count, cpu, mem, soc, disk);
    }
}

void device_metrics_get(device_metrics_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    *out = s_snapshot;

    /* 新鲜度现算：到达时刻 + valid_for_seconds */
    if (s_last_arrival_us > 0) {
        const int64_t age_us = esp_timer_get_time() - s_last_arrival_us;
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
        memset(&s_snapshot, 0, sizeof(s_snapshot));
        s_last_arrival_us = 0;
        xSemaphoreGive(s_lock);
    }
    s_initialized = true;
    return ESP_OK;
}
