/*
 * 以太网链路（S1）
 *
 * 板载 PHY 是 IP101GRI，走 ESP32-P4 内置 EMAC 的 RMII 接口，接 RJ45（10/100M）。
 *
 * 设计约束（对应方案 §6.3）：启动不得阻塞在网络。
 * 所以 eth_link_start_async() 只创建任务后立即返回，PHY 初始化、链路协商、
 * DHCP 全部在后台任务和事件回调里完成，UI 线程只做只读快照。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * 链路状态机
 *
 * 成员统一加 ETH_STATE_ 前缀：esp_eth 的 hal/eth_types.h 里已经有
 * ETH_LINK_UP / ETH_LINK_DOWN 枚举，重名会编译失败。
 */
typedef enum {
    ETH_STATE_IDLE = 0,      /*!< 还没启动 */
    ETH_STATE_STARTING,      /*!< 正在装驱动 / PHY 初始化 */
    ETH_STATE_DETECTING,     /*!< 驱动已启动，等待第一次链路检测 */
    ETH_STATE_LINK_UP,       /*!< 链路已连通（可能还在等 DHCP） */
    ETH_STATE_LINK_DOWN,     /*!< 无链路：网线未插、对端未通电 */
    ETH_STATE_ERROR,         /*!< 驱动安装或 PHY 初始化失败 */
} eth_link_state_t;

/** 供 UI 轮询的快照，互斥保护 */
typedef struct {
    eth_link_state_t state;
    uint8_t  mac[6];
    char     ip[16];        /*!< 空串表示还没有 IP */
    char     netmask[16];
    char     gw[16];
    uint32_t speed_mbps;    /*!< 0 表示未知 */
    bool     full_duplex;
    char     error[48];     /*!< 仅在 ETH_LINK_ERROR 时有意义 */
} eth_link_status_t;

/**
 * @brief 后台启动以太网（非阻塞）
 *
 * 只创建任务并立即返回。真正的驱动安装结果通过 eth_link_get_status() 观察。
 */
esp_err_t eth_link_start_async(void);

/**
 * @brief 取一份状态快照（任意任务可调用，不加锁返回）
 */
void eth_link_get_status(eth_link_status_t *out);
