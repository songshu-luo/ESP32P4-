/*
 * 总览页（S2-d）
 *
 * 原 S2-b/S2-c 那一屏整体搬进「总览」页签里：四张指标卡 + 一张网络卡 + 一张 MQTT 卡。
 * 在线状态灯已经挪到跨页面常驻的顶部状态栏（见 app_ui.c），这里不再重复。
 *
 * 页面尺寸 = 1024 x (600 - 状态栏 56 - 页签栏 56) = 1024 x 488。
 *
 * 新鲜度与「--」的规则全部沿用 S2-b：
 *   - 解析成功且未过期才显示数值，否则显示 "--"；
 *   - 说明文字按「等待 Hub 数据 / 数据已过期 / 未上报」区分，绝不把没数据说成 0。
 */

#include "app_ui_internal.h"

#include <stdio.h>

#include "device_metrics.h"
#include "eth_link.h"
#include "mqtt_link.h"

#include "fonts/cjk_fonts.h"

/* 轮询周期：数据 2 s 一帧、500 ms 轮询足够跟上，且过期判断及时 */
#define METRICS_POLL_PERIOD_MS  (500)
#define NET_POLL_PERIOD_MS      (500)
#define MQTT_POLL_PERIOD_MS     (1000)

/* ---------------------------------------------------------------- 工具函数 */

/** 一个指标卡：标题贴顶、大字数值居中、细节贴底 */
static lv_obj_t *create_metric_card(lv_obj_t *parent, int32_t x, int32_t y,
                                    int32_t w, int32_t h, const char *title,
                                    lv_obj_t **out_value, lv_obj_t **out_detail)
{
    lv_obj_t *card = ui_card(parent, w, h);
    lv_obj_set_pos(card, x, y);

    lv_obj_t *t = ui_label(card, &lv_font_cjk_20, COLOR_MUTED, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 数值只有数字和 %，用 Montserrat 36 更有冲击力 */
    *out_value = ui_label(card, &lv_font_montserrat_36, COLOR_STALE, "--");
    lv_obj_align(*out_value, LV_ALIGN_LEFT_MID, 0, 6);

    *out_detail = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
    lv_obj_align(*out_detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return card;
}

/** 数值位置显示 "--" 时的说明文字。绝不能把「没有数据」说成「0」。 */
static const char *placeholder_detail(const device_metrics_t *m)
{
    if (!m->valid) {
        return "等待 Hub 数据";
    }
    if (m->expired) {
        return "数据已过期";
    }
    return "未上报";
}

/* ---------------------------------------------------------------- 指标区 */

static void metrics_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    device_metrics_t m;
    device_metrics_get(&m);

    /* 「可以当当前值显示」= 解析成功且未过期。
     * 过期后数值降为灰色 "--"，说明文字继续告诉人是断了还是没上报。 */
    const bool fresh = m.valid && !m.expired;
    const char *placeholder = placeholder_detail(&m);

    char value[32];
    char detail[64];

    /* ---- CPU ---- */
    if (fresh && m.has_cpu) {
        snprintf(value, sizeof(value), "%.1f%%", m.cpu_percent);
        if (m.has_load && m.cpu_cores > 0) {
            snprintf(detail, sizeof(detail), "%d 核   负载 %.2f", m.cpu_cores, m.load1);
        } else {
            snprintf(detail, sizeof(detail), "占用率");
        }
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(detail, sizeof(detail), "%s", placeholder);
    }
    lv_obj_set_style_text_color(s_ctx.val_cpu, fresh ? COLOR_ACCENT : COLOR_STALE, 0);
    lv_label_set_text(s_ctx.val_cpu, value);
    lv_label_set_text(s_ctx.det_cpu, detail);

    /* ---- 内存 ---- */
    if (fresh && m.has_mem) {
        snprintf(value, sizeof(value), "%.1f%%", m.mem_percent);
        if (!ui_format_bytes_pair(detail, sizeof(detail), m.mem_used_bytes, m.mem_total_bytes)) {
            snprintf(detail, sizeof(detail), "已占用");
        }
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(detail, sizeof(detail), "%s", placeholder);
    }
    lv_obj_set_style_text_color(s_ctx.val_mem, fresh ? COLOR_OK : COLOR_STALE, 0);
    lv_label_set_text(s_ctx.val_mem, value);
    lv_label_set_text(s_ctx.det_mem, detail);

    /* ---- 主控温度 ---- */
    if (fresh && m.has_soc) {
        /* LVGL 默认字体没有 "°"，用 C 代替，避免出现方块字 */
        snprintf(value, sizeof(value), "%.1f C", m.soc_celsius);
        if (m.has_soc_max) {
            snprintf(detail, sizeof(detail), "最高 %.1f C", m.soc_max_celsius);
        } else {
            snprintf(detail, sizeof(detail), "主控传感器");
        }
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(detail, sizeof(detail), "%s", placeholder);
    }
    /* 温度 >= 70 C 已偏离正常待机区间，用黄色提示 */
    lv_color_t temp_color = COLOR_TEXT;
    if (fresh && m.has_soc && m.soc_celsius >= 70.0) {
        temp_color = COLOR_WARN;
    }
    lv_obj_set_style_text_color(s_ctx.val_temp, fresh ? temp_color : COLOR_STALE, 0);
    lv_label_set_text(s_ctx.val_temp, value);
    lv_label_set_text(s_ctx.det_temp, detail);

    /* ---- 磁盘 ---- */
    if (fresh && m.has_disk) {
        snprintf(value, sizeof(value), "%.1f%%", m.disk_percent);
        if (!ui_format_bytes_pair(detail, sizeof(detail), m.disk_used_bytes, m.disk_total_bytes)) {
            snprintf(detail, sizeof(detail), "已占用");
        }
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(detail, sizeof(detail), "%s", placeholder);
    }
    /* 磁盘 >= 85% 红色，>= 70% 黄色 */
    lv_color_t disk_color = COLOR_TEXT;
    if (fresh && m.has_disk) {
        if (m.disk_percent >= 85.0) {
            disk_color = COLOR_ERR;
        } else if (m.disk_percent >= 70.0) {
            disk_color = COLOR_WARN;
        }
    }
    lv_obj_set_style_text_color(s_ctx.val_disk, fresh ? disk_color : COLOR_STALE, 0);
    lv_label_set_text(s_ctx.val_disk, value);
    lv_label_set_text(s_ctx.det_disk, detail);
}

/* ---------------------------------------------------------------- 以太网 */

/** 把链路状态机翻译成一行给人看的文字 + 颜色 */
static void net_apply(eth_link_status_t *st)
{
    const char *text = "";
    lv_color_t  color = COLOR_MUTED;
    char detail[96] = "";

    switch (st->state) {
    case ETH_STATE_IDLE:
        text = "空闲"; color = COLOR_MUTED;
        snprintf(detail, sizeof(detail), "未启动");
        break;

    case ETH_STATE_STARTING:
        text = "启动中"; color = COLOR_WARN;
        snprintf(detail, sizeof(detail), "正在安装 EMAC 驱动");
        break;

    case ETH_STATE_DETECTING:
        text = "无链路"; color = COLOR_WARN;
        snprintf(detail, sizeof(detail), "请检查网线");
        break;

    case ETH_STATE_LINK_UP:
        if (st->ip[0] == '\0') {
            text = "链路已通"; color = COLOR_WARN;
            snprintf(detail, sizeof(detail), "等待 DHCP");
        } else {
            /* 正常在线时不显示速率/双工/网关：这些是排障信息，日常只会让屏幕变吵。 */
            text = "在线"; color = COLOR_OK;
            detail[0] = '\0';
        }
        break;

    case ETH_STATE_LINK_DOWN:
        text = "链路断开"; color = COLOR_ERR;
        if (st->mac[0] | st->mac[1] | st->mac[2] | st->mac[3] | st->mac[4] | st->mac[5]) {
            snprintf(detail, sizeof(detail), "网线被拔出  MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     st->mac[0], st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5]);
        } else {
            snprintf(detail, sizeof(detail), "网线被拔出");
        }
        break;

    case ETH_STATE_ERROR:
        text = "网络错误"; color = COLOR_ERR;
        /* 驱动层给的是英文 ASCII，字体里包含 ASCII，照样能显示 */
        snprintf(detail, sizeof(detail), "%s",
                 st->error[0] ? st->error : "driver init failed");
        break;

    default:
        text = "未知";
        break;
    }

    lv_label_set_text(s_ctx.lbl_net_state, text);
    lv_obj_set_style_text_color(s_ctx.lbl_net_state, color, 0);
    lv_label_set_text(s_ctx.lbl_net_ip, st->ip[0] ? st->ip : "---.---.---.---");
    lv_label_set_text(s_ctx.lbl_net_detail, detail);
}

static void net_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    eth_link_status_t st;
    eth_link_get_status(&st);
    net_apply(&st);
}

/* ---------------------------------------------------------------- MQTT */

/** 把 MQTT 状态机翻译成一行文字 + 颜色 */
static void mqtt_apply(const mqtt_link_status_t *st)
{
    const char *text = "";
    lv_color_t  color = COLOR_MUTED;

    switch (st->state) {
    case MQTT_LINK_IDLE:         text = "空闲";         color = COLOR_MUTED; break;
    case MQTT_LINK_WAIT_IP:      text = "等待网络";     color = COLOR_MUTED; break;
    case MQTT_LINK_RESOLVING:    text = "解析中";       color = COLOR_WARN;  break;
    case MQTT_LINK_CONNECTING:   text = "连接中";       color = COLOR_WARN;  break;
    case MQTT_LINK_SUBSCRIBING:  text = "订阅中";       color = COLOR_WARN;  break;
    case MQTT_LINK_ONLINE:       text = "已连接";       color = COLOR_OK;    break;
    case MQTT_LINK_DISCONNECTED: text = "已断开";       color = COLOR_WARN;  break;
    case MQTT_LINK_AUTH_FAIL:    text = "认证失败";     color = COLOR_ERR;   break;
    case MQTT_LINK_RESOLVE_FAIL: text = "找不到服务器"; color = COLOR_ERR;   break;
    case MQTT_LINK_ERROR:        text = "MQTT 错误";    color = COLOR_ERR;   break;
    default:                     text = "未知";         color = COLOR_MUTED; break;
    }

    lv_label_set_text(s_ctx.lbl_mqtt_state, text);
    lv_obj_set_style_text_color(s_ctx.lbl_mqtt_state, color, 0);

    /* 按主题拆开计数：只看总数分不清是 metrics 停了还是 status 停了 */
    lv_label_set_text_fmt(s_ctx.lbl_mqtt_msg, "指标 %u   状态 %u",
                          (unsigned)st->metrics_count, (unsigned)st->status_count);
    char line[96];
    const bool show_addr = (st->state == MQTT_LINK_ONLINE) ||
                           (st->state == MQTT_LINK_SUBSCRIBING) ||
                           (st->state == MQTT_LINK_CONNECTING);
    if (show_addr && st->host[0] && st->ip[0]) {
        snprintf(line, sizeof(line), "%s  %s", st->host, st->ip);
    } else if (st->detail[0]) {
        snprintf(line, sizeof(line), "%s", st->detail);
    } else if (st->host[0] && st->ip[0]) {
        snprintf(line, sizeof(line), "%s  %s", st->host, st->ip);
    } else {
        snprintf(line, sizeof(line), "服务器未解析");
    }
    lv_label_set_text(s_ctx.lbl_mqtt_detail, line);
}

static void mqtt_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    mqtt_link_status_t st;
    mqtt_link_get_status(&st);
    mqtt_apply(&st);
}

/* ---------------------------------------------------------------- 页面构建 */

void ui_page_overview_build(lv_obj_t *page)
{
    /* ---- 四张指标卡：CPU / 内存 / 主控温度 / 磁盘 ----
     * 宽度：4×222 + 3×24 = 960，正好铺满 1024 减去左右 32 边距。
     * 卡片高度按 488 的页面高度重排：4×222 卡 + 底部两张 150 卡。 */
    const int32_t card_w = 222;
    const int32_t card_h = 200;
    const int32_t gap    = 24;
    const int32_t row_y  = 40;

    create_metric_card(page, 32, row_y, card_w, card_h, "CPU",
                       &s_ctx.val_cpu, &s_ctx.det_cpu);
    create_metric_card(page, 32 + (card_w + gap), row_y, card_w, card_h, "内存",
                       &s_ctx.val_mem, &s_ctx.det_mem);
    create_metric_card(page, 32 + 2 * (card_w + gap), row_y, card_w, card_h, "主控温度",
                       &s_ctx.val_temp, &s_ctx.det_temp);
    create_metric_card(page, 32 + 3 * (card_w + gap), row_y, card_w, card_h, "磁盘",
                       &s_ctx.val_disk, &s_ctx.det_disk);

    /* ---- 底部左：以太网链路 + IP ---- */
    lv_obj_t *card_net = ui_card(page, 468, 150);
    lv_obj_align(card_net, LV_ALIGN_BOTTOM_LEFT, 32, -24);

    lv_obj_t *net_title = ui_label(card_net, &lv_font_cjk_16, COLOR_MUTED, "网络");
    lv_obj_set_pos(net_title, 0, 0);

    s_ctx.lbl_net_state = ui_label(card_net, &lv_font_cjk_20, COLOR_WARN, "空闲");
    lv_obj_set_pos(s_ctx.lbl_net_state, 0, 24);

    /* IP 是纯 ASCII，用 Montserrat 28 更醒目 */
    s_ctx.lbl_net_ip = ui_label(card_net, &lv_font_montserrat_28, COLOR_ACCENT,
                                "---.---.---.---");
    lv_obj_set_pos(s_ctx.lbl_net_ip, 0, 52);

    s_ctx.lbl_net_detail = ui_label(card_net, &lv_font_cjk_16, COLOR_MUTED, "未启动");
    lv_obj_align(s_ctx.lbl_net_detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* ---- 底部右：MQTT 链路 ---- */
    lv_obj_t *card_mqtt = ui_card(page, 468, 150);
    lv_obj_align(card_mqtt, LV_ALIGN_BOTTOM_RIGHT, -32, -24);

    lv_obj_t *mqtt_title = ui_label(card_mqtt, &lv_font_cjk_16, COLOR_MUTED, "MQTT 服务器");
    lv_obj_set_pos(mqtt_title, 0, 0);

    s_ctx.lbl_mqtt_state = ui_label(card_mqtt, &lv_font_cjk_20, COLOR_MUTED, "空闲");
    lv_obj_set_pos(s_ctx.lbl_mqtt_state, 0, 24);

    s_ctx.lbl_mqtt_msg = ui_label(card_mqtt, &lv_font_cjk_28, COLOR_TEXT, "指标 0   状态 0");
    lv_obj_set_pos(s_ctx.lbl_mqtt_msg, 0, 52);

    s_ctx.lbl_mqtt_detail = ui_label(card_mqtt, &lv_font_cjk_16, COLOR_MUTED,
                                     "服务器未解析");
    lv_obj_align(s_ctx.lbl_mqtt_detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* ---- 定时器 ---- */
    lv_timer_create(metrics_poll_cb, METRICS_POLL_PERIOD_MS, NULL);
    lv_timer_create(net_poll_cb, NET_POLL_PERIOD_MS, NULL);
    lv_timer_create(mqtt_poll_cb, MQTT_POLL_PERIOD_MS, NULL);

    /* 首帧就填上真实状态，避免开机先闪一下占位文字 */
    metrics_poll_cb(NULL);
    net_poll_cb(NULL);
    mqtt_poll_cb(NULL);
}