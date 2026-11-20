/*
 * 监控页（S4-b 多设备）→ S5 卡面重排 → S6 能力自适应
 *
 * 和总览页的分工（方案 §6.2）：
 *   总览 = 多台设备的大致信息
 *   监控 = 单台设备的细节，页头用 ◀ ▶ 按钮切换查看哪一台
 *
 * 本页的布局是「固定坐标 + 按能力切换卡面」，不是给每台设备写一套页面：
 *
 *   480x240 @ (32,68)   CPU / GPU           —— 两台设备都有（GPU 缺失时逐项显示 --）
 *   464x240 @ (528,68)  进程   / 温区温度    —— 有 processes 就显示进程，否则回落温区
 *   304x140 @ (32,324)  网络   / 平均负载    —— 有 network 就显示网络，否则回落 load/uptime
 *   304x140 @ (352,324) 内存                —— 两台都有
 *   320x140 @ (672,324) 磁盘（含分区列表）   —— 两台都有
 *
 * 两张卡放在同一坐标、同时只显示一张：切设备时只翻转 LV_OBJ_FLAG_HIDDEN，
 * 不重建控件、不改坐标，所以不会闪，也不需要为「每台设备一套页面」维护 N 份代码。
 *
 * 系统服务卡已按需求移除（服务状态不再上屏，Agent 仍照常上报，协议未删）。
 *
 * 槽位越界或该设备没有上报某类数据时，相应区域显示「该设备未上报此项目」，
 * 不说成 0 也不留上一台的残影。
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
#include "device_registry.h"
#include "device_status.h"

#include "fonts/cjk_fonts.h"

#define MON_POLL_PERIOD_MS  (500)

/* CPU/GPU 卡：卡片 480x240，内边距 16 → 内容区 448x208。
 * 两列各 216 宽，中间留 16 宽的缝给竖分隔线。 */
#define CG_COL_W            (216)
#define CG_COL2_X           (232)
#define CG_BAR_H            (10)

/* 进程卡 / 温区卡：卡片 464x240，内容区 432x208 */
#define PROC_ROWS           (3)
#define ZONE_COLS           (3)
#define ZONE_ROWS           (2)
#define ZONE_CELLS          (ZONE_COLS * ZONE_ROWS)

/* 进度条底槽与分隔线，比卡片亮一点点就够，太亮会抢大字的注意力 */
#define COLOR_TRACK         lv_color_hex(0x27344a)
#define COLOR_SEP           lv_color_hex(0x2c3a52)

/* ---------------------------------------------------------------- 工具函数 */

/** 温度分档上色：>= 85 C 红，>= 70 C 黄，其余正常色 */
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

/**
 * 显卡全名 "NVIDIA GeForce RTX 4060 Laptop GPU" 放不进右上角那一行，
 * 去掉厂商前缀就够了（"RTX 4060 Laptop GPU"）。只做前缀剥离，
 * 不做其它改写——屏幕上显示的仍然是设备报上来的型号。
 */
static const char *short_gpu_name(const char *full)
{
    static const char *const prefixes[] = { "NVIDIA GeForce ", "NVIDIA " };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        const size_t n = strlen(prefixes[i]);
        if (strncmp(full, prefixes[i], n) == 0 && full[n] != '\0') {
            return full + n;
        }
    }
    return full;
}

/** thermal_zone 的 type 原文去掉统一的 "-thermal" 后缀再上屏 */
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

/** 运行时长：秒 → 「3 天 5 小时」/「5 小时 12 分」/「12 分」 */
static void format_uptime(char *buf, size_t len, uint32_t seconds)
{
    const uint32_t days  = seconds / 86400u;
    const uint32_t hours = (seconds % 86400u) / 3600u;
    const uint32_t mins  = (seconds % 3600u) / 60u;

    if (days > 0) {
        snprintf(buf, len, "%u 天 %u 小时", (unsigned)days, (unsigned)hours);
    } else if (hours > 0) {
        snprintf(buf, len, "%u 小时 %u 分", (unsigned)hours, (unsigned)mins);
    } else {
        snprintf(buf, len, "%u 分", (unsigned)mins);
    }
}

/** 字节/秒 → 「1.2 MB/s」；B 以下用整数，避免出现 0.0 KB/s 这种噪声 */
static void format_rate(char *buf, size_t len, double bytes_per_sec)
{
    const double kb = 1024.0;
    const double mb = kb * 1024.0;

    if (bytes_per_sec >= mb) {
        snprintf(buf, len, "%.1f MB/s", bytes_per_sec / mb);
    } else if (bytes_per_sec >= kb) {
        snprintf(buf, len, "%.1f KB/s", bytes_per_sec / kb);
    } else {
        snprintf(buf, len, "%.0f B/s", bytes_per_sec);
    }
}

/** 一条 216x10 的细进度条；轨道用 COLOR_TRACK，进度用 COLOR_ACCENT */
static lv_obj_t *make_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, CG_COL_W, CG_BAR_H);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

/** 百分比 → 进度条整数值；越界夹到 0~100，负数不能画成满格 */
static int32_t bar_value(double percent)
{
    if (percent < 0.0) {
        return 0;
    }
    if (percent > 100.0) {
        return 100;
    }
    return (int32_t)(percent + 0.5);
}

/** 固定宽度的右对齐数字列：label 宽度不随文字变化，切设备时列不会左右跳 */
static lv_obj_t *right_label(lv_obj_t *parent, const lv_font_t *font,
                             lv_color_t color, const char *text, int32_t x,
                             int32_t w)
{
    lv_obj_t *label = ui_label(parent, font, color, text);
    lv_obj_set_pos(label, x, 0);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    return label;
}

static void monitor_poll_cb(lv_timer_t *timer);

/* ---------------------------------------------------------------- 设备切换 */

/**
 * ◀ / ▶ 只改 s_mon_slot，由下一次轮询把整页刷新成目标设备的数据。
 * 全程只碰一个 int，不用在事件回调里做重排或重建控件。
 */
static void switch_device(int step)
{
    const int count = device_registry_count();
    if (count <= 0) {
        s_mon_slot = 0;
        return;
    }
    /* 循环切换：最后一台再点 ▶ 回到第一台 */
    s_mon_slot = (s_mon_slot + step) % count;
    if (s_mon_slot < 0) {
        s_mon_slot += count;
    }
}

static void btn_prev_cb(lv_event_t *e)
{
    (void)e;
    switch_device(-1);
    /* 立刻刷新，不用等最多 500 ms 的下一个轮询周期 */
    monitor_poll_cb(NULL);
}

static void btn_next_cb(lv_event_t *e)
{
    (void)e;
    switch_device(+1);
    monitor_poll_cb(NULL);
}

/* ---------------------------------------------------------------- 页头 */

static void build_header(lv_obj_t *page)
{
    /* ◀ 按钮：切到上一个设备（循环） */
    s_ctx.btn_mon_prev = lv_button_create(page);
    lv_obj_remove_style_all(s_ctx.btn_mon_prev);
    lv_obj_set_size(s_ctx.btn_mon_prev, 56, 56);
    lv_obj_set_pos(s_ctx.btn_mon_prev, 32, 8);
    lv_obj_set_style_radius(s_ctx.btn_mon_prev, 12, 0);
    lv_obj_set_style_bg_color(s_ctx.btn_mon_prev, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_ctx.btn_mon_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ctx.btn_mon_prev, COLOR_TEXT, 0);
    lv_obj_set_style_shadow_width(s_ctx.btn_mon_prev, 0, 0);

    lv_obj_t *lbl_prev = lv_label_create(s_ctx.btn_mon_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_prev);
    lv_obj_add_event_cb(s_ctx.btn_mon_prev, btn_prev_cb, LV_EVENT_CLICKED, NULL);

    /* ▶ 按钮：切到下一个设备（循环） */
    s_ctx.btn_mon_next = lv_button_create(page);
    lv_obj_remove_style_all(s_ctx.btn_mon_next);
    lv_obj_set_size(s_ctx.btn_mon_next, 56, 56);
    lv_obj_set_pos(s_ctx.btn_mon_next, 96, 8);
    lv_obj_set_style_radius(s_ctx.btn_mon_next, 12, 0);
    lv_obj_set_style_bg_color(s_ctx.btn_mon_next, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_ctx.btn_mon_next, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ctx.btn_mon_next, COLOR_TEXT, 0);
    lv_obj_set_style_shadow_width(s_ctx.btn_mon_next, 0, 0);

    lv_obj_t *lbl_next = lv_label_create(s_ctx.btn_mon_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_next);
    lv_obj_add_event_cb(s_ctx.btn_mon_next, btn_next_cb, LV_EVENT_CLICKED, NULL);

    /* 设备名 + id：随 s_mon_slot 变化 */
    s_ctx.lbl_mon_device = ui_label(page, &lv_font_cjk_28, COLOR_ACCENT, "");
    lv_obj_set_pos(s_ctx.lbl_mon_device, 168, 4);

    s_ctx.lbl_mon_id = ui_label(page, &lv_font_montserrat_16, COLOR_STALE, "");
    lv_obj_set_pos(s_ctx.lbl_mon_id, 168, 40);

    /* 右侧上：状态；右侧下：数据年龄。两行都要，缺一个都会让人误判新鲜度 */
    s_ctx.lbl_mon_link = ui_label(page, &lv_font_cjk_20, COLOR_MUTED, "等待数据");
    lv_obj_align(s_ctx.lbl_mon_link, LV_ALIGN_TOP_RIGHT, -32, 4);

    s_ctx.lbl_mon_age = ui_label(page, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_align(s_ctx.lbl_mon_age, LV_ALIGN_TOP_RIGHT, -32, 32);
}

/* ---------------------------------------------------------------- CPU / GPU 卡 */

static void build_cpu_gpu_card(lv_obj_t *page)
{
    /* 左侧 480 宽：内容区 448 x 208（卡片内边距 16） */
    lv_obj_t *card = ui_card(page, 480, 240);
    lv_obj_set_pos(card, 32, 68);

    lv_obj_t *title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "CPU / GPU");
    lv_obj_set_pos(title, 0, 0);
    s_ctx.lbl_cg_title = title;

    /* 右上角：显卡短名。取不到就留空，不写「未知显卡」占地方 */
    s_ctx.lbl_cg_gpu_name = ui_label(card, &lv_font_montserrat_14, COLOR_STALE, "");
    lv_obj_align(s_ctx.lbl_cg_gpu_name, LV_ALIGN_TOP_RIGHT, 0, 3);

    /* 竖分隔线：两列各占 216，中间 16 的缝 */
    lv_obj_t *sep = lv_obj_create(card);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, CG_COL2_X - 8, 34);
    lv_obj_set_size(sep, 1, 146);
    lv_obj_set_style_bg_color(sep, COLOR_SEP, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    /* ---- 左列：CPU ---- */
    lv_obj_t *cap_cpu = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "CPU 占用");
    lv_obj_set_pos(cap_cpu, 0, 28);

    s_ctx.val_cpu_big = ui_label(card, &lv_font_montserrat_36, COLOR_ACCENT, "--");
    lv_obj_set_pos(s_ctx.val_cpu_big, 0, 44);

    s_ctx.bar_cpu = make_bar(card);
    lv_obj_set_pos(s_ctx.bar_cpu, 0, 104);

    s_ctx.det_cpu = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
    lv_obj_set_pos(s_ctx.det_cpu, 0, 132);

    s_ctx.det_cpu2 = ui_label(card, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_set_pos(s_ctx.det_cpu2, 0, 162);

    /* ---- 右列：GPU（整列放进容器，方便和容器列整体切换） ---- */
    s_ctx.gpu_col = lv_obj_create(card);
    lv_obj_remove_style_all(s_ctx.gpu_col);
    lv_obj_remove_flag(s_ctx.gpu_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ctx.gpu_col, CG_COL2_X, 0);
    lv_obj_set_size(s_ctx.gpu_col, CG_COL_W, 208);

    lv_obj_t *cap_gpu = ui_label(s_ctx.gpu_col, &lv_font_cjk_16, COLOR_MUTED, "GPU 利用率");
    lv_obj_set_pos(cap_gpu, 0, 28);

    s_ctx.val_gpu_big = ui_label(s_ctx.gpu_col, &lv_font_montserrat_36, COLOR_ACCENT, "--");
    lv_obj_set_pos(s_ctx.val_gpu_big, 0, 44);

    s_ctx.bar_gpu = make_bar(s_ctx.gpu_col);
    lv_obj_set_pos(s_ctx.bar_gpu, 0, 104);

    /* 温度：标题两个字用小字，数值放大到 28 px —— 这是这张卡最该被远看的东西 */
    s_ctx.lbl_gpu_temp_cap = ui_label(s_ctx.gpu_col, &lv_font_cjk_16, COLOR_MUTED, "温度");
    lv_obj_set_pos(s_ctx.lbl_gpu_temp_cap, 0, 134);

    s_ctx.val_gpu_temp = ui_label(s_ctx.gpu_col, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_gpu_temp, 44, 126);

    s_ctx.det_gpu_mem = ui_label(s_ctx.gpu_col, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_set_pos(s_ctx.det_gpu_mem, 0, 168);

    /* ---- 同一坐标的容器列：没有 GPU 的机器（泰山派）显示运行中的容器 ---- */
    s_ctx.cont_col = lv_obj_create(card);
    lv_obj_remove_style_all(s_ctx.cont_col);
    lv_obj_remove_flag(s_ctx.cont_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ctx.cont_col, CG_COL2_X, 0);
    lv_obj_set_size(s_ctx.cont_col, CG_COL_W, 208);
    lv_obj_add_flag(s_ctx.cont_col, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *cap_cont = ui_label(s_ctx.cont_col, &lv_font_cjk_16, COLOR_MUTED, "运行容器");
    lv_obj_set_pos(cap_cont, 0, 28);

    s_ctx.lbl_cont_count = ui_label(s_ctx.cont_col, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_align(s_ctx.lbl_cont_count, LV_ALIGN_TOP_RIGHT, 0, 28);

    for (int i = 0; i < 4; i++) {
        const int32_t y = 58 + i * 34;

        lv_obj_t *dot = lv_obj_create(s_ctx.cont_col);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_color(dot, COLOR_OK, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 0, y + 4);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_ctx.dot_cont[i] = dot;

        s_ctx.lbl_cont_name[i] = ui_label(s_ctx.cont_col, &lv_font_montserrat_16, COLOR_TEXT, "");
        lv_obj_set_pos(s_ctx.lbl_cont_name[i], 18, y);
        lv_obj_set_width(s_ctx.lbl_cont_name[i], CG_COL_W - 18);
        lv_label_set_long_mode(s_ctx.lbl_cont_name[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_ctx.lbl_cont_name[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 一只容器都没有 / 压根读不到 docker 时的说明，避免空列 */
    s_ctx.lbl_cont_note = ui_label(s_ctx.cont_col, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_set_pos(s_ctx.lbl_cont_note, 0, 60);
}

/* ---------------------------------------------------------------- 进程卡 */

static void build_process_card(lv_obj_t *page)
{
    /* 右侧 464 宽：内容区 432 x 208 */
    s_ctx.card_proc = ui_card(page, 464, 240);
    lv_obj_set_pos(s_ctx.card_proc, 528, 68);

    s_ctx.lbl_proc_title = ui_label(s_ctx.card_proc, &lv_font_cjk_16, COLOR_MUTED, "进程");
    lv_obj_set_pos(s_ctx.lbl_proc_title, 0, 0);

    s_ctx.lbl_proc_hint = ui_label(s_ctx.card_proc, &lv_font_cjk_16, COLOR_STALE,
                                  "按内存排序");
    lv_obj_align(s_ctx.lbl_proc_hint, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* 表头：名字左对齐，CPU / 内存固定宽右对齐（列宽不随内容跳动） */
    const char *heads[3] = { "进程", "CPU", "内存" };
    const int32_t head_x[3] = { 0, 276, 352 };
    for (int i = 0; i < 3; i++) {
        if (i == 0) {
            s_ctx.lbl_proc_th[i] = ui_label(s_ctx.card_proc, &lv_font_cjk_16,
                                            COLOR_STALE, heads[i]);
            lv_obj_set_pos(s_ctx.lbl_proc_th[i], head_x[i], 30);
        } else {
            s_ctx.lbl_proc_th[i] = right_label(s_ctx.card_proc, &lv_font_cjk_16,
                                               COLOR_STALE, heads[i], head_x[i],
                                               i == 1 ? 60 : 80);
            lv_obj_set_pos(s_ctx.lbl_proc_th[i], head_x[i], 30);
        }
    }

    for (int i = 0; i < PROC_ROWS; i++) {
        const int32_t y = 54 + i * 36;

        s_ctx.lbl_proc_name[i] = ui_label(s_ctx.card_proc, &lv_font_montserrat_20,
                                          COLOR_TEXT, "");
        lv_obj_set_pos(s_ctx.lbl_proc_name[i], 0, y);

        s_ctx.lbl_proc_cpu[i] = right_label(s_ctx.card_proc, &lv_font_montserrat_16,
                                            COLOR_ACCENT, "", 276, 60);
        lv_obj_set_pos(s_ctx.lbl_proc_cpu[i], 276, y + 3);

        s_ctx.lbl_proc_mem[i] = right_label(s_ctx.card_proc, &lv_font_montserrat_16,
                                            COLOR_MUTED, "", 352, 80);
        lv_obj_set_pos(s_ctx.lbl_proc_mem[i], 352, y + 3);
    }

    /* 分隔线 + 页脚：告诉人「只列了前 3 个」，别以为是全部 */
    lv_obj_t *div = lv_obj_create(s_ctx.card_proc);
    lv_obj_remove_style_all(div);
    lv_obj_set_pos(div, 0, 164);
    lv_obj_set_size(div, 432, 1);
    lv_obj_set_style_bg_color(div, COLOR_SEP, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    s_ctx.lbl_proc_foot = ui_label(s_ctx.card_proc, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_set_pos(s_ctx.lbl_proc_foot, 0, 174);
}

/* ---------------------------------------------------------------- 温区卡（回退） */

static void build_zone_card(lv_obj_t *page)
{
    /* 与进程卡同坐标、同尺寸；默认隐藏，只有设备没有进程数据时才露出来 */
    s_ctx.card_zone = ui_card(page, 464, 240);
    lv_obj_set_pos(s_ctx.card_zone, 528, 68);
    lv_obj_add_flag(s_ctx.card_zone, LV_OBJ_FLAG_HIDDEN);

    s_ctx.lbl_zone_title = ui_label(s_ctx.card_zone, &lv_font_cjk_16, COLOR_MUTED,
                                    "温区温度");
    lv_obj_set_pos(s_ctx.lbl_zone_title, 0, 0);

    const int32_t col_w = 144;
    const int32_t row_h = 84;
    for (int i = 0; i < ZONE_CELLS; i++) {
        const int32_t x = (i % ZONE_COLS) * col_w;
        const int32_t y = 30 + (i / ZONE_COLS) * row_h;

        s_ctx.lbl_zone_name[i] = ui_label(s_ctx.card_zone, &lv_font_cjk_16,
                                          COLOR_MUTED, "--");
        lv_obj_set_pos(s_ctx.lbl_zone_name[i], x, y);

        s_ctx.lbl_zone_val[i] = ui_label(s_ctx.card_zone, &lv_font_montserrat_28,
                                         COLOR_STALE, "--");
        lv_obj_set_pos(s_ctx.lbl_zone_val[i], x, y + 22);
    }
}

/* ---------------------------------------------------------------- 网络卡 */

static void build_network_card(lv_obj_t *page)
{
    s_ctx.card_net = ui_card(page, 304, 140);
    lv_obj_set_pos(s_ctx.card_net, 32, 324);

    lv_obj_t *title = ui_label(s_ctx.card_net, &lv_font_cjk_16, COLOR_MUTED, "网络");
    lv_obj_set_pos(title, 0, 0);

    s_ctx.lbl_mon_net_ip = ui_label(s_ctx.card_net, &lv_font_montserrat_20,
                                COLOR_ACCENT, "--");
    lv_obj_set_pos(s_ctx.lbl_mon_net_ip, 0, 24);

    s_ctx.lbl_mon_net_rx = ui_label(s_ctx.card_net, &lv_font_cjk_16, COLOR_OK, "");
    lv_obj_set_pos(s_ctx.lbl_mon_net_rx, 0, 56);

    s_ctx.lbl_mon_net_tx = ui_label(s_ctx.card_net, &lv_font_cjk_16, COLOR_WARN, "");
    lv_obj_set_pos(s_ctx.lbl_mon_net_tx, 136, 56);

    s_ctx.lbl_mon_net_note = ui_label(s_ctx.card_net, &lv_font_cjk_16, COLOR_STALE,
                                  "局域网");
    lv_obj_align(s_ctx.lbl_mon_net_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ---------------------------------------------------------------- 负载卡（回退） */

/**
 * 左下卡：有 loadavg 的设备显示 1/5/15；没有的（Windows）改显示运行时长。
 * 网络数据缺失时也会回落到这张卡，所以它同时是一个「兜底卡」。
 */
static void build_load_card(lv_obj_t *page)
{
    lv_obj_t *card = ui_card(page, 304, 140);
    lv_obj_set_pos(card, 32, 324);
    s_ctx.card_load = card;

    s_ctx.lbl_load_title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "平均负载");
    lv_obj_set_pos(s_ctx.lbl_load_title, 0, 0);

    s_ctx.val_load1 = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_load1, 0, 30);
    s_ctx.val_load5 = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_load5, 96, 30);
    s_ctx.val_load15 = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_load15, 192, 30);

    /* 运行时长是中文+数字，用 cjk_28；默认隐藏，只有 loadavg 缺失时才出现 */
    s_ctx.val_uptime = ui_label(card, &lv_font_cjk_28, COLOR_TEXT, "");
    lv_obj_set_pos(s_ctx.val_uptime, 0, 26);
    lv_obj_add_flag(s_ctx.val_uptime, LV_OBJ_FLAG_HIDDEN);

    s_ctx.lbl_load_note = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "1 / 5 / 15 分钟");
    lv_obj_align(s_ctx.lbl_load_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ---------------------------------------------------------------- 内存卡 */

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

/* ---------------------------------------------------------------- 磁盘卡 */

static void build_disk_card(lv_obj_t *page)
{
    lv_obj_t *card = ui_card(page, 320, 140);
    lv_obj_set_pos(card, 672, 324);

    /* 挂载点由设备上报（Windows 是 "C:\"，Linux 是 "/"），先给占位 */
    s_ctx.lbl_mon_disk_title = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "磁盘");
    lv_obj_set_pos(s_ctx.lbl_mon_disk_title, 0, 0);

    /* 标题右侧「共 3 个分区」；没有 disks[] 的设备不显示 */
    s_ctx.lbl_disk_parts = ui_label(card, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_align(s_ctx.lbl_disk_parts, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_ctx.val_mon_disk = ui_label(card, &lv_font_montserrat_28, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.val_mon_disk, 0, 26);

    s_ctx.det_mon_disk = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
    lv_obj_set_pos(s_ctx.det_mon_disk, 0, 64);

    /* 底部一行列其余分区的占用率（D: 41.1%  S: 27.7%） */
    s_ctx.det_disk_parts = ui_label(card, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_align(s_ctx.det_disk_parts, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ---------------------------------------------------------------- 轮询 */

/** 该槽位当前应该显示哪台设备。槽位越界时填空 id（UI 显示「无设备」） */
static bool current_device(device_registry_entry_t *entry)
{
    const int count = device_registry_count();
    if (count == 0) {
        memset(entry, 0, sizeof(*entry));
        return false;
    }
    /* s_mon_slot 是按钮改的自由值，设备数变化后可能越界，取模兜底 */
    if (s_mon_slot < 0 || s_mon_slot >= count) {
        s_mon_slot = 0;
    }
    device_registry_get(s_mon_slot, entry);
    return entry->id[0] != '\0';
}

static void apply_header(const device_registry_entry_t *entry,
                         const device_metrics_t *m, const device_status_t *st,
                         bool fresh)
{
    /* 未登记设备没有中文名，显示协议 id；完全没有设备时给明确占位 */
    lv_label_set_text(s_ctx.lbl_mon_device,
                      entry->name[0] ? entry->name :
                      entry->id[0]   ? entry->id   : "无设备");
    lv_label_set_text(s_ctx.lbl_mon_id, entry->id[0] ? entry->id : "-");

    const char *text;
    lv_color_t  color;

    if (!entry->id[0]) {
        text  = "未接入设备";
        color = COLOR_MUTED;
    } else if (st->ever_received && st->state == DEVICE_STATUS_OFFLINE) {
        text  = "设备离线";
        color = COLOR_ERR;
    } else if (m->expired) {
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

    /* 数据年龄：只在这台设备真的收到过帧时才有意义 */
    if (!m->valid) {
        lv_label_set_text(s_ctx.lbl_mon_age, "尚无数据");
    } else {
        lv_label_set_text_fmt(s_ctx.lbl_mon_age, "更新于 %.1f s 前",
                              (double)m->age_ms / 1000.0);
    }
}

/** 右半列的容器列表：只列前 4 个，数量用 container_count 表达 */
static void apply_containers(const device_metrics_t *m, bool fresh)
{
    char buf[32];
    if (m->has_container_count) {
        snprintf(buf, sizeof(buf), "%d 个", m->container_count);
        lv_label_set_text(s_ctx.lbl_cont_count, buf);
    } else {
        lv_label_set_text(s_ctx.lbl_cont_count, "");
    }

    const bool has_list = fresh && m->containers_len > 0;
    for (int i = 0; i < 4; i++) {
        if (has_list && i < m->containers_len) {
            /* compose 起的容器显示服务名（app / postgres），比 27 字符的容器名好认 */
            const char *label = m->containers[i].service[0] != '\0'
                              ? m->containers[i].service
                              : m->containers[i].name;
            lv_label_set_text(s_ctx.lbl_cont_name[i], label);
            lv_obj_remove_flag(s_ctx.lbl_cont_name[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ctx.dot_cont[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ctx.lbl_cont_name[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ctx.dot_cont[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (has_list) {
        lv_label_set_text(s_ctx.lbl_cont_note, "");
    } else if (m->valid && m->has_container_count && m->container_count == 0) {
        lv_label_set_text(s_ctx.lbl_cont_note, "当前没有运行中的容器");
    } else {
        lv_label_set_text(s_ctx.lbl_cont_note, placeholder_detail(m));
    }
}

/** CPU/GPU 卡：所有「--」都带一句为什么没有，避免和真实的 0 混淆 */
static void apply_cpu_gpu(const device_metrics_t *m, bool fresh)
{
    char buf[64];

    /* ---- CPU ---- */
    const bool has_cpu = fresh && m->has_cpu;
    if (has_cpu) {
        snprintf(buf, sizeof(buf), "%.1f%%", m->cpu_percent);
        lv_label_set_text(s_ctx.val_cpu_big, buf);
        lv_bar_set_value(s_ctx.bar_cpu, bar_value(m->cpu_percent), LV_ANIM_OFF);

        if (m->has_cpu_physical && m->cpu_physical_cores > 0 && m->cpu_cores > 0) {
            snprintf(buf, sizeof(buf), "%d 核 / %d 线程",
                     m->cpu_physical_cores, m->cpu_cores);
        } else if (m->cpu_cores > 0) {
            snprintf(buf, sizeof(buf), "%d 线程", m->cpu_cores);
        } else {
            buf[0] = '\0';
        }
        lv_label_set_text(s_ctx.det_cpu, buf);
        lv_label_set_text(s_ctx.det_cpu2, "采样 2 s");
    } else {
        lv_label_set_text(s_ctx.val_cpu_big, "--");
        lv_bar_set_value(s_ctx.bar_cpu, 0, LV_ANIM_OFF);
        lv_label_set_text(s_ctx.det_cpu, placeholder_detail(m));
        lv_label_set_text(s_ctx.det_cpu2, "");
    }
    lv_obj_set_style_text_color(s_ctx.val_cpu_big, has_cpu ? COLOR_ACCENT : COLOR_STALE, 0);
    lv_obj_set_style_text_color(s_ctx.det_cpu, has_cpu ? COLOR_MUTED : COLOR_STALE, 0);

    /* ---- 右半列：有 GPU 走 GPU 列，没有 GPU 的机器走容器列 ---- */
    const bool has_gpu_dev = m->valid &&
        (m->gpu_name[0] != '\0' || m->has_gpu ||
         m->has_gpu_usage || m->has_gpu_temp);
    lv_label_set_text(s_ctx.lbl_cg_title, has_gpu_dev ? "CPU / GPU" : "CPU / 容器");
    if (has_gpu_dev) {
        lv_obj_remove_flag(s_ctx.gpu_col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.cont_col, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ctx.gpu_col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.cont_col, LV_OBJ_FLAG_HIDDEN);
        apply_containers(m, fresh);
    }

    /* ---- GPU ---- */
    /* 显卡名只要有就显示，哪怕这一帧的利用率/温度读不到：
     * 名字来自 nvidia-smi 成功执行过的那一次，说的是「这台有这块卡」。 */
    if (m->valid && m->gpu_name[0] != '\0') {
        lv_label_set_text(s_ctx.lbl_cg_gpu_name, short_gpu_name(m->gpu_name));
    } else {
        lv_label_set_text(s_ctx.lbl_cg_gpu_name, "");
    }

    const bool has_gpu_usage = fresh && m->has_gpu_usage;
    if (has_gpu_usage) {
        snprintf(buf, sizeof(buf), "%.0f%%", m->gpu_percent);
        lv_label_set_text(s_ctx.val_gpu_big, buf);
        lv_bar_set_value(s_ctx.bar_gpu, bar_value(m->gpu_percent), LV_ANIM_OFF);
    } else {
        lv_label_set_text(s_ctx.val_gpu_big, "--");
        lv_bar_set_value(s_ctx.bar_gpu, 0, LV_ANIM_OFF);
    }
    lv_obj_set_style_text_color(s_ctx.val_gpu_big,
                                has_gpu_usage ? COLOR_ACCENT : COLOR_STALE, 0);

    /* 温度单独判定：利用率有、温度没有（或反过来）都要如实分开展示 */
    const bool has_gpu_temp = fresh && m->has_gpu_temp;
    if (has_gpu_temp) {
        snprintf(buf, sizeof(buf), "%.0f C", m->gpu_celsius);
        lv_label_set_text(s_ctx.val_gpu_temp, buf);
        lv_obj_set_style_text_color(s_ctx.val_gpu_temp, temp_color(m->gpu_celsius), 0);
    } else {
        lv_label_set_text(s_ctx.val_gpu_temp, "--");
        lv_obj_set_style_text_color(s_ctx.val_gpu_temp, COLOR_STALE, 0);
    }

    const bool has_gpu_mem = fresh && m->has_gpu_mem;
    if (has_gpu_mem) {
        if (!ui_format_bytes_pair(buf, sizeof(buf),
                                  m->gpu_mem_used_bytes, m->gpu_mem_total_bytes)) {
            snprintf(buf, sizeof(buf), "已占用");
        }
        char line[80];
        snprintf(line, sizeof(line), "显存 %s", buf);   /* 1.2 / 8.0 GB */
        lv_label_set_text(s_ctx.det_gpu_mem, line);
    } else if (!m->valid || !m->has_gpu) {
        lv_label_set_text(s_ctx.det_gpu_mem, placeholder_detail(m));
    } else {
        lv_label_set_text(s_ctx.det_gpu_mem, "显存 --");
    }
    lv_obj_set_style_text_color(s_ctx.det_gpu_mem,
                                has_gpu_mem ? COLOR_MUTED : COLOR_STALE, 0);
}

/** 右侧卡：优先进程（Windows v3），没有就回落温区（Linux） */
static void apply_right_card(const device_metrics_t *m, bool fresh)
{
    const bool has_proc = fresh && m->processes_len > 0;
    const bool has_zone = fresh && m->zones_len > 0;

    if (has_proc || !has_zone) {
        /* 有进程：显示进程；两个都没有时也留在进程卡上，
         * 由页脚说清楚「未上报」，而不是整块空白 */
        lv_obj_remove_flag(s_ctx.card_proc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.card_zone, LV_OBJ_FLAG_HIDDEN);

        char buf[80];
        for (int i = 0; i < PROC_ROWS; i++) {
            const bool has = has_proc && i < m->processes_len;

            if (!has) {
                lv_label_set_text(s_ctx.lbl_proc_name[i], "");
                lv_label_set_text(s_ctx.lbl_proc_cpu[i], "");
                lv_label_set_text(s_ctx.lbl_proc_mem[i], "");
                continue;
            }

            lv_label_set_text(s_ctx.lbl_proc_name[i], m->processes[i].name);

            if (m->processes[i].has_cpu) {
                snprintf(buf, sizeof(buf), "%.1f%%", m->processes[i].cpu_percent);
                lv_label_set_text(s_ctx.lbl_proc_cpu[i], buf);
            } else {
                lv_label_set_text(s_ctx.lbl_proc_cpu[i], "--");
            }

            if (m->processes[i].has_mem && m->processes[i].memory_bytes > 0) {
                const double bytes = m->processes[i].memory_bytes;
                if (bytes >= 1024.0 * 1024.0 * 1024.0) {
                    snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
                } else {
                    snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024.0));
                }
                lv_label_set_text(s_ctx.lbl_proc_mem[i], buf);
            } else {
                lv_label_set_text(s_ctx.lbl_proc_mem[i], "--");
            }
        }

        if (has_proc) {
            if (m->has_process_count) {
                snprintf(buf, sizeof(buf), "共 %d 个进程 · 仅列出前 %d",
                         m->process_count, m->processes_len);
            } else {
                snprintf(buf, sizeof(buf), "仅列出前 %d", m->processes_len);
            }
            lv_label_set_text(s_ctx.lbl_proc_foot, buf);
        } else {
            lv_label_set_text(s_ctx.lbl_proc_foot, placeholder_detail(m));
        }
        return;
    }

    /* 没有进程但有温区：换成温区卡（泰山派） */
    lv_obj_add_flag(s_ctx.card_proc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ctx.card_zone, LV_OBJ_FLAG_HIDDEN);

    char name[DEVICE_METRICS_NAME_LEN];
    char value[24];
    for (int i = 0; i < ZONE_CELLS; i++) {
        const bool has = i < m->zones_len;

        if (has) {
            snprintf(value, sizeof(value), "%.1f C", m->zones[i].celsius);
            lv_label_set_text(s_ctx.lbl_zone_name[i],
                              pretty_zone_name(m->zones[i].name, name, sizeof(name)));
            lv_obj_set_style_text_color(s_ctx.lbl_zone_name[i], COLOR_MUTED, 0);
            lv_obj_set_style_text_color(s_ctx.lbl_zone_val[i],
                                        temp_color(m->zones[i].celsius), 0);
        } else {
            snprintf(value, sizeof(value), "--");
            lv_label_set_text(s_ctx.lbl_zone_name[i], "--");
            lv_obj_set_style_text_color(s_ctx.lbl_zone_name[i], COLOR_STALE, 0);
            lv_obj_set_style_text_color(s_ctx.lbl_zone_val[i], COLOR_STALE, 0);
        }
        lv_label_set_text(s_ctx.lbl_zone_val[i], value);
    }
}

/** 左下卡：优先网络（v3），没有就回落负载/运行时长 */
static void apply_left_bottom(const device_metrics_t *m, bool fresh,
                              const device_status_t *st)
{
    char buf[48];
    const bool has_net = fresh && (m->has_net_rx || m->has_net_tx);

    if (has_net) {
        lv_obj_remove_flag(s_ctx.card_net, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.card_load, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(s_ctx.lbl_mon_net_ip, st->ip[0] ? st->ip : "--");
        if (m->has_net_rx) {
            format_rate(buf, sizeof(buf), m->net_rx_bytes_per_sec);
            char line[64];
            snprintf(line, sizeof(line), "↓ %s", buf);
            lv_label_set_text(s_ctx.lbl_mon_net_rx, line);
        } else {
            lv_label_set_text(s_ctx.lbl_mon_net_rx, "↓ --");
        }
        if (m->has_net_tx) {
            format_rate(buf, sizeof(buf), m->net_tx_bytes_per_sec);
            char line[64];
            snprintf(line, sizeof(line), "↑ %s", buf);
            lv_label_set_text(s_ctx.lbl_mon_net_tx, line);
        } else {
            lv_label_set_text(s_ctx.lbl_mon_net_tx, "↑ --");
        }
        return;
    }

    /* 没网络数据：回到负载卡（隐藏网络卡，露出下面的那张） */
    lv_obj_add_flag(s_ctx.card_net, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ctx.card_load, LV_OBJ_FLAG_HIDDEN);

    const bool has_load   = fresh && m->has_load;
    const bool has_uptime = fresh && m->has_uptime;

    if (has_load) {
        lv_label_set_text(s_ctx.lbl_load_title, "平均负载");
        lv_label_set_text_fmt(s_ctx.val_load1, "%.2f", m->load1);
        lv_label_set_text_fmt(s_ctx.val_load5, "%.2f", m->load5);
        lv_label_set_text_fmt(s_ctx.val_load15, "%.2f", m->load15);
        lv_obj_remove_flag(s_ctx.val_load1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.val_load5, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.val_load15, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.val_uptime, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_ctx.val_load1, COLOR_TEXT, 0);
        lv_obj_set_style_text_color(s_ctx.val_load5, COLOR_TEXT, 0);
        lv_obj_set_style_text_color(s_ctx.val_load15, COLOR_TEXT, 0);
        lv_label_set_text(s_ctx.lbl_load_note, "1 / 5 / 15 分钟");
    } else if (has_uptime) {
        format_uptime(buf, sizeof(buf), m->uptime_seconds);
        lv_label_set_text(s_ctx.lbl_load_title, "运行时长");
        lv_label_set_text(s_ctx.val_uptime, buf);
        lv_obj_add_flag(s_ctx.val_load1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.val_load5, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.val_load15, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.val_uptime, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ctx.lbl_load_note, "开机至今");
    } else {
        lv_label_set_text(s_ctx.lbl_load_title, "平均负载");
        lv_label_set_text(s_ctx.val_load1, "--");
        lv_label_set_text(s_ctx.val_load5, "--");
        lv_label_set_text(s_ctx.val_load15, "--");
        lv_obj_remove_flag(s_ctx.val_load1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.val_load5, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.val_load15, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.val_uptime, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_ctx.val_load1, COLOR_STALE, 0);
        lv_obj_set_style_text_color(s_ctx.val_load5, COLOR_STALE, 0);
        lv_obj_set_style_text_color(s_ctx.val_load15, COLOR_STALE, 0);
        lv_label_set_text(s_ctx.lbl_load_note, placeholder_detail(m));
    }
}

/** 内存 / 磁盘 */
static void apply_bottom(const device_metrics_t *m, bool fresh)
{
    char buf[64];

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

    /* ---- 磁盘主分区 ---- */
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

    /* ---- 其余分区：标题右侧「共 N 个分区」，底部列出除主分区外的占用率 ---- */
    const bool has_parts = fresh && m->disks_len > 1;
    if (has_parts) {
        char count[32];
        snprintf(count, sizeof(count), "共 %u 个分区",
                 (unsigned)m->disks_len);
        lv_label_set_text(s_ctx.lbl_disk_parts, count);

        char line[64] = "";
        size_t used = 0;
        for (int i = 0; i < m->disks_len; i++) {
            if (strcmp(m->disks[i].path, m->disk_path) == 0) {
                continue;   /* 主分区已经用大字显示了 */
            }
            const int n = snprintf(line + used, sizeof(line) - used, "%s%.1f%%",
                                   used ? "   " : "", m->disks[i].used_percent);
            if (n <= 0 || used + (size_t)n >= sizeof(line)) {
                break;
            }
            used += (size_t)n;
        }
        lv_label_set_text(s_ctx.det_disk_parts, line);
    } else {
        lv_label_set_text(s_ctx.lbl_disk_parts, "");
        lv_label_set_text(s_ctx.det_disk_parts, "");
    }
}

static void monitor_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    device_registry_entry_t entry;
    if (!current_device(&entry)) {
        /* 一台设备都没有：整页保持占位，不做任何半真半假的展示 */
        device_metrics_t m = {0};
        device_status_t  s = {0};
        apply_header(&entry, &m, &s, false);
        apply_cpu_gpu(&m, false);
        apply_right_card(&m, false);
        apply_left_bottom(&m, false, &s);
        apply_bottom(&m, false);
        return;
    }

    device_metrics_t m;
    device_metrics_get(s_mon_slot, &m);

    device_status_t st;
    device_status_get(s_mon_slot, &st);

    /* 可以当当前值显示 = 解析成功且未过期 */
    const bool fresh = m.valid && !m.expired;

    apply_header(&entry, &m, &st, fresh);
    apply_cpu_gpu(&m, fresh);
    apply_right_card(&m, fresh);
    apply_left_bottom(&m, fresh, &st);
    apply_bottom(&m, fresh);
}

/* ---------------------------------------------------------------- 页面构建 */

void ui_page_monitor_build(lv_obj_t *page)
{
    build_header(page);
    build_cpu_gpu_card(page);
    /* 先建下面的负载卡，再建同坐标的网络卡：网络卡在上面，隐藏它就把负载卡露出来 */
    build_load_card(page);
    build_network_card(page);
    build_process_card(page);
    build_zone_card(page);
    build_memory_card(page);
    build_disk_card(page);

    /* 默认先隐藏后续由能力决定的那几张 */
    lv_obj_add_flag(s_ctx.card_net, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(monitor_poll_cb, MON_POLL_PERIOD_MS, NULL);

    /* 首帧就填上真实数据，避免开机先闪一下占位文字 */
    monitor_poll_cb(NULL);
}
