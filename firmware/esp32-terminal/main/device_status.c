/*
 * 设备在线状态（S2-c 单设备版 → S2-e 多设备）
 *
 * 解析 hub/v1/devices/{device_id}/status，并实现协议 §3 的在线判定：
 *
 *   1. Hub 不可达（MQTT 断线）  → 未知        ← 这一条由 UI 结合 mqtt_link 判断
 *   2. 收到 LWT 的 state=offline → 离线
 *   3. state=online 且在有效期内 → 在线
 *   4. state=online 但已超时     → 未知（本模块置 expired，UI 可区分显示）
 *   5. 从没收到过                → 未知
 *
 * 为什么状态灯不能只看 status 主题：broker 一挂，最后一条 retained 的
 * "online" 还躺在那里，只看数据的话屏幕会一直绿着。所以「Hub 不可达」
 * 必须由订阅方覆盖成未知——这是协议里唯一一条关于「不要相信缓存」的硬性要求。
 *
 * 新鲜度同样用「到达时刻 + valid_for_seconds」，理由见 device_metrics.h。
 *
 * S2-e 的改动：快照从「一份」变成「按设备槽位的一份数组」，槽位号和
 * device_metrics 共用，所以下标相同即同一台设备。
 */

#include "device_status.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"

#include "device_registry.h"

static const char *TAG = "dev_status";

/* 只认 hub/v1/devices/{device_id}/status；device_id 的解析交给 device_registry */
#define TOPIC_SUFFIX        "status"

/* status 帧很小，1 KB 足够；超了直接丢弃不解析 */
#define MAX_PAYLOAD         (1024)

/* 协议 §4.1：status 每 5 s 一条、valid_for_seconds = 15。缺失时按同样的值兜底。 */
#define DEFAULT_VALID_FOR   (15)

static SemaphoreHandle_t s_lock;
static device_status_t   s_snapshot[DEVICE_REGISTRY_MAX_DEVICES];
static int64_t           s_last_arrival_us[DEVICE_REGISTRY_MAX_DEVICES];  /* 0 = 从未收到 */
static bool              s_initialized;

/* ------------------------------------------------------------ 小工具 */

static bool get_number(const cJSON *obj, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

/* ------------------------------------------------------------ 解析 */

/** 解析一帧 status。返回 false 表示信封或必填字段不合格，调用方不采纳。 */
static bool parse_frame(const cJSON *root, device_status_t *dst)
{
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(ver)) {
        ESP_LOGW(TAG, "missing schema_version, frame dropped");
        return false;
    }
    /* v2 只在 metrics 里加了 gpu，status 的字段一个都没动，所以 1 和 2 都收；
     * 再高就不认识了，直接丢。metrics 的同一处判断见 device_metrics.c。 */
    const int schema = (int)ver->valuedouble;
    if (schema < 1 || schema > 4) {
        ESP_LOGW(TAG, "unsupported schema_version %d, frame dropped", schema);
        return false;
    }

    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "missing payload object, frame dropped");
        return false;
    }

    double valid_for = 0;
    dst->valid_for_seconds =
        (get_number(root, "valid_for_seconds", &valid_for) && valid_for > 0)
            ? (uint32_t)valid_for : DEFAULT_VALID_FOR;

    /* state 是必填字段：看不懂就整帧丢弃，不要猜成 online */
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(payload, "state");
    if (!cJSON_IsString(state) || state->valuestring == NULL) {
        ESP_LOGW(TAG, "missing payload.state, frame dropped");
        return false;
    }
    if (strcmp(state->valuestring, "offline") == 0) {
        dst->state = DEVICE_STATUS_OFFLINE;
    } else if (strcmp(state->valuestring, "online") == 0) {
        dst->state = DEVICE_STATUS_ONLINE;
    } else {
        /* 协议未来可能加别的状态，未知就显示未知 */
        ESP_LOGW(TAG, "unknown state '%s', treated as unknown", state->valuestring);
        dst->state = DEVICE_STATUS_UNKNOWN;
    }

    const cJSON *host = cJSON_GetObjectItemCaseSensitive(payload, "hostname");
    if (cJSON_IsString(host) && host->valuestring != NULL) {
        snprintf(dst->hostname, sizeof(dst->hostname), "%s", host->valuestring);
    }

    /* ips 是数组，取第一个当展示地址 */
    const cJSON *ips = cJSON_GetObjectItemCaseSensitive(payload, "ips");
    if (cJSON_IsArray(ips)) {
        const cJSON *first = cJSON_GetArrayItem(ips, 0);
        if (cJSON_IsString(first) && first->valuestring != NULL) {
            snprintf(dst->ip, sizeof(dst->ip), "%s", first->valuestring);
        }
    }

    const cJSON *clk = cJSON_GetObjectItemCaseSensitive(payload, "clock_synced");
    if (cJSON_IsBool(clk)) {
        dst->has_clock_flag = true;
        dst->clock_synced = cJSON_IsTrue(clk);
    }

    double up = 0;
    dst->has_uptime = get_number(payload, "uptime_seconds", &up);
    if (dst->has_uptime) {
        dst->uptime_seconds = (uint32_t)up;
    }

    return true;
}

/* ------------------------------------------------------------ 对外接口 */

void device_status_on_message(const char *topic, int topic_len,
                              const char *data, int data_len)
{
    if (!s_initialized || topic == NULL || data == NULL) {
        return;
    }

    /* 主题过滤 + device_id 解析都交给 device_registry：只有恰好四段的
     * hub/v1/devices/{id}/status 才会通过，其余一律忽略。 */
    const int slot = device_registry_slot_from_topic(topic, topic_len, TOPIC_SUFFIX);
    if (slot < 0) {
        return;
    }

    if (data_len <= 0 || data_len > MAX_PAYLOAD) {
        ESP_LOGW(TAG, "payload size %d out of range, dropped", data_len);
        return;
    }

    /* 直接按长度解析 MQTT 缓冲区，不拷贝（esp-mqtt 任务栈只有 6 KB） */
    cJSON *root = cJSON_ParseWithLength(data, (size_t)data_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed (%d bytes), frame dropped", data_len);
        return;
    }

    device_status_t next;
    memset(&next, 0, sizeof(next));

    const bool ok = parse_frame(root, &next);
    cJSON_Delete(root);
    if (!ok) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();

    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        next.frame_count = s_snapshot[slot].frame_count + 1;
        s_snapshot[slot] = next;
        s_last_arrival_us[slot] = now_us;
        xSemaphoreGive(s_lock);
    }

    /* LWT 和每次状态变化都值得打一行；正常心跳只每 60 帧（约 5 分钟）打一次 */
    if (next.state != DEVICE_STATUS_ONLINE || next.frame_count == 1 || next.frame_count % 60 == 0) {
        ESP_LOGI(TAG, "device %d frame #%u  state=%s  host=%s  ip=%s  clock_synced=%d",
                 slot, (unsigned)next.frame_count,
                 next.state == DEVICE_STATUS_ONLINE ? "online" :
                 next.state == DEVICE_STATUS_OFFLINE ? "offline" : "unknown",
                 next.hostname[0] ? next.hostname : "-",
                 next.ip[0] ? next.ip : "-",
                 (int)next.clock_synced);
    }
}

void device_status_get(int slot, device_status_t *out)
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

    if (s_last_arrival_us[slot] > 0) {
        out->ever_received = true;
        const int64_t age_us = esp_timer_get_time() - s_last_arrival_us[slot];
        out->age_ms = (uint32_t)(age_us / 1000);

        /* 只有 online 会过期：offline 是 LWT 给出的确定性结论，
         * 它不需要「刷新」，等到下一条 online 才算恢复。 */
        if (out->state == DEVICE_STATUS_ONLINE &&
            age_us > (int64_t)out->valid_for_seconds * 1000000) {
            out->expired = true;
            out->state = DEVICE_STATUS_UNKNOWN;
        }
    }

    xSemaphoreGive(s_lock);
}

esp_err_t device_status_init(void)
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
