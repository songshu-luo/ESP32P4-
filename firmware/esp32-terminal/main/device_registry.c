/*
 * 设备索引（S2-e）
 *
 * 只负责 device_id → 槽位号的映射，不存任何指标数据。
 * 指标和状态各有一份按槽位排布的数组，两边用同一个下标即代表同一台设备。
 *
 * 为什么不直接把 device_id 当键在 UI 里查：
 *   1. UI 每 500 ms 轮询一次，字符串比较 × 设备数 × 页面数没必要；
 *   2. 卡片布局需要「稳定顺序」，槽位一旦分配就不再变；
 *   3. 监控页上下翻页本质上就是在遍历这个槽位表。
 */

#include "device_registry.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

static const char *TAG = "dev_reg";

/* 显示名表：device_id 是协议键，中文名只在这里做一次翻译。
 * 新增设备时在这里加一行即可；没登记的 id 直接显示原始 id（ASCII，屏幕上能看清）。
 * 注意：改了这里的中文就要重跑 tools/gen_cjk_font.sh。 */
typedef struct {
    const char *id;
    const char *name;
} known_device_t;

static const known_device_t KNOWN_DEVICES[] = {
    { "taishan-3m",   "泰山派 3M" },
    { "windows-main", "Windows 主机" },
    { "cloud-main",   "云服务器" },
};

#define KNOWN_COUNT ((int)(sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0])))

static SemaphoreHandle_t s_lock;
static device_registry_entry_t s_slots[DEVICE_REGISTRY_MAX_DEVICES];
static int  s_count;
static bool s_initialized;

/* ------------------------------------------------------------ 校验 */

/** 协议 §1：device_id 只允许 [a-z0-9-]，长度 1..31 */
static bool id_is_valid(const char *id, int len)
{
    if (len <= 0 || len >= DEVICE_REGISTRY_ID_LEN) {
        return false;
    }
    for (int i = 0; i < len; i++) {
        const char c = id[i];
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ 对外接口 */

int device_registry_slot(const char *id, int id_len)
{
    if (!s_initialized || id == NULL || !id_is_valid(id, id_len)) {
        return -1;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if ((int)strlen(s_slots[i].id) == id_len &&
            memcmp(s_slots[i].id, id, (size_t)id_len) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (s_count >= DEVICE_REGISTRY_MAX_DEVICES) {
            xSemaphoreGive(s_lock);
            /* 槽位满了只告警不崩溃：多出来的设备不上屏，已有设备照常刷新。
             * 真要接第 5 台，改 DEVICE_REGISTRY_MAX_DEVICES 并重排总览页布局。 */
            ESP_LOGW(TAG, "registry full (%d), device '%.*s' ignored",
                     DEVICE_REGISTRY_MAX_DEVICES, id_len, id);
            return -1;
        }
        slot = s_count++;
        memcpy(s_slots[slot].id, id, (size_t)id_len);
        s_slots[slot].id[id_len] = '\0';

        const char *name = NULL;
        for (int i = 0; i < KNOWN_COUNT; i++) {
            if (strcmp(KNOWN_DEVICES[i].id, s_slots[slot].id) == 0) {
                name = KNOWN_DEVICES[i].name;
                break;
            }
        }
        if (name != NULL) {
            snprintf(s_slots[slot].name, sizeof(s_slots[slot].name), "%s", name);
        } else {
            s_slots[slot].name[0] = '\0';   /* 空 = 未登记，UI 显示 id */
        }

        ESP_LOGI(TAG, "slot %d = '%s' (%s)", slot, s_slots[slot].id,
                 s_slots[slot].name[0] ? s_slots[slot].name : "未登记");
    }

    xSemaphoreGive(s_lock);
    return slot;
}

int device_registry_slot_from_topic(const char *topic, int topic_len,
                                    const char *suffix)
{
    static const char PREFIX[] = "hub/v1/devices/";
    const int prefix_len = (int)sizeof(PREFIX) - 1;

    if (topic == NULL || suffix == NULL || topic_len <= 0) {
        return -1;
    }
    if (topic_len < prefix_len ||
        memcmp(topic, PREFIX, (size_t)prefix_len) != 0) {
        return -1;
    }

    const int suffix_len = (int)strlen(suffix);
    const int id_len = topic_len - prefix_len - 1 - suffix_len;   /* -1 = 中间的 '/' */
    if (id_len <= 0 || topic[prefix_len + id_len] != '/' ||
        memcmp(topic + prefix_len + id_len + 1, suffix, (size_t)suffix_len) != 0) {
        return -1;
    }
    return device_registry_slot(topic + prefix_len, id_len);
}

int device_registry_count(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    const int n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

void device_registry_get(int slot, device_registry_entry_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (slot < 0 || s_lock == NULL ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (slot < s_count) {
        *out = s_slots[slot];
    }
    xSemaphoreGive(s_lock);
}

esp_err_t device_registry_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_slots, 0, sizeof(s_slots));
        s_count = 0;
        xSemaphoreGive(s_lock);
    }
    s_initialized = true;
    return ESP_OK;
}