/*
 * ESP32-P4 智能终端 —— 基线固件
 *
 * 当前已完成的阶段：
 *   S0 屏幕点亮 + GT911 触摸 + 背光
 *   S1 以太网（IP101GRI / EMAC RMII）后台启动并显示 DHCP 地址
 *   S2-a MQTT 接入：解析 squirrel-lckfb.local → 连接 → 订阅
 *   S2-b 把 hub/v1/devices/taishan-3m/metrics 解析成屏幕上的指标卡
 *
 * 不依赖 PC，符合方案 §6.3「启动不得阻塞在网络」：
 * 网络与 MQTT 在独立任务里初始化，主流程只负责把 UI 建好。
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

#include "bsp/esp-bsp.h"

#include "app_ui.h"
#include "device_metrics.h"
#include "device_status.h"
#include "eth_link.h"
#include "mqtt_link.h"

static const char *TAG = "app_main";

/* 背光默认亮度（%）。调低便于长时间调试时观察，也便于判断亮度接口是否生效。 */
#define APP_BACKLIGHT_PERCENT   (80)

static void log_board_info(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    const esp_err_t flash_err = esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "================ board info ================");
    ESP_LOGI(TAG, "chip      : ESP32-P4  rev v%u.%u  cores %u",
             (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100),
             (unsigned)chip.cores);
    if (flash_err == ESP_OK) {
        ESP_LOGI(TAG, "flash     : %u MB", (unsigned)(flash_size / (1024 * 1024)));
    } else {
        ESP_LOGI(TAG, "flash     : size query failed (%s)", esp_err_to_name(flash_err));
    }
    ESP_LOGI(TAG, "psram     : %u MB", (unsigned)(esp_psram_get_size() / (1024 * 1024)));
    ESP_LOGI(TAG, "lcd       : %dx%d  EK79007  touch GT911",
             (int)BSP_LCD_H_RES, (int)BSP_LCD_V_RES);
    ESP_LOGI(TAG, "============================================");
}

void app_main(void)
{
    log_board_info();

    ESP_LOGI(TAG, "starting LVGL display");
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        /* 这里失败通常是 MIPI 排线、屏供电或 BSP 面板类型配置问题。
         * 不 assert，让串口日志能完整打印出来。 */
        ESP_LOGE(TAG, "bsp_display_start() failed - check MIPI FPC, LCD 5V and panel type");
        return;
    }

    ESP_LOGI(TAG, "building UI");
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "cannot take LVGL lock");
        return;
    }
    app_ui_build(disp);
    bsp_display_unlock();

    /* 触摸句柄自检：TAP ME 能响应说明触摸驱动已工作，
     * 这里只把句柄是否被 BSP 记录下来打到日志，便于排查。 */
    lv_indev_t *indev = bsp_display_get_input_dev();
    ESP_LOGI(TAG, "touch input device: %s", indev ? "registered" : "MISSING");

    /* 先建界面再开背光：避免用户先看到一片未初始化的花屏 */
    ESP_ERROR_CHECK(bsp_display_brightness_set(APP_BACKLIGHT_PERCENT));

    /* 网络在后台任务里起来，屏幕立刻可用；IP 会在界面底部自动出现 */
    esp_err_t err = eth_link_start_async();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ethernet start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ethernet starting in background (RJ45 needed for link)");
    }

    /* MQTT 也自己等以太网就绪，所以这里紧接着启动即可，不会互相阻塞 */
    err = mqtt_link_start_async();
    if (err == ESP_OK) {
        /* 两个解析器都必须在 MQTT 开始收包之前就绪：
         * on_message 在未初始化时会直接丢弃消息，而不是缓存。 */
        const esp_err_t m_err = device_metrics_init();
        const esp_err_t s_err = device_status_init();
        if (m_err != ESP_OK || s_err != ESP_OK) {
            ESP_LOGE(TAG, "parser init failed: metrics=%s status=%s",
                     esp_err_to_name(m_err), esp_err_to_name(s_err));
            err = (m_err != ESP_OK) ? m_err : s_err;
        } else {
            mqtt_link_add_data_cb(device_metrics_on_message);
            mqtt_link_add_data_cb(device_status_on_message);
            ESP_LOGI(TAG, "parsers ready: metrics + status hooked to mqtt data events");
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mqtt link starting in background");
    }

    ESP_LOGI(TAG, "S2-c status lamp ready - backlight %d%%", APP_BACKLIGHT_PERCENT);
}
