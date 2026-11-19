/*
 * 监控页（S2-d）—— 泰山派 3M 的详细数据
 *
 * 和总览页的分工（方案 §6.2）：
 *   总览 = 多台设备的大致信息（后续接 Windows / 云端）
 *   监控 = 单台设备的细节，页头留了设备切换的位置，后续用按键切设备
 *
 * 本页现在只有一台设备（taishan-3m），所以页头只显示设备名和链路状态，
 * 不放假按钮——等第二台设备真的有数据了再加切换控件。
 *
 * 页面尺寸 = 1024 x (600 - 状态栏 56 - 页签栏 56) = 1024 x 488。
 *
 * 「--」规则沿用总览页：解析成功且未过期才显示数值；字段缺失显示 "--"，
 * 说明文字区分「等待 Hub 数据 / 数据已过期 / 未上报」，绝不把没数据说成 0。
 */

#include "app_ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "device_metrics.h"

#include "fonts/cjk_fonts.h"

#define MON_POLL_PERIOD_MS  (500)

/* 温区格子：3 列 × 2 行，正好放 RK3576 的 6 个温区 */
#define ZONE_COLS           (3)
#define ZONE_ROWS           (2)
#define ZONE_CELLS          (ZONE_COLS * ZONE_ROWS)

/* 服务区最多显示几行：协议里 HUB_SERVICES 现在只有 mosquitto 一个 */
#define SVC_ROWS            (6)

/* ---------------------------------------------------------------- 工具函数 */

/** 温区温度分档上色：>= 85 C 红，>= 70 C 黄，其余正常色 */
static lv_color_t temp_color(double celsius)
{
    if (celsius >= 85.0) {
        return COLOR_ERR;
    }
    if (celsius >= 70.0) {
        return COLOR_WARN;
    }
    return COLOR_TEXT;
}

/**
 * thermal_zone 的 type 原文是 "soc-thermal" / "little-core-thermal" 这种，
 * 去掉统一的 "-thermal" 后缀再上屏，省下来的宽度够多显示两位小数。
 * 不改写其它任何内容——名字必须还是设备上报的那个。
 */
static const char *pretty_zone_name(const char *name, char *buf, size_t len)
{
    const char *suffix = "-thermal";
    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);

    if (name_len > suffix_len &&
        strcmp(name + name_len - suffix_len, suffix) == 0) {
        const size_t keep = name_len - suffix_len;
        if (keep < len) {
            memcpy(buf, name, keep);
            buf[keep] = '\0';
            return buf;
        }
    }
    return name;
}

/** 「--」位置下方的说明文字。绝不能把「没有数据」说成「0」。 */
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

/* ---------------------------------------------------------------- 页头 */

static void build_header(lv_obj_t *page)
{
    s_ctx.lbl_mon_device = ui_label(page, &lv_font_cjk_28, COLOR_ACCENT, "泰山派 3M");
    lv_obj_set_pos(s_ctx.lbl_mon_device, 32, 12);

    /* 设备 id 是协议键，用等宽的 ASCII 显示，方便和日志对得上 */
    lv_obj_t *id = ui_label(page, &lv_font_montserrat_16, COLOR_STALE, "taishan-3m");
    lv_obj_set_pos(id, 32, 46);

    s_ctx.lbl_mon_link = ui_label(page, &lv_font_cjk_20, COLOR_MUTED, "等待数据");
    lv_obj_align(s_ctx.lbl_mon_link, LV_ALIGN_TOP_RIGHT, -32, 20);
}

/* ---------------------------------------------------------------- 温区卡片 */

static void build_zone_card(lv_obj_t *page)
{
    /* 左侧 480 宽：内容区 448 x 208（卡片内边距 16） */
    lv_obj_t *card = ui_card(page, 480, 240);
    lv_obj_set_pos(card, 32, 68);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "温区温度");
    lv_obj_set_pos(title, 0, 0);

    /* 标题占 28 px，剩下 180 px 分两行，每行 90 px */
    const int32_t row_h = 90;
    const int32_t col_w = 149;

    for (int i = 0; i < ZONE_CELLS; i++) {
        const int32_t x = (i % ZONE_COLS) * col_w;
        const int32_t y = 28 + (i / ZONE_COLS) * row_h;

        s_ctx.det_zone[i] = ui_label(card, &lv_font_montserrat_16, COLOR_MUTED, "--");
        lv_obj_set_pos(s_ctx.det_zone[i], x, y);

        s_ctx.val_zone[i] = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
        lv_obj_set_pos(s_ctx.val_zone[i], x, y + 24);
    }
}

/* ---------------------------------------------------------------- 服务卡片 */

static void build_service_card(lv_obj_t *page)
{
    /* 右侧 464 宽：内容区 432 x 208 */
    lv_obj_t *card = ui_card(page, 464, 240);
    lv_obj_set_pos(card, 528, 68);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "系统服务");
    lv_obj_set_pos(title, 0, 0);

    const int32_t row_h = 28;
    for (int i = 0; i < SVC_ROWS; i++) {
        const int32_t y = 34 + i * row_h;

        s_ctx.lbl_svc[i] = ui_label(card, &lv_font_montserrat_20, COLOR_TEXT, "");
        lv_obj_set_pos(s_ctx.lbl_svc[i], 0, y);

        s_ctx.lbl_svc_state[i] = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
        lv_obj_align(s_ctx.lbl_svc_state[i], LV_ALIGN_TOP_RIGHT, 0, y + 2);
    }
}

/* ---------------------------------------------------------------- 底部三卡 */

static void build_load_card(lv_obj_t *page)
{
    lv_obj_t *card = ui_card(page, 304, 140);
    lv_obj_set_pos(card, 32, 324);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "平均负载");
    lv_obj_set_pos(title, 0, 0);

    s_ctx.val_load1 = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_load1, 0, 30);
    s_ctx.val_load5 = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_load5, 96, 30);
    s_ctx.val_load15 = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_load15, 192, 30);

    lv_obj_t *note = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "1 / 5 / 15 分钟");
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void build_memory_card(lv_obj_t *page)
{
    lv_obj_t *card = ui_card(page, 304, 140);
    lv_obj_set_pos(card, 352, 324);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "内存");
    lv_obj_set_pos(title, 0, 0);

    s_ctx.val_mon_mem = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_mon_mem, 0, 26);

    s_ctx.det_mon_mem = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
    lv_obj_set_pos(s_ctx.det_mon_mem, 0, 64);

    /* 没有 swap 分区时协议给 null——这里必须说「未上报」，不能显示 0% */
    s_ctx.val_mon_swap = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "交换 --");
    lv_obj_align(s_ctx.val_mon_swap, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void build_disk_card(lv_obj_t *page)
{
    lv_obj_t *card = ui_card(page, 320, 140);
    lv_obj_set_pos(card, 672, 324);

    /* 挂载点由设备上报（现在固定是 "/"），先给占位，轮询时再替换 */
    s_ctx.lbl_mon_disk_title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "磁盘");
    lv_obj_set_pos(s_ctx.lbl_mon_disk_title, 0, 0);

    s_ctx.val_mon_disk = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_mon_disk, 0, 26);

    s_ctx.det_mon_disk = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
    lv_obj_align(s_ctx.det_mon_disk, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ---------------------------------------------------------------- 轮询 */

static void apply_header(const device_metrics_t *m, bool fresh)
{
    const char *text;
    lv_color_t  color;

    if (m->expired) {
        text  = m->valid ? "数据已过期" : "等待数据";
        color = m->valid ? COLOR_WARN : COLOR_MUTED;
    } else if (!fresh) {
        text  = "等待数据";
        color = COLOR_MUTED;
    } else {
        text  = "数据在线";
        color = COLOR_OK;
    }

    lv_label_set_text(s_ctx.lbl_mon_link, text);
    lv_obj_set_style_text_color(s_ctx.lbl_mon_link, color, 0);
}

static void apply_zones(const device_metrics_t *m, bool fresh)
{
    char name[DEVICE_METRICS_NAME_LEN];
    char value[24];

    for (int i = 0; i < ZONE_CELLS; i++) {
        const bool has = fresh && i < m->zones_len;

        if (has) {
            snprintf(value, sizeof(value), "%.1f C", m->zones[i].celsius);
            lv_label_set_text(s_ctx.det_zone[i],
                              pretty_zone_name(m->zones[i].name, name, sizeof(name)));
            lv_obj_set_style_text_color(s_ctx.det_zone[i], COLOR_MUTED, 0);
            lv_obj_set_style_text_color(s_ctx.val_zone[i],
                                        temp_color(m->zones[i].celsius), 0);
        } else {
            snprintf(value, sizeof(value), "--");
            /* 没数据时名字也留空，避免出现「soc 46.5 C」这种半真半假的格子 */
            lv_label_set_text(s_ctx.det_zone[i], "--");
            lv_obj_set_style_text_color(s_ctx.det_zone[i], COLOR_STALE, 0);
            lv_obj_set_style_text_color(s_ctx.val_zone[i], COLOR_STALE, 0);
        }
        lv_label_set_text(s_ctx.val_zone[i], value);
    }
}

static void apply_services(const device_metrics_t *m, bool fresh)
{
    for (int i = 0; i < SVC_ROWS; i++) {
        const bool has = fresh && i < m->services_len;

        if (!has) {
            lv_label_set_text(s_ctx.lbl_svc[i], "");
            lv_label_set_text(s_ctx.lbl_svc_state[i], "");
            continue;
        }

        lv_label_set_text(s_ctx.lbl_svc[i], m->services[i].name);

        /* ActiveState 是 systemd 的英文原值；只翻译给人看，不改变含义 */
        const char *state = m->services[i].state;
        const char *text = state;
        lv_color_t  color = COLOR_MUTED;

        if (strcmp(state, "active") == 0) {
            text = "运行中"; color = COLOR_OK;
        } else if (strcmp(state, "failed") == 0) {
            text = "失败"; color = COLOR_ERR;
        } else if (strcmp(state, "inactive") == 0) {
            text = "已停止"; color = COLOR_MUTED;
        } else if (strcmp(state, "activating") == 0) {
            text = "启动中"; color = COLOR_WARN;
        } else if (strcmp(state, "deactivating") == 0) {
            text = "停止中"; color = COLOR_WARN;
        } else if (strcmp(state, "reloading") == 0) {
            text = "重载中"; color = COLOR_WARN;
        }

        lv_label_set_text(s_ctx.lbl_svc_state[i], text);
        lv_obj_set_style_text_color(s_ctx.lbl_svc_state[i], color, 0);
    }
}

static void apply_bottom(const device_metrics_t *m, bool fresh)
{
    char buf[64];

    /* ---- 负载 1 / 5 / 15 ---- */
    const bool has_load = fresh && m->has_load;
    lv_label_set_text_fmt(s_ctx.val_load1, has_load ? "%.2f" : "--", m->load1);
    lv_label_set_text_fmt(s_ctx.val_load5, has_load ? "%.2f" : "--", m->load5);
    lv_label_set_text_fmt(s_ctx.val_load15, has_load ? "%.2f" : "--", m->load15);
    lv_obj_set_style_text_color(s_ctx.val_load1, has_load ? COLOR_TEXT : COLOR_STALE, 0);
    lv_obj_set_style_text_color(s_ctx.val_load5, has_load ? COLOR_TEXT : COLOR_STALE, 0);
    lv_obj_set_style_text_color(s_ctx.val_load15, has_load ? COLOR_TEXT : COLOR_STALE, 0);

    /* ---- 内存 ---- */
    const bool has_mem = fresh && m->has_mem;
    if (has_mem) {
        snprintf(buf, sizeof(buf), "%.1f%%", m->mem_percent);
        lv_label_set_text(s_ctx.val_mon_mem, buf);
        if (!ui_format_bytes_pair(buf, sizeof(buf), m->mem_used_bytes, m->mem_total_bytes)) {
            snprintf(buf, sizeof(buf), "已占用");
        }
        lv_label_set_text(s_ctx.det_mon_mem, buf);
    } else {
        lv_label_set_text(s_ctx.val_mon_mem, "--");
        lv_label_set_text(s_ctx.det_mon_mem, placeholder_detail(m));
    }
    lv_obj_set_style_text_color(s_ctx.val_mon_mem, has_mem ? COLOR_OK : COLOR_STALE, 0);

    /* ---- 交换分区：has_swap=false 表示设备没有 swap，不是 0% ---- */
    if (fresh && m->has_swap) {
        lv_label_set_text_fmt(s_ctx.val_mon_swap, "交换 %.1f%%", m->swap_used_percent);
    } else {
        lv_label_set_text(s_ctx.val_mon_swap, "交换 --");
    }

    /* ---- 磁盘 ---- */
    const bool has_disk = fresh && m->has_disk;
    if (has_disk) {
        snprintf(buf, sizeof(buf), "%.1f%%", m->disk_percent);
        lv_label_set_text(s_ctx.val_mon_disk, buf);
        if (!ui_format_bytes_pair(buf, sizeof(buf), m->disk_used_bytes, m->disk_total_bytes)) {
            snprintf(buf, sizeof(buf), "已占用");
        }
        lv_label_set_text(s_ctx.det_mon_disk, buf);
    } else {
        lv_label_set_text(s_ctx.val_mon_disk, "--");
        lv_label_set_text(s_ctx.det_mon_disk, placeholder_detail(m));
    }
    lv_obj_set_style_text_color(s_ctx.val_mon_disk,
                                has_disk ? (m->disk_percent >= 85.0 ? COLOR_ERR :
                                            m->disk_percent >= 70.0 ? COLOR_WARN : COLOR_TEXT)
                                         : COLOR_STALE,
                                0);

    if (m->disk_path[0] != '\0') {
        char title[48];
        snprintf(title, sizeof(title), "磁盘 %s", m->disk_path);
        lv_label_set_text(s_ctx.lbl_mon_disk_title, title);
    } else {
        lv_label_set_text(s_ctx.lbl_mon_disk_title, "磁盘");
    }
}

static void monitor_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    device_metrics_t m;
    device_metrics_get(&m);

    /* 可以当当前值显示 = 解析成功且未过期 */
    const bool fresh = m.valid && !m.expired;

    apply_header(&m, fresh);
    apply_zones(&m, fresh);
    apply_services(&m, fresh);
    apply_bottom(&m, fresh);
}

/* ---------------------------------------------------------------- 页面构建 */

void ui_page_monitor_build(lv_obj_t *page)
{
    build_header(page);
    build_zone_card(page);
    build_service_card(page);
    build_load_card(page);
    build_memory_card(page);
    build_disk_card(page);

    lv_timer_create(monitor_poll_cb, MON_POLL_PERIOD_MS, NULL);

    /* 首帧就填上真实数据，避免开机先闪一下占位文字 */
    monitor_poll_cb(NULL);
}