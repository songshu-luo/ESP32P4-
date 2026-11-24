/*
 * 设置页（S7）
 *
 * 布局（页面坐标原点左上，内容区 1024x488）：
 *
 *   480x140 @ (16,0)     卡 A 显示：亮度滑块 + 息屏超时
 *   480x140 @ (504,0)    卡 B 网络：链路 / IP / 网关 + MAC
 *   480x140 @ (16,148)   卡 C 连接：MQTT / 收包计数 / 对时
 *   480x140 @ (504,148)  卡 D 固件：版本 / 运行槽 / OTA 地址 / 更新状态
 *   976x130 @ (16,296)   卡 E 诊断日志：最近 6 条 ESP_LOG
 *
 * 设计理由：
 *   - 本页是「现场排障页」，所有值都来自各模块的互斥快照，定时器里
 *     只读内存、不做网络或文件 IO，所以 500 ms 刷新不会卡住 LVGL；
 *   - 亮度与息屏状态属于设备而不是页面，放在 app_settings 里，换页/重建
 *     界面都不会丢；本文件只保留控件句柄；
 *   - 日志来自 log_ring 的静态环形缓冲，页面只负责显示最近几行。
 *
 * 控件句柄全部是文件内 static，不往 s_ctx 加字段：这一页没有跨页面共享
 * 的对象，放进去只会让 app_ui_internal.h 继续膨胀。
 */

#include "app_ui_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "app_settings.h"
#include "eth_link.h"
#include "log_ring.h"
#include "mqtt_link.h"
#include "ota_update.h"

#include "fonts/cjk_fonts.h"

#define SETTINGS_POLL_PERIOD_MS (500)

/* 对时门槛与总览页保持一致：2023-11 之前的时间视为还没对上 */
#define SETTINGS_CLOCK_VALID_EPOCH  ((time_t)1700000000)

/* 卡 E 显示的行数；log_ring_snapshot() 的行间分隔符是 '\n'，
 * 6 行 × 158 字节 + 分隔符仍小于下面的缓冲。 */
#define SETTINGS_LOG_LINES      (6)
#define SETTINGS_LOG_BUF_LEN    (SETTINGS_LOG_LINES * 160 + 16)

/* 息屏按钮可选值：30 秒 / 1 分钟 / 5 分钟 / 常亮 */
static const uint32_t s_dim_values[4] = { 30000, 60000, 300000, 0 };
static const char *const s_dim_texts[4] = { "30 秒", "1 分钟", "5 分钟", "常亮" };

/* ---------------------------------------------------------------- 控件句柄 */

static lv_obj_t *s_slider;
static lv_obj_t *s_lbl_percent;
static lv_obj_t *s_lbl_dim[4];

static lv_obj_t *s_lbl_net_link;
static lv_obj_t *s_lbl_net_ip;
static lv_obj_t *s_lbl_net_gw;

static lv_obj_t *s_lbl_mqtt_state;
static lv_obj_t *s_lbl_mqtt_count;
static lv_obj_t *s_lbl_clock;

static lv_obj_t *s_lbl_fw_version;
static lv_obj_t *s_lbl_fw_slot;
static lv_obj_t *s_lbl_fw_ota;
static lv_obj_t *s_lbl_fw_update;

static lv_obj_t *s_lbl_log_count;
static lv_obj_t *s_lbl_log_body;

/* 日志快照缓冲：LVGL 定时器回调里用静态区，避免占 LVGL 任务栈 */
static char s_log_buf[SETTINGS_LOG_BUF_LEN];

/* ---------------------------------------------------------------- 工具函数 */

/** 运行时长：秒 → 「35 分钟」/「5 小时 12 分」/「3 天 5 小时」 */
static void format_uptime(char *buf, size_t len, uint64_t seconds)
{
    const uint64_t days  = seconds / 86400u;
    const uint64_t hours = (seconds % 86400u) / 3600u;
    const uint64_t mins  = (seconds % 3600u) / 60u;

    if (days > 0) {
        snprintf(buf, len, "%llu 天 %llu 小时",
                 (unsigned long long)days, (unsigned long long)hours);
    } else if (hours > 0) {
        snprintf(buf, len, "%llu 小时 %llu 分",
                 (unsigned long long)hours, (unsigned long long)mins);
    } else {
        snprintf(buf, len, "%llu 分钟", (unsigned long long)mins);
    }
}

/** 息屏超时 → 中文短词，用于按钮高亮匹配 */
static bool dim_timeout_selected(uint32_t timeout)
{
    return app_settings_get_dim_timeout() == timeout;
}

/** 按当前设置刷新 4 个息屏按钮的文字颜色；构建时与点击后各调一次 */
static void update_dim_buttons(void)
{
    for (int i = 0; i < 4; i++) {
        const bool selected = dim_timeout_selected(s_dim_values[i]);
        lv_obj_set_style_text_color(s_lbl_dim[i],
                                    selected ? COLOR_ACCENT : COLOR_MUTED, 0);
    }
}

/* ---------------------------------------------------------------- 事件回调 */

static void brightness_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const int value = (int)lv_slider_get_value(slider);

    app_settings_set_brightness(value);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", app_settings_get_brightness());
    lv_label_set_text(s_lbl_percent, buf);
}

static void dim_button_cb(lv_event_t *e)
{
    const uint32_t *value = lv_event_get_user_data(e);
    app_settings_set_dim_timeout(value != NULL ? *value : 0);
    update_dim_buttons();
}

/* 「息屏」按钮：立刻息屏（不受右侧超时设置影响）。
 * 屏保会盖住整屏，这里不做任何页面刷新。 */
static void sleep_now_cb(lv_event_t *e)
{
    (void)e;
    app_settings_sleep_now();
}

/* ---------------------------------------------------------------- 卡 A：显示 */

static void build_display_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 480, 140);
    lv_obj_set_pos(card, 16, 0);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, "显示");
    lv_obj_set_pos(title, 0, 0);

    /* -------- 第一行：亮度 -------- */
    lv_obj_t *cap = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "亮度");
    lv_obj_set_pos(cap, 0, 36);

    s_slider = lv_slider_create(card);
    lv_obj_set_pos(s_slider, 88, 34);
    lv_obj_set_size(s_slider, 260, 16);
    lv_slider_set_range(s_slider, 10, 100);
    lv_slider_set_value(s_slider, app_settings_get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, brightness_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    char percent[16];
    snprintf(percent, sizeof(percent), "%d%%", app_settings_get_brightness());
    s_lbl_percent = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, percent);
    lv_obj_set_pos(s_lbl_percent, 368, 26);
    lv_obj_set_width(s_lbl_percent, 80);
    lv_obj_set_style_text_align(s_lbl_percent, LV_TEXT_ALIGN_RIGHT, 0);

    /* -------- 第二行：息屏（原地改成可点击按钮，位置沿用原 cap2） -------- */
    lv_obj_t *btn_sleep = lv_button_create(card);
    lv_obj_set_pos(btn_sleep, 0, 92);
    lv_obj_set_size(btn_sleep, 72, 32);          /* 0 + 72 = 72 < 88，不与「30 秒」重叠 */
    lv_obj_set_style_radius(btn_sleep, 8, 0);
    lv_obj_set_style_bg_color(btn_sleep, COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(btn_sleep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_sleep, 1, 0);       /* 1px 描边：明确是可按的按钮 */
    lv_obj_set_style_border_color(btn_sleep, COLOR_MUTED, 0);
    lv_obj_set_style_shadow_width(btn_sleep, 0, 0);
    lv_obj_set_style_outline_width(btn_sleep, 0, 0);
    lv_obj_set_style_pad_all(btn_sleep, 0, 0);

    lv_obj_t *lbl_sleep = ui_label(btn_sleep, &lv_font_cjk_16, COLOR_TEXT, "息屏");
    lv_obj_center(lbl_sleep);
    lv_obj_set_clickable(lbl_sleep, false);      /* 点击交给按钮 */

    lv_obj_add_event_cb(btn_sleep, sleep_now_cb, LV_EVENT_CLICKED, NULL);

    /* 4 个息屏按钮。规格给的 x 序列 88/196/304/412 + 100 px 宽、8 px 间距，
     * 第 4 个的右边缘会落到内容区 x=512（内容区只有 448 宽），「常亮」按钮
     * 有一半被卡片裁掉。这里保留起点 88 与 8 px 间距，只把宽度收到 84：
     * 88 + 4×84 + 3×8 = 448，四个按钮完整落在卡片内，文字仍然宽松可读。 */
    static const int32_t btn_x[4] = { 88, 180, 272, 364 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_button_create(card);
        lv_obj_set_pos(btn, btn_x[i], 92);
        lv_obj_set_size(btn, 84, 32);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, COLOR_BAR, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);

        s_lbl_dim[i] = ui_label(btn, &lv_font_cjk_16, COLOR_MUTED, s_dim_texts[i]);
        lv_obj_center(s_lbl_dim[i]);

        /* user_data 用静态数组元素的地址：生命周期等于固件 */
        lv_obj_add_event_cb(btn, dim_button_cb, LV_EVENT_CLICKED,
                            (void *)&s_dim_values[i]);
    }
    update_dim_buttons();
}

/* ---------------------------------------------------------------- 卡 B：网络 */

static void build_network_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 480, 140);
    lv_obj_set_pos(card, 504, 0);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, "网络");
    lv_obj_set_pos(title, 0, 0);

    /* 三行：链路 / IP + 掩码 / 网关 + MAC。每行 18 px 高、行距 6 px，
     * 从 y=36 起排到 y=84，正好落在 448x108 的内容区里。 */
    s_lbl_net_link = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "链路 --");
    lv_obj_set_pos(s_lbl_net_link, 0, 36);
    lv_obj_set_size(s_lbl_net_link, 448, 20);
    lv_label_set_long_mode(s_lbl_net_link, LV_LABEL_LONG_DOT);

    s_lbl_net_ip = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "IP -- | 掩码 --");
    lv_obj_set_pos(s_lbl_net_ip, 0, 60);
    lv_obj_set_size(s_lbl_net_ip, 448, 20);
    lv_label_set_long_mode(s_lbl_net_ip, LV_LABEL_LONG_DOT);

    s_lbl_net_gw = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "网关 -- | MAC --");
    lv_obj_set_pos(s_lbl_net_gw, 0, 84);
    lv_obj_set_size(s_lbl_net_gw, 448, 20);
    lv_label_set_long_mode(s_lbl_net_gw, LV_LABEL_LONG_DOT);
}

/* ---------------------------------------------------------------- 卡 C：连接 */

static const char *mqtt_state_text(mqtt_link_state_t state)
{
    switch (state) {
    case MQTT_LINK_ONLINE:       return "在线";
    case MQTT_LINK_AUTH_FAIL:    return "鉴权失败";
    case MQTT_LINK_RESOLVE_FAIL: return "解析失败";
    default:                     return "断开";
    }
}

static void build_connect_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 480, 140);
    lv_obj_set_pos(card, 16, 148);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, "连接");
    lv_obj_set_pos(title, 0, 0);

    s_lbl_mqtt_state = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "断开 -- -> --");
    lv_obj_set_pos(s_lbl_mqtt_state, 0, 36);
    lv_obj_set_size(s_lbl_mqtt_state, 448, 20);
    lv_label_set_long_mode(s_lbl_mqtt_state, LV_LABEL_LONG_DOT);

    s_lbl_mqtt_count = ui_label(card, &lv_font_cjk_16, COLOR_TEXT,
                                "收包 0（metrics 0 / status 0）");
    lv_obj_set_pos(s_lbl_mqtt_count, 0, 60);
    lv_obj_set_size(s_lbl_mqtt_count, 448, 20);
    lv_label_set_long_mode(s_lbl_mqtt_count, LV_LABEL_LONG_DOT);

    s_lbl_clock = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "对时 未同步");
    lv_obj_set_pos(s_lbl_clock, 0, 84);
}

/* ---------------------------------------------------------------- 卡 D：固件 */

static void build_firmware_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 480, 140);
    lv_obj_set_pos(card, 504, 148);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, "固件");
    lv_obj_set_pos(title, 0, 0);

    /* 第 1 行用 20 px 大字：版本号是这一页最常被读到的信息 */
    s_lbl_fw_version = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, "0.0.0");
    lv_obj_set_pos(s_lbl_fw_version, 0, 30);

    s_lbl_fw_slot = ui_label(card, &lv_font_cjk_16, COLOR_TEXT,
                             "运行槽 -- | 开机 0 分钟");
    lv_obj_set_pos(s_lbl_fw_slot, 0, 58);
    lv_obj_set_size(s_lbl_fw_slot, 448, 20);
    lv_label_set_long_mode(s_lbl_fw_slot, LV_LABEL_LONG_DOT);

    s_lbl_fw_ota = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "OTA 页面 等待网络");
    lv_obj_set_pos(s_lbl_fw_ota, 0, 80);
    lv_obj_set_size(s_lbl_fw_ota, 448, 20);
    lv_label_set_long_mode(s_lbl_fw_ota, LV_LABEL_LONG_DOT);

    /* 第 4 行：卡片底部还剩 20 px，够放一行 16 px 的更新状态 */
    s_lbl_fw_update = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "更新状态 --");
    lv_obj_set_pos(s_lbl_fw_update, 0, 102);
    lv_obj_set_size(s_lbl_fw_update, 448, 20);
    lv_label_set_long_mode(s_lbl_fw_update, LV_LABEL_LONG_DOT);
}

/* ---------------------------------------------------------------- 卡 E：诊断日志 */

static void build_log_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, 976, 130);
    lv_obj_set_pos(card, 16, 296);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, "诊断日志");
    lv_obj_set_pos(title, 0, 0);

    s_lbl_log_count = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "最近 0 条");
    lv_obj_align(s_lbl_log_count, LV_ALIGN_TOP_RIGHT, 0, 2);

    /* 正文 6 行。卡高 130、内边距 16、标题占 24 px，内容区只剩 90 px，
     * 按规格的 17 px 行高排 6 行要 102 px，会被卡片裁掉最后一条。
     * 这里保留 6 行内容和 cjk_16 字号，用 -6 的行距把 20 px 自然行高
     * 压到 14 px：5×14 + 20 = 90 px，正好填满正文区。这是本页唯一的
     * 尺寸取舍，换来「6 条日志全部可见」。 */
    s_lbl_log_body = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "暂无日志");
    lv_obj_set_pos(s_lbl_log_body, 0, 24);
    /* 宽度故意给到 1500（大于卡内容区 944）：日志行已经由 log_ring
     * 截到 155 字节，ASCII 行约 1200 px，如果按 944 宽排版会折成两行，
     * 把第 6 条日志挤出卡片。给足宽度让每条日志保持一个物理行，
     * 超出的部分由卡片的裁剪区切掉，视觉上仍是「一行一条」。 */
    lv_obj_set_size(s_lbl_log_body, 1500, 90);
    lv_label_set_long_mode(s_lbl_log_body, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_line_space(s_lbl_log_body, -6, 0);
}

/* ---------------------------------------------------------------- 刷新 */

static void refresh_network_card(void)
{
    eth_link_status_t eth;
    eth_link_get_status(&eth);

    char buf[128];

    /* 行 1：链路状态。速率只在已知时显示，全双工单独跟一句。 */
    const char *state;
    switch (eth.state) {
    case ETH_STATE_LINK_UP:
        state = "已连接";
        break;
    case ETH_STATE_LINK_DOWN:
        state = "未插网线";
        break;
    case ETH_STATE_STARTING:
    case ETH_STATE_DETECTING:
    case ETH_STATE_IDLE:
        state = "连接中";
        break;
    case ETH_STATE_ERROR:
        state = "故障";
        break;
    default:
        state = "连接中";
        break;
    }

    if (eth.state == ETH_STATE_LINK_UP) {
        char speed[24] = "";
        if (eth.speed_mbps > 0) {
            snprintf(speed, sizeof(speed), " %uM", (unsigned)eth.speed_mbps);
        }
        snprintf(buf, sizeof(buf), "链路 %s%s%s", state, speed,
                 eth.full_duplex ? " 全双工" : "");
    } else if (eth.state == ETH_STATE_ERROR) {
        snprintf(buf, sizeof(buf), "链路 故障 %s", eth.error[0] ? eth.error : "--");
    } else {
        snprintf(buf, sizeof(buf), "链路 %s", state);
    }
    lv_label_set_text(s_lbl_net_link, buf);

    /* 行 2：IP 与掩码，空串显示 "--" */
    snprintf(buf, sizeof(buf), "IP %s | 掩码 %s",
             eth.ip[0] ? eth.ip : "--",
             eth.netmask[0] ? eth.netmask : "--");
    lv_label_set_text(s_lbl_net_ip, buf);

    /* 行 3：网关与 MAC，MAC 固定写成 30:ed:a0:e2:23:6f 形式 */
    snprintf(buf, sizeof(buf),
             "网关 %s | MAC %02x:%02x:%02x:%02x:%02x:%02x",
             eth.gw[0] ? eth.gw : "--",
             eth.mac[0], eth.mac[1], eth.mac[2],
             eth.mac[3], eth.mac[4], eth.mac[5]);
    lv_label_set_text(s_lbl_net_gw, buf);
}

static void refresh_connect_card(void)
{
    mqtt_link_status_t mqtt;
    mqtt_link_get_status(&mqtt);

    char buf[192];

    /* 行 1：中文状态 + broker 主机名 → 解析到的 IP */
    snprintf(buf, sizeof(buf), "%s %s -> %s",
             mqtt_state_text(mqtt.state),
             mqtt.host[0] ? mqtt.host : "--",
             mqtt.ip[0] ? mqtt.ip : "--");
    lv_label_set_text(s_lbl_mqtt_state, buf);

    /* 行 2：总包数 + 按主题分类的计数，能区分 metrics 停了还是 status 停了 */
    snprintf(buf, sizeof(buf), "收包 %u（metrics %u / status %u）",
             (unsigned)mqtt.msg_count,
             (unsigned)mqtt.metrics_count,
             (unsigned)mqtt.status_count);
    lv_label_set_text(s_lbl_mqtt_count, buf);

    /* 行 3：对时状态。没对上时本地时间还是 1970，展示出来只会误导，
     * 所以只在已同步时附上时间。 */
    const time_t now = time(NULL);
    if (now > SETTINGS_CLOCK_VALID_EPOCH) {
        struct tm tm_now;
        char stamp[32] = "--";
        if (localtime_r(&now, &tm_now) != NULL) {
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm_now);
        }
        snprintf(buf, sizeof(buf), "对时 已同步 %s", stamp);
    } else {
        snprintf(buf, sizeof(buf), "对时 未同步");
    }
    lv_label_set_text(s_lbl_clock, buf);
}

static void refresh_firmware_card(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL) {
        lv_label_set_text(s_lbl_fw_version, desc->version);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *label = (running != NULL && running->label[0] != '\0')
                            ? running->label : "--";

    char uptime[48];
    format_uptime(uptime, sizeof(uptime),
                  (uint64_t)(esp_timer_get_time() / 1000000));

    char buf[160];
    snprintf(buf, sizeof(buf), "运行槽 %s | 开机 %s", label, uptime);
    lv_label_set_text(s_lbl_fw_slot, buf);

    eth_link_status_t eth;
    eth_link_get_status(&eth);
    if (eth.ip[0] != '\0') {
        snprintf(buf, sizeof(buf), "OTA 页面 http://%s/", eth.ip);
    } else {
        snprintf(buf, sizeof(buf), "OTA 页面 等待网络");
    }
    lv_label_set_text(s_lbl_fw_ota, buf);

    ota_status_t ota;
    ota_update_get_status(&ota);
    snprintf(buf, sizeof(buf), "更新状态 %s", ota_update_state_name(ota.state));
    lv_label_set_text(s_lbl_fw_update, buf);
}

static void refresh_log_card(void)
{
    const size_t lines = log_ring_snapshot(s_log_buf, sizeof(s_log_buf),
                                           SETTINGS_LOG_LINES);

    if (lines > 0) {
        lv_label_set_text(s_lbl_log_body, s_log_buf);
        lv_label_set_text_fmt(s_lbl_log_count, "最近 %u 条", (unsigned)lines);
        return;
    }

    /* snapshot 返回 0 也可能是这一拍没拿到锁。只有环形缓冲确实为空时
     * 才显示「暂无日志」，否则保留上一拍的文本，避免文字闪烁。 */
    if (log_ring_count() == 0) {
        lv_label_set_text(s_lbl_log_body, "暂无日志");
        lv_label_set_text(s_lbl_log_count, "最近 0 条");
    }
}

static void settings_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 只读互斥快照，不做任何阻塞 IO；滑块和息屏按钮不在这里重置 */
    refresh_network_card();
    refresh_connect_card();
    refresh_firmware_card();
    refresh_log_card();
}

/* ---------------------------------------------------------------- 页面构建 */

void ui_page_settings_build(lv_obj_t *page)
{
    build_display_card(page);
    build_network_card(page);
    build_connect_card(page);
    build_firmware_card(page);
    build_log_card(page);

    lv_timer_create(settings_poll_cb, SETTINGS_POLL_PERIOD_MS, NULL);

    /* 首帧就填上真实数据，避免先闪一下占位文字 */
    settings_poll_cb(NULL);
}