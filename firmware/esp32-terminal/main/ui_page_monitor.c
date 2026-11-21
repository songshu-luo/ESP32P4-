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
 *                       CPU / 任务状态      —— 无 GPU 时右列改成 5 项任务状态（v5 checks）
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

/* 任务状态列（无 GPU 设备的右列）：行高沿用原容器列表的 34 px；
 * detail 列固定 68 px 宽，刚好放下 Agent 给的最长 detail「未检测到」（4 个汉字
 * = 64 px）；左侧中文名占剩下的 124 px，「ESP32通信」这类中英混排也放得下。 */
#define TASK_ROW_H          (34)
#define TASK_DET_W          (68)

/* down 用「灰红」：COLOR_ERR 在暗背景上太扎眼，和右侧「设备离线」抢注意力，
 * 按 70% 亮度混入一点灰，仍然一眼能看出是红色系。 */
#define CHECK_DOWN_COLOR    lv_color_hex(0xb4575a)

/* 任务状态行的控件句柄。放本文件静态数组而不是 s_ctx：
 * 行数由协议固定为 5，app_ui_internal.h 的容器行数组只有 4 个，且本次不改那个头文件。 */
static lv_obj_t *s_chk_dot[DEVICE_METRICS_MAX_CHECKS];
static lv_obj_t *s_chk_name[DEVICE_METRICS_MAX_CHECKS];
static lv_obj_t *s_chk_det[DEVICE_METRICS_MAX_CHECKS];

/* 字模收集提示 —— 不要删！
 *
 * 任务状态卡的 label / detail 全部来自 Agent 的 payload，不在本文件的任何字符串
 * 字面量里；而 tools/gen_cjk_font.sh 只扫描源码字面量来挑汉字。少了这一行，
 * 重新生成字模后屏幕上这 5 行会是空心方块（tofu）。
 * 内容 = Agent 可能下发的全部汉字（labels 5 个 + details 7 种），重复无害。 */
static const char k_check_glyph_hint[] __attribute__((unused)) =
    "闲鱼助手网络代理通信摄像头在线运行中不健康未检测到监听连接";

/* 字模收集提示 —— 不要删！
 *
 * 网络卡新样式的「有线 / 无线 / 未接入 / 当前 / 其他」都出现在运行时拼接的
 * 字符串里（snprintf / 条件选词），不是完整的字面量；少了这一行，重新生成
 * 字模后这几行会是空心方块（tofu）。build_network_card 里用 (void) 引用它，
 * 保证 -Wunused 在 -Werror 下不会报错。 */
static const char k_net_glyph_hint[] = "有线无线未接入当前其他";

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

/** 字节/秒 → 「12B」/「1.2K」/「3.4M」：网络卡两行样式每行要塞 ↓/↑ 两个速率，
 *  砍掉 "/s" 后缀换更短的单位，宽度才放得下。缺失值由调用方写 "--"。 */
static void format_rate_short(char *buf, size_t len, double bytes_per_sec)
{
    const double kb = 1024.0;
    const double mb = kb * 1024.0;

    if (bytes_per_sec >= mb) {
        snprintf(buf, len, "%.1fM", bytes_per_sec / mb);
    } else if (bytes_per_sec >= kb) {
        snprintf(buf, len, "%.1fK", bytes_per_sec / kb);
    } else {
        snprintf(buf, len, "%.0fB", bytes_per_sec);
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

    /* ---- 同一坐标的右列：没有 GPU 的机器（泰山派）显示 5 项任务状态 ----
     *
     * 句柄放在本文件的静态数组里而不是 s_ctx：任务状态固定 5 行，
     * 而 app_ui_internal.h 里的容器行数组只有 4 个，且本次不改那个头文件。
     * 列容器本身仍复用 s_ctx.cont_col（坐标、尺寸与原来完全一致）。 */
    s_ctx.cont_col = lv_obj_create(card);
    lv_obj_remove_style_all(s_ctx.cont_col);
    lv_obj_remove_flag(s_ctx.cont_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ctx.cont_col, CG_COL2_X, 0);
    lv_obj_set_size(s_ctx.cont_col, CG_COL_W, 208);
    lv_obj_add_flag(s_ctx.cont_col, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *cap_tasks = ui_label(s_ctx.cont_col, &lv_font_cjk_16, COLOR_MUTED, "任务状态");
    lv_obj_set_pos(cap_tasks, 0, 28);   /* 与左列「CPU 占用」同一基线 */

    /* 5 行 × 34 px 行高（与原容器列表一致），从 y=50 起：
     * 最后一行在 186，字体行高 20 → 底边 206，仍在 208 高的列内。 */
    for (int i = 0; i < DEVICE_METRICS_MAX_CHECKS; i++) {
        const int32_t y = 50 + i * TASK_ROW_H;

        lv_obj_t *dot = lv_obj_create(s_ctx.cont_col);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_color(dot, COLOR_STALE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 0, y + 6);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_chk_dot[i] = dot;

        s_chk_name[i] = ui_label(s_ctx.cont_col, &lv_font_cjk_16, COLOR_TEXT, "");
        lv_obj_set_pos(s_chk_name[i], 18, y);
        lv_obj_set_width(s_chk_name[i], CG_COL_W - 18 - TASK_DET_W - 6);
        lv_label_set_long_mode(s_chk_name[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_chk_name[i], LV_OBJ_FLAG_HIDDEN);

        /* 右侧短说明（在线 / 未运行），固定宽度右对齐，切设备时列不跳 */
        s_chk_det[i] = right_label(s_ctx.cont_col, &lv_font_cjk_16, COLOR_MUTED, "",
                                   CG_COL_W - TASK_DET_W, TASK_DET_W);
        lv_obj_set_pos(s_chk_det[i], CG_COL_W - TASK_DET_W, y);
        lv_label_set_long_mode(s_chk_det[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_chk_det[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 整块没有数据时的说明（等待数据），放在第一行任务状态的位置上：
     * 此时 5 行都隐藏，正好空出这一格，不会和标题重叠。 */
    s_ctx.lbl_cont_note = ui_label(s_ctx.cont_col, &lv_font_cjk_16, COLOR_STALE, "");
    lv_obj_set_pos(s_ctx.lbl_cont_note, 0, 50);
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

/* 网络卡 304x140，内边距 16 → 内容区 272x108。
 * 新样式两行固定「第一行有线、第二行无线」，y 从内容区顶部算：
 *   0   标题 / 右上「当前 有线」
 *   34  有线行
 *   68  无线行
 * 行高 34：cjk16 行高 20，两行文字之间留 14 的空隙，远看不会黏在一起。 */
#define NET_ROW_WIRED_Y     (34)
#define NET_ROW_WIFI_Y      (68)
#define NET_RATE_W          (120)   /* 右侧速率列固定宽，右对齐，切设备时不跳动 */

/** 一张网卡在 card_net 内的行序号：有线 0，无线 1。
 *  只认 "wired" / "wifi"，其余（null、空串、"other"）不上两行样式。 */
static int netif_row_of(const device_metrics_netif_t *n)
{
    if (strcmp(n->type, "wired") == 0) {
        return 0;
    }
    if (strcmp(n->type, "wifi") == 0) {
        return 1;
    }
    return -1;
}

static void build_network_card(lv_obj_t *page)
{
    /* 字模提示串的引用点：见文件顶部 k_net_glyph_hint 的说明 */
    (void)k_net_glyph_hint;

    s_ctx.card_net = ui_card(page, 304, 140);
    lv_obj_set_pos(s_ctx.card_net, 32, 324);

    lv_obj_t *title = ui_label(s_ctx.card_net, &lv_font_cjk_16, COLOR_MUTED, "网络");
    lv_obj_set_pos(title, 0, 0);

    /* 右上角「当前 有线」：只有分网卡数据（v6）时才有意义 */
    s_ctx.lbl_net_active = ui_label(s_ctx.card_net, &lv_font_cjk_16, COLOR_ACCENT, "");
    lv_obj_align(s_ctx.lbl_net_active, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(s_ctx.lbl_net_active, LV_OBJ_FLAG_HIDDEN);

    /* 新样式两行：左侧名称左对齐，右侧速率右对齐（同一基线，量测字高不同时观感一致） */
    static const int32_t row_y[2] = { NET_ROW_WIRED_Y, NET_ROW_WIFI_Y };
    for (int i = 0; i < 2; i++) {
        s_ctx.lbl_net_if[i] = ui_label(s_ctx.card_net, &lv_font_cjk_16, COLOR_MUTED, "");
        lv_obj_set_pos(s_ctx.lbl_net_if[i], 0, row_y[i]);

        s_ctx.lbl_net_rate[i] = right_label(s_ctx.card_net, &lv_font_cjk_16,
                                            COLOR_MUTED, "",
                                            272 - NET_RATE_W, NET_RATE_W);
        lv_obj_set_pos(s_ctx.lbl_net_rate[i], 272 - NET_RATE_W, row_y[i]);
    }

    /* 旧单行样式：IP + 单卡 ↓/↑（v1–v5 设备），坐标保持原样 */
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

    /* 新旧两套互斥，由 apply_left_bottom 决定露哪一套；先全部藏起来 */
    lv_obj_add_flag(s_ctx.lbl_mon_net_ip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ctx.lbl_mon_net_rx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ctx.lbl_mon_net_tx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ctx.lbl_mon_net_note, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 2; i++) {
        lv_obj_add_flag(s_ctx.lbl_net_if[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.lbl_net_rate[i], LV_OBJ_FLAG_HIDDEN);
    }
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

    /* C / D / S 三行逐行显示：左侧盘符+占用率，右侧已用/总量 */
    for (int i = 0; i < 3; i++) {
        s_ctx.lbl_disk_row[i] = ui_label(card, &lv_font_cjk_16, COLOR_TEXT, "");
        lv_obj_set_pos(s_ctx.lbl_disk_row[i], 0, 26 + i * 28);

        s_ctx.lbl_disk_val[i] = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
        lv_obj_align(s_ctx.lbl_disk_val[i], LV_ALIGN_TOP_RIGHT, 0, 26 + i * 28);

        lv_obj_add_flag(s_ctx.lbl_disk_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.lbl_disk_val[i], LV_OBJ_FLAG_HIDDEN);
    }
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

/** 任务状态圆点配色：ok 绿 / warn 琥珀 / down 灰红 / 未知灰 */
static lv_color_t check_dot_color(device_check_state_t state)
{
    switch (state) {
    case DEVICE_CHECK_OK:   return COLOR_OK;
    case DEVICE_CHECK_WARN: return COLOR_WARN;
    case DEVICE_CHECK_DOWN: return CHECK_DOWN_COLOR;
    default:                return COLOR_STALE;
    }
}

/**
 * 右半列（无 GPU 设备）的 5 行任务状态（v5 checks）。
 *
 * checks_count <= 0 表示没拿到数据（缺失 / 解析失败 / 空数组），整列只显示
 * 一句「等待数据」——不留空行，也不把 0 条说成「全部正常」。
 * 某一行 detail 为空时只显示圆点和中文名，不补「--」，避免看起来像故障。
 */
static void apply_checks(const device_metrics_t *m, bool fresh)
{
    const bool has_list = fresh && m->checks_count > 0;
    for (int i = 0; i < DEVICE_METRICS_MAX_CHECKS; i++) {
        if (has_list && i < m->checks_count) {
            const device_metrics_check_t *c = &m->checks[i];
            lv_obj_set_style_bg_color(s_chk_dot[i], check_dot_color(c->state), 0);
            lv_label_set_text(s_chk_name[i], c->label);
            lv_label_set_text(s_chk_det[i], c->detail);
            lv_obj_remove_flag(s_chk_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_chk_name[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_chk_det[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_chk_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chk_name[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_chk_det[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (has_list) {
        lv_label_set_text(s_ctx.lbl_cont_note, "");
        return;
    }
    /* 拿到了列表但帧已过期：和本页其它卡一样明说「数据已过期」
     * （旧值不可信，但说清楚原因比笼统的「等待数据」有用）。
     * 真正没数据时才是「等待数据」。 */
    const bool stale_with_list = m->valid && m->expired && m->checks_count > 0;
    lv_label_set_text(s_ctx.lbl_cont_note, stale_with_list ? "数据已过期" : "等待数据");
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

    /* ---- 右半列：有 GPU 走 GPU 列，没有 GPU 的机器走任务状态列 ---- */
    const bool has_gpu_dev = m->valid &&
        (m->gpu_name[0] != '\0' || m->has_gpu ||
         m->has_gpu_usage || m->has_gpu_temp);
    lv_label_set_text(s_ctx.lbl_cg_title, has_gpu_dev ? "CPU / GPU" : "CPU / 任务状态");
    if (has_gpu_dev) {
        lv_obj_remove_flag(s_ctx.gpu_col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.cont_col, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ctx.gpu_col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.cont_col, LV_OBJ_FLAG_HIDDEN);
        apply_checks(m, fresh);
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

/** 右上角「当前 有线 / 当前 无线 / 当前 其他 / 当前 --」。
 *  active_type 由 Agent 给（wired / wifi / other），认不出来就说「其他」，
 *  没有 active 信息（v1–v5 帧、字符串缺失）才是「--」。 */
static void netif_active_text(const device_metrics_t *m, char *buf, size_t len)
{
    const char *kind = "--";
    if (m->has_active) {
        if (strcmp(m->active_type, "wired") == 0) {
            kind = "有线";
        } else if (strcmp(m->active_type, "wifi") == 0) {
            kind = "无线";
        } else {
            kind = "其他";
        }
    }
    snprintf(buf, len, "当前 %s", kind);
}

/** 这一行网卡是不是当前上网卡：别名相同，或类型相同（active_id 可能只是别名变了） */
static bool netif_is_active(const device_metrics_t *m, const device_metrics_netif_t *n)
{
    if (!m->has_active) {
        return false;
    }
    return (m->active_id[0] != '\0' && strcmp(m->active_id, n->id) == 0) ||
           (m->active_type[0] != '\0' && strcmp(m->active_type, n->type) == 0);
}

/** 网络卡新样式：有线一行、无线一行，当前上网那一行整行高亮 */
static void apply_netif_rows(const device_metrics_t *m)
{
    /* 两行固定按 type 找，不按 netifs[] 顺序：只有一张卡时另一行直接隐藏 */
    for (int row = 0; row < 2; row++) {
        const device_metrics_netif_t *n = NULL;
        for (int i = 0; i < m->netifs_len && i < DEVICE_METRICS_MAX_NETIFS; i++) {
            if (netif_row_of(&m->netifs[i]) == row) {
                n = &m->netifs[i];
                break;
            }
        }

        if (n == NULL) {
            lv_obj_add_flag(s_ctx.lbl_net_if[row], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ctx.lbl_net_rate[row], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const char *name = row == 0 ? "有线" : "无线";

        /* 名称：up 且报得出协商速率时带上速率（「有线 1000M」），否则只写名字 */
        if (!n->up) {
            lv_label_set_text_fmt(s_ctx.lbl_net_if[row], "%s 未接入", name);
        } else if (n->has_link && n->link_mbps > 0.0) {
            lv_label_set_text_fmt(s_ctx.lbl_net_if[row], "%s %.0fM", name, n->link_mbps);
        } else {
            lv_label_set_text(s_ctx.lbl_net_if[row], name);
        }

        /* 速率：字段缺失单独显示 "--"，两个都缺时整行只留「--」 */
        char rx[16];
        char tx[16];
        if (n->has_rx) {
            format_rate_short(rx, sizeof(rx), n->rx_bytes_per_sec);
        } else {
            snprintf(rx, sizeof(rx), "--");
        }
        if (n->has_tx) {
            format_rate_short(tx, sizeof(tx), n->tx_bytes_per_sec);
        } else {
            snprintf(tx, sizeof(tx), "--");
        }
        lv_label_set_text_fmt(s_ctx.lbl_net_rate[row], "↓%s ↑%s", rx, tx);

        /* 颜色：down 整行用 STALE；能用的卡里当前上网那行用 ACCENT，另一行 MUTED */
        const lv_color_t color = !n->up ? COLOR_STALE
                                : netif_is_active(m, n) ? COLOR_ACCENT
                                                        : COLOR_MUTED;
        lv_obj_set_style_text_color(s_ctx.lbl_net_if[row], color, 0);
        lv_obj_set_style_text_color(s_ctx.lbl_net_rate[row], color, 0);

        lv_obj_remove_flag(s_ctx.lbl_net_if[row], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ctx.lbl_net_rate[row], LV_OBJ_FLAG_HIDDEN);
    }
}

/** 新旧两套 label 互斥：new_style=true 露两行样式，false 露旧的 IP + ↓/↑ */
static void net_card_use_if_style(bool new_style)
{
    lv_obj_t *old_labels[4] = {
        s_ctx.lbl_mon_net_ip, s_ctx.lbl_mon_net_rx,
        s_ctx.lbl_mon_net_tx, s_ctx.lbl_mon_net_note,
    };
    for (int i = 0; i < 4; i++) {
        if (new_style) {
            lv_obj_add_flag(old_labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(old_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!new_style) {
        lv_obj_add_flag(s_ctx.lbl_net_active, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 2; i++) {
            lv_obj_add_flag(s_ctx.lbl_net_if[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ctx.lbl_net_rate[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** 左下卡：优先网络（v3），没有就回落负载/运行时长 */
static void apply_left_bottom(const device_metrics_t *m, bool fresh,
                              const device_status_t *st)
{
    char buf[48];
    const bool has_net = fresh && (m->netifs_len > 0 || m->has_net_rx || m->has_net_tx);

    if (has_net) {
        lv_obj_remove_flag(s_ctx.card_net, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ctx.card_load, LV_OBJ_FLAG_HIDDEN);

        /* v6 有分网卡明细：两行样式；否则（泰山派等 v5 设备）回落到旧单行 */
        if (m->netifs_len > 0) {
            net_card_use_if_style(true);
            netif_active_text(m, buf, sizeof(buf));
            lv_label_set_text(s_ctx.lbl_net_active, buf);
            lv_obj_remove_flag(s_ctx.lbl_net_active, LV_OBJ_FLAG_HIDDEN);
            apply_netif_rows(m);
            return;
        }

        net_card_use_if_style(false);
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

    /* ---- 磁盘：最多三行（Windows 的 C/D/S，Linux 通常只有一行） ---- */
    int rows = 0;
    if (fresh) {
        for (int i = 0; i < m->disks_len && rows < 3; i++) {
            char path[24];
            snprintf(path, sizeof(path), "%s", m->disks[i].path);
            size_t n = strlen(path);
            while (n > 1 && (path[n - 1] == '\\' || path[n - 1] == '/')) {
                path[--n] = '\0';       /* "C:\\" → "C:"；单独的 "/" 保留 */
            }
            const double pct = m->disks[i].used_percent;
            lv_label_set_text_fmt(s_ctx.lbl_disk_row[rows], "%s %.1f%%",
                                  path[0] ? path : "?", pct);
            if (!ui_format_bytes_pair(buf, sizeof(buf), m->disks[i].used_bytes,
                                      m->disks[i].total_bytes)) {
                snprintf(buf, sizeof(buf), "已占用");
            }
            lv_label_set_text(s_ctx.lbl_disk_val[rows], buf);
            lv_obj_set_style_text_color(s_ctx.lbl_disk_row[rows],
                                        pct >= 85.0 ? COLOR_ERR :
                                        pct >= 70.0 ? COLOR_WARN : COLOR_TEXT, 0);
            rows++;
        }
        /* 老设备没有 disks[]，退回单个 disk 对象，至少别显示空卡 */
        if (rows == 0 && m->has_disk) {
            lv_label_set_text_fmt(s_ctx.lbl_disk_row[0], "%s %.1f%%",
                                  m->disk_path[0] ? m->disk_path : "磁盘",
                                  m->disk_percent);
            if (!ui_format_bytes_pair(buf, sizeof(buf), m->disk_used_bytes,
                                      m->disk_total_bytes)) {
                snprintf(buf, sizeof(buf), "已占用");
            }
            lv_label_set_text(s_ctx.lbl_disk_val[0], buf);
            lv_obj_set_style_text_color(s_ctx.lbl_disk_row[0],
                                        m->disk_percent >= 85.0 ? COLOR_ERR :
                                        m->disk_percent >= 70.0 ? COLOR_WARN : COLOR_TEXT, 0);
            rows = 1;
        }
    }

    for (int i = 0; i < 3; i++) {
        if (i < rows) {
            lv_obj_remove_flag(s_ctx.lbl_disk_row[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ctx.lbl_disk_val[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ctx.lbl_disk_row[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ctx.lbl_disk_val[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (fresh && m->disks_len > 0) {
        char count[32];
        snprintf(count, sizeof(count), "共 %u 个分区", (unsigned)m->disks_len);
        lv_label_set_text(s_ctx.lbl_disk_parts, count);
    } else {
        lv_label_set_text(s_ctx.lbl_disk_parts, "");
    }

    /* 盘符已经逐行显示，标题固定写“磁盘”，避免出现“磁盘 C:\”这种重复 */
    lv_label_set_text(s_ctx.lbl_mon_disk_title, "磁盘");
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
