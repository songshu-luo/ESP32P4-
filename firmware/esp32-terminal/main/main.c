/*
 * ESP32-P4 智能终端 —— 基线固件
 *
 * 当前已完成的阶段：
 *   S0 屏幕点亮 + GT911 触摸 + 背光
 *   S1 以太网（IP101GRI / EMAC RMII）后台启动并显示 DHCP 地址
 *   S2-a MQTT 接入：解析 squirrel-lckfb.local → 连接 → 订阅
 *   S2-b 把 hub/v1/devices/{id}/metrics 解析成屏幕上的指标卡
 *   S2-e 多设备：device_registry 槽位表，总览页逐设备显示
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
#include "esp_sntp.h"

#include "bsp/esp-bsp.h"

#include <stdlib.h>
#include <time.h>

#include "app_settings.h"
#include "app_ui.h"
#include "background.h"
#include "ui_screensaver.h"
#include "wall_sd.h"
#include "device_metrics.h"
#include "device_registry.h"
#include "device_status.h"
#include "eth_link.h"
#include "log_ring.h"
#include "mqtt_link.h"
#include "ota_update.h"
#include "ota_web.h"

static const char *TAG = "app_main";

/* 背光默认亮度（%）。调低便于长时间调试时观察，也便于判断亮度接口是否生效。 */
#define APP_BACKLIGHT_PERCENT   (80)

/* 本机时区：CST-8 = 中国标准时间 UTC+8（POSIX TZ 的符号与直觉相反，负号表示东八区） */
#define APP_TZ              "CST-8"
/* 局域网里的 Hub（泰山派）同时跑 chrony/ntpd，用它当首选 NTP 服务器，不依赖外网 */
#define APP_NTP_SERVER      "192.168.0.156"

/**
 * 启动 SNTP 对时（非阻塞）。
 *
 * 只做三件事：设时区、设服务器、开轮询。esp_sntp_init() 起的是 lwip 里的
 * 后台任务，不会等待第一次同步完成，所以放在 UI 起来之后调用也不影响启动。
 * 屏幕上的时钟卡自己判断 time() 是否已过 2023-11，没对上就显示「对时中」。
 */
static void sntp_start(void)
{
    setenv("TZ", APP_TZ, 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, APP_NTP_SERVER);
    esp_sntp_init();

    ESP_LOGI(TAG, "sntp polling started, server %s, tz %s", APP_NTP_SERVER, APP_TZ);
}

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

static void ota_mqtt_data_cb(const char *topic, int topic_len,
                             const char *data, int data_len)
{
    (void)topic;
    (void)topic_len;
    (void)data;
    (void)data_len;
    ota_update_on_mqtt_data();
}

void app_main(void)
{
    /* 日志钩子必须最先安装：log_board_info() 的板级信息也要进环形缓冲，
     * 设置页的「诊断日志」才有启动早期的内容可看。 */
    log_ring_init();

    log_board_info();

    ESP_LOGI(TAG, "starting LVGL display");
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        /* 这里失败通常是 MIPI 排线、屏供电或 BSP 面板类型配置问题。
         * 不 assert，让串口日志能完整打印出来。 */
        ESP_LOGE(TAG, "bsp_display_start() failed - check MIPI FPC, LCD 5V and panel type");
        return;
    }

    /* 设置页与 status_poll_cb 都要读这些状态，必须在建界面之前初始化 */
    esp_err_t settings_err = app_settings_init();
    if (settings_err != ESP_OK) {
        ESP_LOGE(TAG, "app settings init failed: %s", esp_err_to_name(settings_err));
    }

    ESP_LOGI(TAG, "building UI");
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "cannot take LVGL lock");
        return;
    }
    app_ui_build(disp);
    /* 休眠壁纸层：LVGL 锁区内、页面建好之后创建，默认隐藏 */
    ui_screensaver_init();
    bsp_display_unlock();

    /* 放在解锁后，避免 init 里的 SD 挂载/解码长时间占着 LVGL 锁 */
    wall_sd_init();

    /* 主界面背景图：wall_sd_init() 已经把 SD 挂好，这里只读 /sdcard/bg.jpg。
     * 失败（没卡 / 没图 / 解码失败）不致命，背景层保持纯色底。 */
    background_init();

    /* UI 是在 SD 挂载之前建好的，那时 background_dsc() 还是 NULL；
     * 解码完成后在锁内把图补挂到背景层上。 */
    if (bsp_display_lock(0)) {
        app_ui_background_refresh();
        bsp_display_unlock();
    }

    /* 触摸句柄自检：TAP ME 能响应说明触摸驱动已工作，
     * 这里只把句柄是否被 BSP 记录下来打到日志，便于排查。 */
    lv_indev_t *indev = bsp_display_get_input_dev();
    ESP_LOGI(TAG, "touch input device: %s", indev ? "registered" : "MISSING");

    /* 先建界面再开背光：避免用户先看到一片未初始化的花屏 */
    ESP_ERROR_CHECK(bsp_display_brightness_set(APP_BACKLIGHT_PERCENT));

    /* OTA 状态机必须在 MQTT 开始收包前初始化，否则首帧无法确认新固件 */
    esp_err_t ota_err = ota_update_init();
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "ota update init failed: %s", esp_err_to_name(ota_err));
    }

    /* 网络在后台任务里起来，屏幕立刻可用；IP 会在界面底部自动出现 */
    esp_err_t err = eth_link_start_async();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ethernet start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ethernet starting in background (RJ45 needed for link)");
    }

    /* OTA 网页也等以太网拿到 IP 后再启动，不阻塞这里 */
    ota_err = ota_web_start_async();
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "ota web start failed: %s", esp_err_to_name(ota_err));
    }

    /* SNTP 依赖 lwip 网络栈（已在以太网启动前初始化），这里只是开一个后台轮询，
     * 不等待同步完成，因此不会拖慢后面的 UI 或 MQTT 启动。 */
    sntp_start();

    /* MQTT 也自己等以太网就绪，所以这里紧接着启动即可，不会互相阻塞 */
    err = mqtt_link_start_async();
    if (err == ESP_OK) {
        /* 三个模块都必须在 MQTT 开始收包之前就绪：
         * registry 先分配槽位表，metrics/status 才能把消息按槽位存放；
         * on_message 在未初始化时会直接丢弃消息，而不是缓存。 */
        const esp_err_t r_err = device_registry_init();
        const esp_err_t m_err = device_metrics_init();
        const esp_err_t s_err = device_status_init();
        if (r_err != ESP_OK || m_err != ESP_OK || s_err != ESP_OK) {
            ESP_LOGE(TAG, "parser init failed: registry=%s metrics=%s status=%s",
                     esp_err_to_name(r_err), esp_err_to_name(m_err),
                     esp_err_to_name(s_err));
            err = (r_err != ESP_OK) ? r_err : (m_err != ESP_OK) ? m_err : s_err;
        } else {
            mqtt_link_add_data_cb(device_metrics_on_message);
            mqtt_link_add_data_cb(device_status_on_message);
            mqtt_link_add_data_cb(ota_mqtt_data_cb);
            ESP_LOGI(TAG, "parsers ready: registry + metrics + status hooked to mqtt data events");
            ESP_LOGI(TAG, "ota rollback probe hooked to first mqtt data");
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mqtt link starting in background");
    }

    ESP_LOGI(TAG, "S2-e multi-device ready - backlight %d%%", APP_BACKLIGHT_PERCENT);
}
