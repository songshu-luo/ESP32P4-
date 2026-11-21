/*
 * OTA 更新与回滚（S6）
 *
 * 流程：
 *   1. GET http://192.168.0.156:8080/version.json
 *   2. 比较 manifest.version 与当前 app 版本
 *   3. 流式下载 bin 到另一个 OTA 槽，同时计算 SHA256
 *   4. 校验大小 / SHA256 / esp_ota_end 镜像，再切启动分区并重启
 *   5. 新固件首启为 PENDING_VERIFY，MQTT 首帧到达后标记 valid；
 *      120 秒没有首帧则回滚到旧槽。
 */

#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"

static const char *TAG = "ota_update";

/* 泰山派常驻 HTTP 服务（只读目录 /home/lckfb/esp32-ota） */
#define OTA_MANIFEST_URL        "http://192.168.0.156:8080/version.json"
#define OTA_MANIFEST_MAX_BYTES  (2048)
#define OTA_HTTP_BUF_BYTES      (8192)
#define OTA_TASK_STACK          (8192)
#define OTA_TASK_PRIO           (5)
#define OTA_ROLLBACK_TIMEOUT_US (120000000LL)   /* 120 秒 */
#define OTA_REBOOT_DELAY_MS     (3000)

#define OTA_BIT_CHECK           (1u << 0)
#define OTA_BIT_UPDATE          (1u << 1)

typedef struct {
    char version[48];
    char url[192];
    char sha256[65];
    size_t size;
} ota_manifest_t;

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static ota_status_t s_status;
static bool s_initialized;
static bool s_pending_verify;
static esp_timer_handle_t s_rollback_timer;

/* ------------------------------------------------------------ 状态工具 */

static void copy_string(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

static void status_set(ota_state_t state, const char *message)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_status.state = state;
    copy_string(s_status.message, sizeof(s_status.message), message);
    xSemaphoreGive(s_lock);
}

static void status_set_error(const char *message, const char *error)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_status.state = OTA_STATE_FAILED;
    copy_string(s_status.message, sizeof(s_status.message), message);
    copy_string(s_status.last_error, sizeof(s_status.last_error), error);
    xSemaphoreGive(s_lock);
}

static void status_set_available(const char *version, size_t size)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_status.state = OTA_STATE_UPDATE_AVAILABLE;
    copy_string(s_status.available_version, sizeof(s_status.available_version), version);
    s_status.total = size;
    s_status.downloaded = 0;
    s_status.progress_percent = 0;
    snprintf(s_status.message, sizeof(s_status.message),
             "发现新版本 %s，大小 %u 字节", version, (unsigned)size);
    xSemaphoreGive(s_lock);
}

static void status_progress(size_t downloaded, size_t total)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    s_status.state = OTA_STATE_DOWNLOADING;
    s_status.downloaded = downloaded;
    s_status.total = total;
    int percent = 0;
    if (total > 0) {
        percent = (int)((downloaded * 100ULL) / total);
        if (percent > 99) {
            percent = 99;
        }
    }
    s_status.progress_percent = percent;
    snprintf(s_status.message, sizeof(s_status.message),
             "正在下载 %u / %u 字节", (unsigned)downloaded, (unsigned)total);
    xSemaphoreGive(s_lock);
}

const char *ota_update_state_name(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_IDLE:             return "idle";
    case OTA_STATE_CHECKING:         return "checking";
    case OTA_STATE_UPDATE_AVAILABLE: return "update_available";
    case OTA_STATE_UP_TO_DATE:       return "up_to_date";
    case OTA_STATE_DOWNLOADING:      return "downloading";
    case OTA_STATE_VERIFYING:        return "verifying";
    case OTA_STATE_SUCCESS:          return "success";
    case OTA_STATE_FAILED:           return "failed";
    default:                         return "unknown";
    }
}

void ota_update_get_status(ota_status_t *out)
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
    *out = s_status;
    xSemaphoreGive(s_lock);
}

bool ota_update_is_busy(void)
{
    if (s_lock == NULL) {
        return false;
    }
    bool busy = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        busy = (s_status.state == OTA_STATE_CHECKING ||
                s_status.state == OTA_STATE_DOWNLOADING ||
                s_status.state == OTA_STATE_VERIFYING);
        xSemaphoreGive(s_lock);
    }
    return busy;
}

/* ------------------------------------------------------------ 回滚确认 */

static void rollback_timer_cb(void *arg)
{
    (void)arg;
    bool pending = false;

    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        pending = s_pending_verify;
        if (pending) {
            s_pending_verify = false;
        }
        xSemaphoreGive(s_lock);
    }

    if (!pending) {
        return;
    }

    ESP_LOGE(TAG, "新固件 120 秒内没有收到 MQTT 首帧，回滚到旧槽");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback failed: %s", esp_err_to_name(err));
    esp_restart();
}

void ota_update_on_mqtt_data(void)
{
    bool pending = false;

    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        pending = s_pending_verify;
        if (pending) {
            s_pending_verify = false;
        }
        xSemaphoreGive(s_lock);
    }

    if (!pending) {
        return;
    }

    if (s_rollback_timer != NULL) {
        esp_timer_stop(s_rollback_timer);
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT 首帧到达，新固件已标记 valid");
        status_set(OTA_STATE_IDLE, "新固件已通过 MQTT 首帧确认");
    } else {
        ESP_LOGE(TAG, "标记固件 valid 失败: %s", esp_err_to_name(err));
        status_set_error("标记新固件有效失败", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------ manifest */

static esp_err_t http_get_text(const char *url, char *out, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    if (content_len >= (int64_t)cap) {
        ESP_LOGE(TAG, "manifest too large: %lld", (long long)content_len);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GET %s returned HTTP %d", url, status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total < cap - 1) {
        int r = esp_http_client_read(client, out + total, (int)(cap - 1 - total));
        if (r < 0) {
            ESP_LOGE(TAG, "read manifest failed: %d", r);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    out[total] = '\0';

    if (!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGE(TAG, "manifest read incomplete");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t parse_manifest(const char *json, ota_manifest_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");

    esp_err_t err = ESP_OK;
    if (!cJSON_IsString(version) || version->valuestring[0] == '\0' ||
        !cJSON_IsString(url) || url->valuestring[0] == '\0' ||
        !cJSON_IsString(sha) || strlen(sha->valuestring) != 64) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }

    if (strncmp(url->valuestring, "http://", 7) != 0 &&
        strncmp(url->valuestring, "https://", 8) != 0) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }

    memset(out, 0, sizeof(*out));
    copy_string(out->version, sizeof(out->version), version->valuestring);
    copy_string(out->url, sizeof(out->url), url->valuestring);
    copy_string(out->sha256, sizeof(out->sha256), sha->valuestring);

    if (cJSON_IsNumber(size) && size->valuedouble > 0) {
        if (size->valuedouble > (double)(4 * 1024 * 1024)) {
            err = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        out->size = (size_t)size->valuedouble;
    }

done:
    cJSON_Delete(root);
    return err;
}

static esp_err_t fetch_manifest(ota_manifest_t *out)
{
    char json[OTA_MANIFEST_MAX_BYTES];
    esp_err_t err = http_get_text(OTA_MANIFEST_URL, json, sizeof(json));
    if (err != ESP_OK) {
        return err;
    }
    return parse_manifest(json, out);
}

/* ------------------------------------------------------------ 下载与切槽 */

static bool sha256_matches(const char *expected, const uint8_t digest[32])
{
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return strcasecmp(hex, expected) == 0;
}

static esp_err_t download_and_install(const ota_manifest_t *manifest)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (manifest->size == 0 || manifest->size > update->size) {
        ESP_LOGE(TAG, "固件大小异常: %u, 槽大小 %u",
                 (unsigned)manifest->size, (unsigned)update->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, manifest->size, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_http_client_config_t cfg = {
        .url = manifest->url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        esp_ota_abort(handle);
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open firmware failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        esp_ota_abort(handle);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GET firmware returned HTTP %d", status);
        esp_http_client_cleanup(client);
        esp_ota_abort(handle);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_HTTP_BUF_BYTES);
    if (buf == NULL) {
        esp_http_client_cleanup(client);
        esp_ota_abort(handle);
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    size_t downloaded = 0;
    size_t total = manifest->size;
    if (total == 0 && content_len > 0) {
        total = (size_t)content_len;
    }
    bool failed = false;

    while (true) {
        int r = esp_http_client_read(client, (char *)buf, OTA_HTTP_BUF_BYTES);
        if (r < 0) {
            ESP_LOGE(TAG, "firmware read failed: %d", r);
            failed = true;
            break;
        }
        if (r == 0) {
            break;
        }
        if (downloaded + (size_t)r > update->size) {
            ESP_LOGE(TAG, "固件超过 OTA 槽容量");
            failed = true;
            break;
        }

        err = esp_ota_write(handle, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }

        mbedtls_sha256_update(&sha, buf, (size_t)r);
        downloaded += (size_t)r;
        status_progress(downloaded, total);
    }

    free(buf);

    if (!failed && !esp_http_client_is_complete_data_received(client)) {
        ESP_LOGE(TAG, "firmware download incomplete");
        failed = true;
    }
    if (!failed && manifest->size > 0 && downloaded != manifest->size) {
        ESP_LOGE(TAG, "固件大小不匹配: got %u expected %u",
                 (unsigned)downloaded, (unsigned)manifest->size);
        failed = true;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (!failed && !sha256_matches(manifest->sha256, digest)) {
        ESP_LOGE(TAG, "SHA256 不匹配");
        failed = true;
    }

    esp_http_client_cleanup(client);

    if (failed) {
        esp_ota_abort(handle);
        return ESP_FAIL;
    }

    status_set(OTA_STATE_VERIFYING, "下载完成，正在校验镜像");
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA 写入完成，切换到槽 %s，准备重启", update->label);
    status_set(OTA_STATE_SUCCESS, "升级成功，正在重启");
    return ESP_OK;
}

/* ------------------------------------------------------------ 任务 */

static void ota_do_check(void)
{
    status_set(OTA_STATE_CHECKING, "正在检查版本");
    ota_manifest_t manifest;
    esp_err_t err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        status_set_error("检查版本失败", esp_err_to_name(err));
        return;
    }

    ota_status_t current;
    ota_update_get_status(&current);
    if (strcmp(manifest.version, current.current_version) == 0) {
        status_set(OTA_STATE_UP_TO_DATE, "已经是最新版本");
        return;
    }

    status_set_available(manifest.version, manifest.size);
}

static void ota_do_update(void)
{
    if (s_pending_verify) {
        status_set_error("当前新固件还未确认", "等待 MQTT 首帧后再升级");
        return;
    }

    status_set(OTA_STATE_CHECKING, "正在获取版本信息");
    ota_manifest_t manifest;
    esp_err_t err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        status_set_error("获取版本信息失败", esp_err_to_name(err));
        return;
    }

    ota_status_t current;
    ota_update_get_status(&current);
    if (strcmp(manifest.version, current.current_version) == 0) {
        status_set(OTA_STATE_UP_TO_DATE, "已经是最新版本");
        return;
    }

    status_set_available(manifest.version, manifest.size);
    status_set(OTA_STATE_DOWNLOADING, "开始下载固件");

    err = download_and_install(&manifest);
    if (err != ESP_OK) {
        status_set_error("固件升级失败", esp_err_to_name(err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}

static void ota_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events, OTA_BIT_CHECK | OTA_BIT_UPDATE,
                                              pdTRUE, pdFALSE, portMAX_DELAY);
        if ((bits & OTA_BIT_UPDATE) != 0) {
            ota_do_update();
        } else if ((bits & OTA_BIT_CHECK) != 0) {
            ota_do_check();
        }
    }
}

esp_err_t ota_update_request_check(void)
{
    if (!s_initialized || s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ota_update_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_events, OTA_BIT_UPDATE);
    xEventGroupSetBits(s_events, OTA_BIT_CHECK);
    return ESP_OK;
}

esp_err_t ota_update_request_update(void)
{
    if (!s_initialized || s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ota_update_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_events, OTA_BIT_CHECK);
    xEventGroupSetBits(s_events, OTA_BIT_UPDATE);
    return ESP_OK;
}

esp_err_t ota_update_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = OTA_STATE_IDLE;
    copy_string(s_status.current_version, sizeof(s_status.current_version),
                esp_app_get_description()->version);
    copy_string(s_status.message, sizeof(s_status.message), "OTA 就绪");

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;

        esp_timer_create_args_t timer_args = {
            .callback = rollback_timer_cb,
            .arg = NULL,
            .name = "ota_rollback",
        };
        if (esp_timer_create(&timer_args, &s_rollback_timer) == ESP_OK) {
            esp_timer_start_once(s_rollback_timer, OTA_ROLLBACK_TIMEOUT_US);
            ESP_LOGW(TAG, "新固件处于 PENDING_VERIFY，120 秒内等待 MQTT 首帧");
        } else {
            ESP_LOGE(TAG, "创建回滚定时器失败");
        }
        status_set(OTA_STATE_IDLE, "新固件待确认：等待 MQTT 首帧");
    } else {
        ESP_LOGI(TAG, "当前运行分区不是 PENDING_VERIFY，无需回滚确认");
    }

    BaseType_t ok = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK, NULL,
                                OTA_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OTA 就绪，当前版本 %s", s_status.current_version);
    return ESP_OK;
}
