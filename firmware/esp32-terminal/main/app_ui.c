/*
 * 终端界面主骨架（S2-d）
 *
 *   S0：点屏 / 触摸 / 背光
 *   S1：以太网（网络卡片）
 *   S2-a：MQTT 接入（MQTT 服务器卡片）
 *   S2-b：泰山派真实指标大卡
 *   S2-c：在线状态灯（在线 / 离线 / 过期 / 未知）
 *   S2-d：顶部常驻状态栏 + 三个页面（总览 / 监控 / 设置）
 *   S2-e：多设备——状态灯变成「所有设备」的综合结论，总览页逐设备出卡
 *
 * 本文件只负责「跨页面」的东西：顶部状态栏、页签、定时器。
 * 各页面的内容在 ui_page_overview.c / ui_page_monitor.c（以及后续的设置页）里构建。
 *
 * 控制页已按需求移除：这块板子只做「查看状态」，不提供任何控制入口
 * （项目交接 §7 的红线也因此不受影响）。
 *
 * 设计原则：UI 线程只读各模块的互斥快照，绝不等待网络、绝不解析 JSON
 * （解析在 esp-mqtt 任务里做，见 device_metrics.c）。
 *
 * 中文字体：LVGL 自带的 Montserrat 只有拉丁字母，中文用 tools/gen_cjk_font.sh
 * 生成的子集字体（只包含 main/ 下出现过的汉字），尺寸 16/20/28 px。
 * 改动中文文案后必须重新生成字体，否则会显示成方块。
 */

#include "app_ui.h"
#include "app_ui_internal.h"

#include <stdio.h>

#include "device_registry.h"
#include "device_status.h"
#include "mqtt_link.h"

#include "fonts/cjk_fonts.h"

#define APP_VERSION     "0.5.5-s2e"

/* 在线状态轮询周期（ms）：status 5 s 一条、有效期 15 s，500 ms 足够 */
#define STATUS_POLL_PERIOD_MS   (500)

typedef struct {
    const char *name;
    void (*build)(lv_obj_t *page);
} page_def_t;

app_ui_ctx_t s_ctx;

/* 监控页当前显示的设备槽位（◀ ▶ 按钮改它，监控页轮询回调读它）。
 * 初始 0：泰山派通常是最先上报的设备，开机就能看到它的详细数据。 */
int s_mon_slot = 0;

/* 页面顺序即页签顺序。
 * 控制页已按需求去掉——这块板子现在只负责「看」，不提供任何控制入口。 */
static void page_settings_stub(lv_obj_t *page)
{
    ui_page_placeholder_build(page, "设置页",
                              "修改 broker 地址、凭据与设备参数需要固件重配，"
                              "本阶段只读展示，不提供服务端写入。");
}

static const page_def_t PAGES[] = {
    { "总览", ui_page_overview_build },
    { "监控", ui_page_monitor_build },
    { "设置", page_settings_stub },
};

#define PAGE_COUNT  ((int)(sizeof(PAGES) / sizeof(PAGES[0])))

/* ---------------------------------------------------------------- 通用控件 */

/**
 * 字节数 → "1.7 / 3.8 GB"，避免在屏幕上出现 10 位数字。
 *
 * total <= 0（字段缺失）时返回 false，让调用方换成别的说明，
 * 而不是显示成 "0 / 0 MB"。
 */
bool ui_format_bytes_pair(char *buf, size_t len, double used, double total)
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

lv_obj_t *ui_card(lv_obj_t *parent, int32_t w, int32_t h)
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

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font,
                   lv_color_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

/* ---------------------------------------------------------------- 顶部状态栏 */

/**
 * 常驻在三个页面之上的一行：
 *
 *   ● 在线   总览                          固件 0.5.4-s2e
 *
 * 状态灯从 S2-c 的指标区挪到这里——它描述的是「链路和设备」，不属于某一页。
 * clock_synced=false 时中间会出现一行黄色提示（协议 §4.1 要求）。
 */
static void build_status_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, lv_pct(100), STATUS_BAR_H);
    lv_obj_set_style_bg_color(bar, COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(bar, false);

    s_ctx.lamp = lv_obj_create(bar);
    lv_obj_remove_style_all(s_ctx.lamp);
    lv_obj_set_size(s_ctx.lamp, 16, 16);
    lv_obj_set_style_radius(s_ctx.lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ctx.lamp, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ctx.lamp, COLOR_MUTED, 0);
    lv_obj_set_clickable(s_ctx.lamp, false);
    lv_obj_set_pos(s_ctx.lamp, 24, 20);

    s_ctx.lbl_status = ui_label(bar, &lv_font_cjk_20, COLOR_MUTED, "未知");
    lv_obj_set_pos(s_ctx.lbl_status, 48, 14);

    /* 这里只显示当前页面名；设备名在总览页的卡片和监控页页头里 */
    s_ctx.lbl_page = ui_label(bar, &lv_font_cjk_20, COLOR_ACCENT, PAGES[0].name);
    lv_obj_set_pos(s_ctx.lbl_page, 132, 14);

    s_ctx.lbl_clock_warn = ui_label(bar, &lv_font_cjk_16, COLOR_WARN,
                                    "有设备时间未同步，过期判断不可信");
    lv_obj_set_pos(s_ctx.lbl_clock_warn, 216, 18);
    lv_obj_add_flag(s_ctx.lbl_clock_warn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ver = ui_label(bar, &lv_font_cjk_16, COLOR_MUTED, "固件 " APP_VERSION);
    lv_obj_align(ver, LV_ALIGN_RIGHT_MID, -24, 0);
}

/* ---------------------------------------------------------------- 页签 */

/** 页签被点中 / 被滑动切换时，更新状态栏里的页面名 */
static void tab_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    const uint32_t idx = lv_tabview_get_tab_active(tv);
    if (idx < (uint32_t)PAGE_COUNT) {
        lv_label_set_text(s_ctx.lbl_page, PAGES[idx].name);
    }
}

/**
 * 三个页面用 lv_tabview：它自带「按钮 + 左右滑动」两种切换方式，
 * 不需要自己实现手势识别，也不会把触摸事件和页面内控件抢走。
 */
static void build_pages(lv_obj_t *scr)
{
    lv_obj_t *tv = lv_tabview_create(scr);
    lv_obj_set_pos(tv, 0, STATUS_BAR_H);
    lv_obj_set_size(tv, lv_pct(100), LV_VER_RES - STATUS_BAR_H);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    /* tab_pos 是 LV_DIR_TOP（属于 LV_DIR_VER），所以这里设的是高度 */
    lv_tabview_set_tab_bar_size(tv, TAB_BAR_H);
    lv_obj_set_style_bg_color(tv, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tv, 0, 0);
    lv_obj_add_event_cb(tv, tab_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 页签栏：只覆盖主题的视觉样式（背景/圆角/内边距），**不能** remove_style_all——
     * flex_flow 在 LVGL 9 里也是样式属性，全删后三个页签按钮会全部叠在原点，
     * 屏幕上就是一团叠字。尺寸 100% x 56。 */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_size(tab_bar, lv_pct(100), TAB_BAR_H);
    lv_obj_set_style_bg_color(tab_bar, COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tab_bar, 0, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_shadow_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 0, 0);
    lv_obj_set_style_pad_column(tab_bar, 0, 0);

    /* 内容容器：不留内边距和间距，页面自己按 1024x(600-56-56) 排版 */
    lv_obj_t *cont = lv_tabview_get_content(tv);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_style_pad_column(cont, 0, 0);

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *page = lv_tabview_add_tab(tv, PAGES[i].name);

        /* 页面默认样式是白底 + 圆角 + 内边距，全部去掉，由页面自己排版。
         * 注意：remove_style_all 会连 add_tab 里 set_size(100%,100%) 的本地样式
         * 一起删掉，页面会缩回默认尺寸导致卡片被裁剪/叠在一起——删完必须补回去。 */
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, lv_pct(100), lv_pct(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_scrollable(page, false);

        /* 页签按钮的文字必须换成中文字体，否则默认 montserrat 显示成方块 */
        lv_obj_t *btn = lv_tabview_get_tab_button(tv, i);
        if (btn != NULL) {
            lv_obj_set_style_radius(btn, 0, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_text_font(btn, &lv_font_cjk_20, 0);
            lv_obj_set_style_text_color(btn, COLOR_MUTED, 0);
            lv_obj_set_style_text_color(btn, COLOR_ACCENT, LV_STATE_CHECKED);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(btn, COLOR_CARD, LV_STATE_CHECKED);
        }

        if (PAGES[i].build != NULL) {
            PAGES[i].build(page);
        }
    }

    /* 先默认选中「总览」；每个页面要么有真实构建函数，要么是占位页 */
    lv_tabview_set_active(tv, 0, LV_ANIM_OFF);
}

/* ---------------------------------------------------------------- 在线状态灯 */

/**
 * 三态判定，严格按 protocol/topics-v1.md §3 的优先级。
 * S2-e 起不再只看一台设备，而是把所有已接入设备汇总成一个结论：
 *
 *   1. Hub 不可达（MQTT 没连上）      → 未知，绝不沿用上次的绿色
 *   2. 一台设备都没接入                → 等待设备
 *   3. 有任何一台报 offline（LWT）      → N 台离线（红）
 *   4. 所有设备都 online 且没过期       → 在线（绿）
 *   5. 有设备心跳超时                  → N 台过期（黄）
 *   6. 其余（收到过但状态不认识）       → 未知
 *
 * 第 1 条是重点：broker 挂了以后，最后一条 retained 的 online 还躺在那里，
 * 只看消息的话灯会一直绿着——这正是要避免的「假在线」。
 * 「全部在线才绿」也是同一个道理：有一台断了就不该给人「都没事」的印象。
 */
static void status_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    mqtt_link_status_t link;
    mqtt_link_get_status(&link);

    const int count = device_registry_count();

    int online = 0;
    int offline = 0;
    int expired = 0;
    int unknown = 0;
    bool clock_untrusted = false;

    for (int slot = 0; slot < count; slot++) {
        device_status_t s;
        device_status_get(slot, &s);
        if (!s.ever_received) {
            unknown++;
            continue;
        }
        switch (s.state) {
        case DEVICE_STATUS_ONLINE:  online++;  break;
        case DEVICE_STATUS_OFFLINE: offline++; break;
        default:                    expired++; break;   /* 收到过但已超时 */
        }
        if (s.has_clock_flag && !s.clock_synced) {
            clock_untrusted = true;
        }
    }

    const char *text;
    lv_color_t  color;

    if (link.state != MQTT_LINK_ONLINE) {
        text  = "未知";
        color = COLOR_MUTED;
    } else if (count == 0) {
        text  = "等待设备";
        color = COLOR_MUTED;
    } else if (offline > 0) {
        text  = NULL;   /* 下面按数量格式化 */
        color = COLOR_ERR;
    } else if (online == count) {
        text  = "在线";
        color = COLOR_OK;
    } else if (expired > 0) {
        text  = NULL;
        color = COLOR_WARN;
    } else {
        text  = "未知";
        color = COLOR_MUTED;
    }

    lv_obj_set_style_bg_color(s_ctx.lamp, color, 0);
    if (text != NULL) {
        lv_label_set_text(s_ctx.lbl_status, text);
    } else if (offline > 0) {
        lv_label_set_text_fmt(s_ctx.lbl_status, "%d 台离线", offline);
    } else {
        lv_label_set_text_fmt(s_ctx.lbl_status, "%d 台过期", expired);
    }
    lv_obj_set_style_text_color(s_ctx.lbl_status, color, 0);

    /* 协议 §4.1：设备报 clock_synced=false 时订阅方必须提示时间不可信。
     * 只在链路通、且确实有设备报了该字段为 false 时才提示——链路断开时
     * 「时间未同步」会让状态栏同时出现两个警告，反而看不出重点。 */
    if (link.state == MQTT_LINK_ONLINE && clock_untrusted) {
        lv_obj_remove_flag(s_ctx.lbl_clock_warn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ctx.lbl_clock_warn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------- 界面构建 */

void app_ui_build(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(scr);
    build_pages(scr);

    lv_timer_create(status_poll_cb, STATUS_POLL_PERIOD_MS, NULL);

    /* 首帧就把真实状态填上，避免开机先闪一下占位文字 */
    status_poll_cb(NULL);
}
