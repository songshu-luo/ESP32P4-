idf_component_register(SRCS "main.c" "app_ui.c" "ui_page_overview.c" "ui_page_monitor.c"
                            "ui_page_placeholder.c"
                            "eth_link.c" "mqtt_link.c" "device_registry.c" "device_metrics.c" "device_status.c"
                            "ota_update.c" "ota_web.c"
                            "fonts/lv_font_cjk_16.c" "fonts/lv_font_cjk_20.c" "fonts/lv_font_cjk_28.c"
                       INCLUDE_DIRS "." "fonts"
                       REQUIRES esp32_p4_function_ev_board esp_lvgl_port esp_eth esp_netif esp_event mqtt json
                                esp_http_server esp_http_client app_update esp_app_format mbedtls
                       PRIV_REQUIRES spi_flash esp_timer esp_psram esp_hw_support lwip)
