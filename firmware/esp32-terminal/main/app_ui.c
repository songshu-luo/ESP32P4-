/*
 * 终端界面
 *
 *   S0：点屏 / 触摸 / 背光
 *   S1：以太网（网络卡片）
 *   S2-a：MQTT 接入（MQTT 服务器卡片）
 *   S2-b：泰山派真实指标大卡（替换掉原来的触摸 / TAP ME 自检卡）
 *
 * 设计原则：UI 线程只读各模块的互斥快照，绝不等待网络、绝不解析 JSON
 * （解析在 esp-mqtt 任务里做，见 device_metrics.c）。
 *
 * 中文字体：LVGL 自带的 Montserrat 只有拉丁字母，中文用 tools/gen_cjk_font.sh
 * 生成的子集字体（只包含本文件里出现过的汉字），尺寸 16/20/28 px。
 * 改动本文件里的中文文案后必须重新生成字体，否则会显示成方块。
 */

#include "app_ui.h"

#include <stdio.h>

#include "esp_timer.h"

#include "device_metrics.h"
#include "device_status.h"
#include "eth_link.h"
#include "mqtt_link.h"

#include "fonts/cjk_fonts.h"

#define APP_VERSION     "0.5.1-s2c"

/* 网络状态轮询周期（ms） */
#define NET_POLL_PERIOD_MS      (500)

/* MQTT 链路状态轮询周期（ms） */
#define MQTT_POLL_PERIOD_MS     (1000)

/* 指标轮询周期（ms）：数据 2 s 一帧，500 ms 轮询足够跟上，且过期判断及时 */
#define METRICS_POLL_PERIOD_MS  (500)

/* 在线状态轮询周期（ms）：status 5 s 一条、有效期 15 s，500 ms 足够 */
#define STATUS_POLL_PERIOD_MS   (500)

/* 配色：深色背景 + 高对比前景，1024x600 远距离可读 */
#define COLOR_BG        lv_color_hex(0x101418)
#define COLOR_CARD      lv_color_hex(0x1c2230)
#define COLOR_ACCENT    lv_color_hex(0x29b6f6)
#define COLOR_TEXT      lv_color_hex(0xe8eef6)
#define COLOR_MUTED     lv_color_hex(0x8a97a8)
#define COLOR_OK        lv_color_hex(0x43a047)
#define COLOR_WARN      lv_color_hex(0xffb300)
#define COLOR_ERR       lv_color_hex(0xef5350)
#define COLOR_STALE     lv_color_hex(0x55606e)   /* 过期数据的灰，比 MUTED 更暗 */

typedef struct {
    lv_obj_t *lbl_uptime;

    /* 在线状态灯 */
    lv_obj_t *lamp;
    lv_obj_t *lbl_status;

    /* 指标区 */
    lv_obj_t *lbl_dev_head;
    lv_obj_t *lbl_dev_age;
    lv_obj_t *val_cpu;
    lv_obj_t *det_cpu;
    lv_obj_t *val_mem;
    lv_obj_t *det_mem;
    lv_obj_t *val_temp;
    lv_obj_t *det_temp;
    lv_obj_t *val_disk;
    lv_obj_t *det_disk;

    /* 网络 / MQTT */
    lv_obj_t *lbl_net_state;
    lv_obj_t *lbl_net_ip;
    lv_obj_t *lbl_net_detail;
    lv_obj_t *lbl_mqtt_state;
    lv_obj_t *lbl_mqtt_msg;
    lv_obj_t *lbl_mqtt_detail;
} app_ui_ctx_t;

static app_ui_ctx_t s_ctx;

/* ---------------------------------------------------------------- 工具函数 */

static lv_obj_t *create_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_scrollable(card, false);
    return card;
}

static lv_obj_t *create_label(lv_obj_t *parent, const lv_font_t *font,
                              lv_color_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

/** 一个指标卡：标题贴顶、大字数值居中、细节贴底 */
static lv_obj_t *create_metric_card(lv_obj_t *parent, int32_t x, int32_t y,
                                    int32_t w, int32_t h, const char *title,
                                    lv_obj_t **out_value, lv_obj_t **out_detail)
{
    lv_obj_t *card = create_card(parent, w, h);
    lv_obj_set_pos(card, x, y);

    lv_obj_t *t = create_label(card, &lv_font_cjk_20, COLOR_MUTED, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 数值只有数字和 %，用 Montserrat 36 更有冲击力 */
    *out_value = create_label(card, &lv_font_montserrat_36, COLOR_STALE, "--");
    lv_obj_align(*out_value, LV_ALIGN_LEFT_MID, 0, 6);

    *out_detail = create_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
    lv_obj_align(*out_detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return card;
}

/** 字节数 → "1.7 / 3.8 GB"，避免在屏幕上出现 10 位数字。
 *  total 缺失（<=0）时返回 false，让调用方换成别的说明，而不是显示 "0 / 0 MB"。 */
static bool format_bytes_pair(char *buf, size_t len, double used, double total)
{
    const double gib = 1024.0 * 1024.0 * 1024.0;
    if (total <= 0) {
        return false;
    }
    if (total >= gib) {
        snprintf(buf, len, "%.1f / %.1f GB", used / gib, total / gib);
    } else {
        snprintf(buf, len, "%.0f / %.0f MB", used / (1024.0 * 1024.0),
                 total / (1024.0 * 1024.0));
    }
    return true;
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

/* ---------------------------------------------------------------- 定时刷新 */

/** 运行时长每秒刷新，兼作「LVGL 任务没卡死」的证据 */
static void uptime_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_ctx.lbl_uptime, "运行  %02u:%02u:%02u",
                          (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
}

/* ---------------------------------------------------------------- 以太网状态 */

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
            /* 正常在线时不显示速率/双工/网关：这些是排障信息，日常只会让屏幕变吵。
             * 需要时看串口日志即可。 */
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

/* ---------------------------------------------------------------- MQTT 状态 */

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

/* ---------------------------------------------------------------- 在线状态灯 */

/**
 * 三态判定，严格按 protocol/topics-v1.md §3 的优先级：
 *
 *   1. Hub 不可达（MQTT 没连上）      → 未知，绝不沿用上次的绿色
 *   2. status 报了 online 且没过期     → 在线
 *   3. status 报了 offline（LWT）      → 离线
 *   4. 曾经 online 但超时              → 过期
 *   5. 从没收到过                      → 未知
 *
 * 第 1 条是重点：broker 挂了以后，最后一条 retained 的 online 还躺在那里，
 * 只看消息的话灯会一直绿着——这正是要避免的「假在线」。
 */
static void status_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    mqtt_link_status_t link;
    mqtt_link_get_status(&link);

    device_status_t s;
    device_status_get(&s);

    const char *text;
    lv_color_t  color;

    if (link.state != MQTT_LINK_ONLINE) {
        text  = "未知";
        color = COLOR_MUTED;
    } else if (s.state == DEVICE_STATUS_ONLINE) {
        text  = "在线";
        color = COLOR_OK;
    } else if (s.state == DEVICE_STATUS_OFFLINE) {
        text  = "离线";
        color = COLOR_ERR;
    } else if (s.expired) {
        text  = "过期";
        color = COLOR_WARN;
    } else {
        text  = "未知";
        color = COLOR_MUTED;
    }

    lv_obj_set_style_bg_color(s_ctx.lamp, color, 0);
    lv_label_set_text(s_ctx.lbl_status, text);
    lv_obj_set_style_text_color(s_ctx.lbl_status, color, 0);
}

/* ---------------------------------------------------------------- 指标区 */

static void metrics_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    device_metrics_t m;
    device_metrics_get(&m);

    /* 「可以当当前值显示」= 解析成功且未过期。
     * 过期后数值降为灰色 "--"，但设备名和新鲜度继续显示，让人知道是断了、还有旧数据。 */
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
        if (!format_bytes_pair(detail, sizeof(detail), m.mem_used_bytes, m.mem_total_bytes)) {
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
        if (!format_bytes_pair(detail, sizeof(detail), m.disk_used_bytes, m.disk_total_bytes)) {
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

    /* ---- 头部新鲜度 ---- */
    if (!m.valid) {
        lv_label_set_text(s_ctx.lbl_dev_age, "等待第一帧数据");
        lv_obj_set_style_text_color(s_ctx.lbl_dev_age, COLOR_MUTED, 0);
    } else if (m.expired) {
        lv_label_set_text_fmt(s_ctx.lbl_dev_age, "已过期 %.1f 秒（上限 %u 秒）",
                              m.age_ms / 1000.0, (unsigned)m.valid_for_seconds);
        lv_obj_set_style_text_color(s_ctx.lbl_dev_age, COLOR_ERR, 0);
    } else {
        lv_label_set_text_fmt(s_ctx.lbl_dev_age, "更新于 %.1f 秒前   第 %u 帧",
                              m.age_ms / 1000.0, (unsigned)m.frame_count);
        lv_obj_set_style_text_color(s_ctx.lbl_dev_age, COLOR_OK, 0);
    }
}

/* ---------------------------------------------------------------- 界面构建 */

void app_ui_build(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);

    /* ---- 顶部：标题 + 固件版本 + 运行时长 ----
     * 芯片型号/rev/核心数/屏幕分辨率属于固定信息，屏幕上看一次就够，
     * 删掉把纵向空间让给指标区。需要时看串口日志。 */
    lv_obj_t *title = create_label(scr, &lv_font_cjk_28, COLOR_TEXT, "ESP32-P4 智能终端");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 32, 22);

    lv_obj_t *ver = create_label(scr, &lv_font_cjk_16, COLOR_MUTED,
                                 "固件 " APP_VERSION);
    lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -32, 26);

    s_ctx.lbl_uptime = create_label(scr, &lv_font_cjk_20, COLOR_ACCENT, "运行  00:00:00");
    lv_obj_align(s_ctx.lbl_uptime, LV_ALIGN_TOP_RIGHT, -32, 50);

    /* ---- 指标区头部：在线状态灯 + 设备名 + 新鲜度 ---- */
    s_ctx.lamp = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ctx.lamp);
    lv_obj_set_size(s_ctx.lamp, 16, 16);
    lv_obj_set_style_radius(s_ctx.lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ctx.lamp, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ctx.lamp, COLOR_MUTED, 0);
    lv_obj_set_clickable(s_ctx.lamp, false);
    lv_obj_set_pos(s_ctx.lamp, 32, 94);

    s_ctx.lbl_status = create_label(scr, &lv_font_cjk_20, COLOR_MUTED, "未知");
    lv_obj_set_pos(s_ctx.lbl_status, 56, 86);

    s_ctx.lbl_dev_head = create_label(scr, &lv_font_cjk_20, COLOR_TEXT,
                                      "泰山派 3M   squirrel-lckfb");
    lv_obj_set_pos(s_ctx.lbl_dev_head, 140, 86);

    s_ctx.lbl_dev_age = create_label(scr, &lv_font_cjk_16, COLOR_MUTED,
                                     "等待第一帧数据");
    lv_obj_align(s_ctx.lbl_dev_age, LV_ALIGN_TOP_RIGHT, -32, 90);

    /* ---- 四张指标卡：CPU / 内存 / 主控温度 / 磁盘 ----
     * 宽度：4×222 + 3×24 = 960，正好铺满 1024 减去左右 32 边距。 */
    const int32_t card_w = 222;
    const int32_t card_h = 210;
    const int32_t gap = 24;
    const int32_t row_y = 132;

    create_metric_card(scr, 32, row_y, card_w, card_h, "CPU",
                       &s_ctx.val_cpu, &s_ctx.det_cpu);
    create_metric_card(scr, 32 + (card_w + gap), row_y, card_w, card_h, "内存",
                       &s_ctx.val_mem, &s_ctx.det_mem);
    create_metric_card(scr, 32 + 2 * (card_w + gap), row_y, card_w, card_h, "主控温度",
                       &s_ctx.val_temp, &s_ctx.det_temp);
    create_metric_card(scr, 32 + 3 * (card_w + gap), row_y, card_w, card_h, "磁盘",
                       &s_ctx.val_disk, &s_ctx.det_disk);

    /* ---- 底部左：以太网链路 + IP ---- */
    lv_obj_t *card_net = create_card(scr, 468, 140);
    lv_obj_align(card_net, LV_ALIGN_BOTTOM_LEFT, 32, -46);

    lv_obj_t *net_title = create_label(card_net, &lv_font_cjk_16, COLOR_MUTED, "网络");
    lv_obj_set_pos(net_title, 0, 0);

    s_ctx.lbl_net_state = create_label(card_net, &lv_font_cjk_20, COLOR_WARN, "空闲");
    lv_obj_set_pos(s_ctx.lbl_net_state, 0, 22);

    /* IP 是纯 ASCII，用 Montserrat 28 更醒目 */
    s_ctx.lbl_net_ip = create_label(card_net, &lv_font_montserrat_28, COLOR_ACCENT, "---.---.---.---");
    lv_obj_set_pos(s_ctx.lbl_net_ip, 0, 48);

    s_ctx.lbl_net_detail = create_label(card_net, &lv_font_cjk_16, COLOR_MUTED, "未启动");
    lv_obj_set_pos(s_ctx.lbl_net_detail, 0, 82);

    /* ---- 底部右：MQTT 链路 ---- */
    lv_obj_t *card_mqtt = create_card(scr, 468, 140);
    lv_obj_align(card_mqtt, LV_ALIGN_BOTTOM_RIGHT, -32, -46);

    lv_obj_t *mqtt_title = create_label(card_mqtt, &lv_font_cjk_16, COLOR_MUTED, "MQTT 服务器");
    lv_obj_set_pos(mqtt_title, 0, 0);

    s_ctx.lbl_mqtt_state = create_label(card_mqtt, &lv_font_cjk_20, COLOR_MUTED, "空闲");
    lv_obj_set_pos(s_ctx.lbl_mqtt_state, 0, 22);

    s_ctx.lbl_mqtt_msg = create_label(card_mqtt, &lv_font_cjk_28, COLOR_TEXT, "0 条");
    lv_obj_set_pos(s_ctx.lbl_mqtt_msg, 0, 46);

    s_ctx.lbl_mqtt_detail = create_label(card_mqtt, &lv_font_cjk_16, COLOR_MUTED,
                                         "服务器未解析");
    lv_obj_set_pos(s_ctx.lbl_mqtt_detail, 0, 82);

    /* ---- 页脚已删除：LVGL 版本号和数据来源属于说明书内容，不该占屏幕。
     * 需要时看 docs/S2c-在线状态灯.md 或串口日志。 ---- */

    /* ---- 定时器 ---- */
    lv_timer_create(uptime_cb, 1000, NULL);
    lv_timer_create(net_poll_cb, NET_POLL_PERIOD_MS, NULL);
    lv_timer_create(mqtt_poll_cb, MQTT_POLL_PERIOD_MS, NULL);
    lv_timer_create(status_poll_cb, STATUS_POLL_PERIOD_MS, NULL);
    lv_timer_create(metrics_poll_cb, METRICS_POLL_PERIOD_MS, NULL);

    /* 首帧就把真实状态填上，避免开机先闪一下占位文字 */
    net_poll_cb(NULL);
    mqtt_poll_cb(NULL);
    status_poll_cb(NULL);
    metrics_poll_cb(NULL);
}
