/*
 * ESP32 OTA 网页（S6）
 *
 * 只在以太网拿到 IP 后启动，避免 DHCP 尚未完成时 httpd 绑定失败。
 * 页面地址：http://192.168.0.100/
 */

#pragma once

#include "esp_err.h"

/** 后台等待 IP 并启动 HTTP 服务（非阻塞）。 */
esp_err_t ota_web_start_async(void);
