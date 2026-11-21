/*
 * OTA 更新与回滚（S6）
 *
 * 设计约束：
 *   - 不阻塞 UI / 网络启动；检查与下载都在独立任务里完成；
 *   - 固件从泰山派 HTTP 服务拉取，先校验 manifest 与 SHA256，再切槽；
 *   - 新槽首次启动为 PENDING_VERIFY，MQTT 收到首帧后标记 valid；
 *   - 120 秒内没有首帧则调用 esp_ota_mark_app_invalid_rollback_and_reboot()。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_UPDATE_AVAILABLE,
    OTA_STATE_UP_TO_DATE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current_version[48];
    char available_version[48];
    char message[128];
    char last_error[128];
    size_t downloaded;
    size_t total;
    int progress_percent;
} ota_status_t;

/** 初始化：记录当前版本、创建任务/事件组；若当前槽为 PENDING_VERIFY 则启动回滚计时。 */
esp_err_t ota_update_init(void);

/** 取一份状态快照（任意任务可调用）。 */
void ota_update_get_status(ota_status_t *out);

/** 请求检查版本（非阻塞）。 */
esp_err_t ota_update_request_check(void);

/** 请求下载并升级（非阻塞）。 */
esp_err_t ota_update_request_update(void);

/** MQTT 收到任意数据时调用：首次数据到达即确认新固件有效。 */
void ota_update_on_mqtt_data(void);

/** 是否正在检查/下载/校验。 */
bool ota_update_is_busy(void);

/** 状态枚举转字符串，供网页显示。 */
const char *ota_update_state_name(ota_state_t state);
