/*
 * 以太网链路（S1）
 *
 * 板载 PHY 为 IP101GRI，经 ESP32-P4 内置 EMAC 的 RMII 接口连到 RJ45（10/100M）。
 * 引脚对照来自官方原理图 v1.5.2 第 2 页（P4 引脚）与第 5 页（PHY），与 IDF 对
 * ESP32-P4 的默认值一致。
 *
 * 设计约束（方案 §6.3「启动不得阻塞在网络」）：
 * eth_link_start_async() 只建任务就返回，PHY 初始化 / 自协商 / DHCP 全在后台完成；
 * UI 线程只调用 eth_link_get_status() 取只读快照。
 */

#include "eth_link.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"

static const char *TAG = "eth_link";

/* ------------------------------------------------------------ 硬件引脚映射 */

/* 原理图 v1.5.2 实测对照（RMII 数据线走 IO_MUX，不是 GPIO Matrix） */
#define ETH_MDC_GPIO        (31)
#define ETH_MDIO_GPIO       (52)
#define ETH_PHY_RST_GPIO    (51)    /* PHY_RSTN，低有效 */
#define ETH_RMII_CLK_GPIO   (50)    /* IP101 送 50MHz REF_CLK 给 P4 */
#define ETH_TX_EN_GPIO      (49)
#define ETH_TXD0_GPIO       (34)
#define ETH_TXD1_GPIO       (35)
#define ETH_CRS_DV_GPIO     (28)
#define ETH_RXD0_GPIO       (29)
#define ETH_RXD1_GPIO       (30)

/* 原理图上 PHY_AD0 上拉、PHY_AD3 接地 → 地址 0b0001 */
#define ETH_PHY_ADDR        (1)

/* ------------------------------------------------------------ 状态存放 */

#define ETH_TASK_STACK      (4096)
#define ETH_TASK_PRIO       (5)

static SemaphoreHandle_t  s_lock;
static eth_link_status_t  s_status;
static esp_eth_handle_t   s_eth = NULL;
static esp_netif_t       *s_netif = NULL;

void eth_link_get_status(eth_link_status_t *out)
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

/** 带锁更新状态：所有写操作都走这里，避免 UI 读到半截字符串 */
static void status_update(void (*fn)(eth_link_status_t *st, const void *arg), const void *arg)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    fn(&s_status, arg);
    xSemaphoreGive(s_lock);
}

/* ---------- 各种状态的写入器（都是小函数，避免在回调里写字面量代码） ---------- */

static void write_state(eth_link_status_t *st, const void *arg)
{
    st->state = *(const eth_link_state_t *)arg;
}

static void set_state(eth_link_state_t state)
{
    status_update(write_state, &state);
}

typedef struct {
    const char *msg;
} error_arg_t;

static void write_error(eth_link_status_t *st, const void *arg)
{
    const error_arg_t *e = (const error_arg_t *)arg;
    st->state = ETH_STATE_ERROR;
    snprintf(st->error, sizeof(st->error), "%s", e->msg);
}

static void set_error(const char *msg)
{
    error_arg_t arg = { .msg = msg };
    status_update(write_error, &arg);
}

typedef struct {
    uint32_t speed_mbps;
    bool     full_duplex;
} link_arg_t;

static void write_link(eth_link_status_t *st, const void *arg)
{
    const link_arg_t *l = (const link_arg_t *)arg;
    st->state = ETH_STATE_LINK_UP;
    st->speed_mbps = l->speed_mbps;
    st->full_duplex = l->full_duplex;
}

static void write_link_down(eth_link_status_t *st, const void *arg)
{
    (void)arg;
    st->state = ETH_STATE_LINK_DOWN;
    st->speed_mbps = 0;
    st->full_duplex = false;
    st->ip[0] = '\0';
    st->gw[0] = '\0';
    st->netmask[0] = '\0';
}

static void write_mac(eth_link_status_t *st, const void *arg)
{
    memcpy(st->mac, arg, sizeof(st->mac));
}

typedef struct {
    char ip[16];
    char gw[16];
    char mask[16];
} ip_arg_t;

static void write_ip(eth_link_status_t *st, const void *arg)
{
    const ip_arg_t *p = (const ip_arg_t *)arg;
    snprintf(st->ip, sizeof(st->ip), "%s", p->ip);
    snprintf(st->gw, sizeof(st->gw), "%s", p->gw);
    snprintf(st->netmask, sizeof(st->netmask), "%s", p->mask);
}

static void write_no_ip(eth_link_status_t *st, const void *arg)
{
    (void)arg;
    st->ip[0] = '\0';
    st->gw[0] = '\0';
    st->netmask[0] = '\0';
}

/* ------------------------------------------------------------ 事件回调 */

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    esp_eth_handle_t hdl = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "driver started, waiting for link");
        set_state(ETH_STATE_DETECTING);
        break;

    case ETHERNET_EVENT_CONNECTED: {
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(hdl, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(hdl, ETH_CMD_G_DUPLEX_MODE, &duplex);

        ESP_LOGI(TAG, "link up: %s, %s duplex",
                 speed == ETH_SPEED_100M ? "100 Mbps" : "10 Mbps",
                 duplex == ETH_DUPLEX_FULL ? "full" : "half");

        link_arg_t la = {
            .speed_mbps = (speed == ETH_SPEED_100M) ? 100 : 10,
            .full_duplex = (duplex == ETH_DUPLEX_FULL),
        };
        status_update(write_link, &la);
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        status_update(write_link_down, NULL);
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "driver stopped");
        break;

    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip " IPSTR "  gw " IPSTR "  mask " IPSTR,
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), IP2STR(&ev->ip_info.netmask));

    ip_arg_t a = {0};
    snprintf(a.ip, sizeof(a.ip), IPSTR, IP2STR(&ev->ip_info.ip));
    snprintf(a.gw, sizeof(a.gw), IPSTR, IP2STR(&ev->ip_info.gw));
    snprintf(a.mask, sizeof(a.mask), IPSTR, IP2STR(&ev->ip_info.netmask));
    status_update(write_ip, &a);
}

static void lost_ip_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)event_data;
    ESP_LOGW(TAG, "lost ip");
    status_update(write_no_ip, NULL);
}

/* ------------------------------------------------------------ 驱动初始化 */

static esp_err_t eth_link_init(void)
{
    /* netif 与默认事件循环只允许初始化一次；重复调用返回 INVALID_STATE，视为正常 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    /* --- MAC --- */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    /* IP101 自带 25MHz 晶振并输出 50MHz REF_CLK 给 P4，所以是外部时钟输入 */
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.tx_en_num  = ETH_TX_EN_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.txd0_num   = ETH_TXD0_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.txd1_num   = ETH_TXD1_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.crs_dv_num = ETH_CRS_DV_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.rxd0_num   = ETH_RXD0_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.rxd1_num   = ETH_RXD1_GPIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (mac == NULL) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 failed - RMII GPIO/clock config rejected");
        return ESP_FAIL;
    }

    /* --- PHY --- */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;
    /* 原理图要求 PHY_RSTN 拉低至少 10ms；置 0 让 IP101 驱动填芯片专用默认值 */
    phy_cfg.hw_reset_assert_time_us = 0;
    phy_cfg.post_hw_reset_delay_ms = 0;

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (phy == NULL) {
        ESP_LOGE(TAG, "esp_eth_phy_new_ip101 failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    /* --- 驱动 --- */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    eth_cfg.check_link_period_ms = 1000;
    err = esp_eth_driver_install(&eth_cfg, &s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        return err;
    }

    uint8_t mac_addr[6] = {0};
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac_addr);
    status_update(write_mac, mac_addr);

    /* --- 网络接口（DHCP 客户端） --- */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth);
    err = esp_netif_attach(s_netif, glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_set_default_netif(s_netif);

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, lost_ip_handler, NULL));

    ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x  IP101 phy_addr=%d  RMII clk in GPIO%d",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
             ETH_PHY_ADDR, ETH_RMII_CLK_GPIO);

    return esp_eth_start(s_eth);
}

static void eth_task(void *arg)
{
    (void)arg;
    set_state(ETH_STATE_STARTING);

    esp_err_t err = eth_link_init();
    if (err != ESP_OK) {
        set_error(esp_err_to_name(err));
    }
    /* 驱动与事件回调接管后续状态，本任务可以退出 */
    vTaskDelete(NULL);
}

esp_err_t eth_link_start_async(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = ETH_STATE_IDLE;

    BaseType_t ok = xTaskCreate(eth_task, "eth_link", ETH_TASK_STACK, NULL, ETH_TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
