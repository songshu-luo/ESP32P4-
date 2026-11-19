/*
 * MQTT 链路（S2-a）
 *
 * 目标：让 ESP32 连上泰山派 Hub，并把「连接到了哪一步」做成屏幕上的可见现象。
 * 本阶段**不解析任何业务载荷**，只关心连接、鉴权、订阅是否成立。
 *
 * 设计约束（与 eth_link.c 一致，对应方案 §6.3「启动不得阻塞在网络」）：
 *   - start_async() 只建任务就返回；
 *   - 解析主机名、TCP 连接、重连全部在后台任务里；
 *   - UI 线程只调 mqtt_link_get_status() 取互斥保护快照。
 *
 * 地址解析策略（泰山派没做 DHCP 保留，所以按顺序尝试）：
 *   1. Kconfig 里的 mDNS 名      squirrel-lckfb.local   ← 用户选定
 *   2. 同一主机的裸名（路由器 DHCP 注册） squirrel-lckfb
 *   3. 写死的回退 IP             192.168.0.156
 * 解析用 lwIP 的 getaddrinfo()：IDF 的 CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES 会对
 * 以 ".local" 结尾的名字发一次性组播查询，收到 A 记录即返回，无需 avahi 客户端。
 *
 * 重连由本模块自己管（关闭 esp-mqtt 的 auto reconnect），因为每次重连前都可能
 * 需要重新解析——DHCP 换了地址后一直连旧 IP 是查不出来的。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** MQTT 连接状态机 */
typedef enum {
    MQTT_LINK_IDLE = 0,      /*!< 还没启动 */
    MQTT_LINK_WAIT_IP,       /*!< 以太网还没有 IP */
    MQTT_LINK_RESOLVING,     /*!< 正在解析 broker 主机名 */
    MQTT_LINK_CONNECTING,    /*!< 已解析，正在 TCP/MQTT 握手 */
    MQTT_LINK_SUBSCRIBING,   /*!< 已连上，正在等 SUBACK */
    MQTT_LINK_ONLINE,        /*!< 订阅成功，链路可用 */
    MQTT_LINK_DISCONNECTED,  /*!< 连上过又断了，准备重连 */
    MQTT_LINK_AUTH_FAIL,     /*!< Broker 明确拒绝凭据 */
    MQTT_LINK_RESOLVE_FAIL,  /*!< 所有候选地址都解析不到 */
    MQTT_LINK_ERROR,         /*!< 其他失败，detail 里有原因 */
} mqtt_link_state_t;

/** 供 UI 轮询的快照，互斥保护 */
typedef struct {
    mqtt_link_state_t state;
    char     host[40];      /*!< 本次使用的 broker 主机名 */
    char     ip[16];        /*!< 解析结果，空串表示未解析 */
    char     detail[48];    /*!< 一行补充说明 */

    /* 按主题分类计数：只看总数分不清是 metrics 停了还是 status 停了 */
    uint32_t msg_count;         /*!< 全部消息 */
    uint32_t metrics_count;     /*!< 以 /metrics 结尾的主题 */
    uint32_t status_count;      /*!< 以 /status 结尾的主题 */
} mqtt_link_status_t;

/**
 * @brief MQTT 数据回调
 *
 * 在 esp-mqtt 的任务里被调用，**不是** LVGL 线程。回调实现不得阻塞、
 * 不得碰 LVGL 对象。topic 与 data 都没有 NUL 结尾，长度单独给出。
 */
typedef void (*mqtt_link_data_cb_t)(const char *topic, int topic_len,
                                    const char *data, int data_len);

/** 后台启动 MQTT 链路（非阻塞） */
esp_err_t mqtt_link_start_async(void);

/**
 * @brief 注册数据回调（可在 start 前或后调用，最多 MQTT_MAX_DATA_CBS 个）
 *
 * 回调在 esp-mqtt 任务上下文中执行；各解析模块（metrics / status）各自注册一个，
 * 由模块自己按主题过滤，mqtt_link 不做分发策略。
 */
void mqtt_link_add_data_cb(mqtt_link_data_cb_t cb);

/** 取一份状态快照（任意任务可调用） */
void mqtt_link_get_status(mqtt_link_status_t *out);
