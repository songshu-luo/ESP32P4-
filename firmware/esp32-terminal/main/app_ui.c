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

#include "app_settings.h"
#include "background.h"
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
 * 控制页已按需求去掉——这块板子现在只负责「看」，不提供任何控制入口。
 * 设置页（S7）从占位页换成真实实现，页序保持 总览 / 监控 / 设置。 */
static const page_def_t PAGES[] = {
    { "总览", ui_page_overview_build },
    { "监控", ui_page_monitor_build },
    { "设置", ui_page_settings_build },
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

/* ---------------------------------------------------------------- 玻璃卡片 */

/* 卡片登记表：滑动开始时统一把它们变实、滑动结束后统一淡回半透明。
 * 页面一次性建好，当前共 18 张（总览 6 + 监控 7 + 设置 5），24 留点余量。 */
#define UI_CARD_MAX (24)

static lv_obj_t *s_cards[UI_CARD_MAX];
static int s_card_cnt = 0;

/* 页面构建完成前不响应滚动事件：build_pages() 末尾的 set_active(0, OFF)
 * 也会同步走一遍 SCROLL_BEGIN/END，不拦住的话开机瞬间卡片会闪一下全实。 */
static bool s_cards_live = false;

/* 滚动结束后的「确认停稳」定时器：tabview 在 SCROLL_END 里还会启动
 * 吸附/惯性动画，而且 indev 发 SCROLL_END 时 scroll_obj 尚未清空，
 * lv_obj_is_scrolling() 当场看会一直是 true。这里等 40 ms 再看一次，
 * 真停了才淡回 183，避免在吸附动画里做 alpha 混合。 */
static lv_timer_t *s_fade_tmr = NULL;

/* LVGL 9 的动画 exec 签名是 (void *, int32_t)，不能直接挂
 * lv_obj_set_style_bg_opa，这里包一层。 */
static void card_opa_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/* 立即把所有卡片设成同一不透明度：滑动开始时用，瞬时设置比动画省事，
 * 也避免「手指已经在拖、卡片还在渐变」的混合开销。 */
static void cards_set_opa_instant(lv_opa_t opa)
{
    for (int i = 0; i < s_card_cnt; i++) {
        if (s_cards[i] == NULL) {
            continue;
        }
        /* 幂等：BEGIN / END 可能连续触发，先停掉这张卡上未完成的淡入动画。
         * 只删自己挂的 exec_cb，别误伤卡片上其它动画。 */
        lv_anim_delete(s_cards[i], card_opa_anim_cb);
        lv_obj_set_style_bg_opa(s_cards[i], opa, 0);
    }
}

/* 120 ms 内把卡片动画回半透明（滑动结束时用）。
 * 起点取卡片当前的不透明度而不是写死 255：tabview 吸附动画结束时
 * 会再发一次 SCROLL_END，那时卡片通常已经是 183，若强行从 255 重新
 * 淡入会看到一次「突然变亮再暗下去」的闪烁。正常路径（BEGIN 刚把
 * 它设成 255）仍然是 255 → 183。 */
static void cards_fade_to(lv_opa_t opa)
{
    for (int i = 0; i < s_card_cnt; i++) {
        lv_obj_t *card = s_cards[i];
        if (card == NULL) {
            continue;
        }
        /* 幂等：SCROLL_END 可能连续触发，重复启动前先删掉旧动画 */
        lv_anim_delete(card, card_opa_anim_cb);

        const lv_opa_t cur = lv_obj_get_style_bg_opa(card, 0);
        if (cur == opa) {
            continue;
        }

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, card);
        lv_anim_set_values(&a, (int32_t)cur, (int32_t)opa);
        lv_anim_set_duration(&a, 120);
        lv_anim_set_exec_cb(&a, card_opa_anim_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

/* 40 ms 轮询一次：等滚动 / tabview 吸附动画真正停下，再把卡片淡回
 * 半透明。lv_obj_is_scrolling() 在 indev 刚发 SCROLL_END 的那一刻仍为
 * true（scroll_obj 还没清空），当场判断会把淡回永远挡掉。 */
static void fade_check_cb(lv_timer_t *timer)
{
    lv_obj_t *cont = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (cont == NULL || lv_obj_is_scrolling(cont)) {
        return;     /* 还在动：保持不透明，下个周期再看 */
    }
    cards_fade_to(183);
    lv_timer_pause(timer);
}

/**
 * 滑动开始：卡片变实（255）。
 *
 * 这两个事件是 tabview 的 content（真正滚动的对象）发出的，不是 tv；
 * 回调里用 lv_event_get_target(e) 现取事件对象，不缓存静态 tv，
 * 避免页面重建后回调还指着旧对象。
 */
static void pages_scroll_begin_cb(lv_event_t *e)
{
    if (!s_cards_live) {
        return;
    }
    lv_obj_t *scroll_obj = lv_event_get_target(e);
    if (scroll_obj == NULL) {
        return;
    }
    /* 新一轮滑动开始：先停掉上一轮的「停稳」检测 */
    if (s_fade_tmr != NULL) {
        lv_timer_pause(s_fade_tmr);
    }
    cards_set_opa_instant(LV_OPA_COVER);
}

/** 滑动结束：启动停稳检测，真停下后再用 120 ms 淡回 183（72%） */
static void pages_scroll_end_cb(lv_event_t *e)
{
    if (!s_cards_live) {
        return;
    }
    lv_obj_t *scroll_obj = lv_event_get_target(e);
    if (scroll_obj == NULL) {
        return;
    }
    if (s_fade_tmr != NULL) {
        lv_timer_reset(s_fade_tmr);
        lv_timer_resume(s_fade_tmr);
    } else {
        /* 理论上不会发生（定时器在 build_pages 里建好），兜底直接淡回 */
        cards_fade_to(183);
    }
}

lv_obj_t *ui_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, 183, 0);                 /* 72% 半透明 */
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);       /* 8%~10% 高光边 */
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_scrollable(card, false);

    if (s_card_cnt < UI_CARD_MAX) {
        s_cards[s_card_cnt++] = card;
    }
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
    /* 80% 半透明：状态栏不在 content 内，滑动时不重绘，可以长期半透明 */
    lv_obj_set_style_bg_opa(bar, LV_OPA_80, 0);
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
    /* tv 本身透明：背景图在最底层，tv 不能盖一层不透明底 */
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(tv, 0, 0);
    lv_obj_add_event_cb(tv, tab_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 页签栏：只覆盖主题的视觉样式（背景/圆角/内边距），**不能** remove_style_all——
     * flex_flow 在 LVGL 9 里也是样式属性，全删后三个页签按钮会全部叠在原点，
     * 屏幕上就是一团叠字。尺寸 100% x 56。 */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_size(tab_bar, lv_pct(100), TAB_BAR_H);
    lv_obj_set_style_bg_color(tab_bar, COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_80, 0);
    lv_obj_set_style_radius(tab_bar, 0, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_shadow_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 0, 0);
    lv_obj_set_style_pad_column(tab_bar, 0, 0);

    /* 内容容器：不留内边距和间距，页面自己按 1024x(600-56-56) 排版；
     * 透明才能让背景图透上来（页面本身已经 remove_style_all）。 */
    lv_obj_t *cont = lv_tabview_get_content(tv);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_style_pad_column(cont, 0, 0);

    /* 真正被拖动 / 惯性滚动的是 content，不是 tv：LVGL 9 的 tabview 把
     * SCROLL_BEGIN / SCROLL_END 发给 content（tv 默认不冒泡事件），所以
     * 两个回调挂在 content 上；回调里仍用 lv_event_get_target(e) 现取对象。 */
    lv_obj_add_event_cb(cont, pages_scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(cont, pages_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    /* 停稳检测定时器：默认暂停，SCROLL_END 之后才启动 */
    s_fade_tmr = lv_timer_create(fade_check_cb, 40, cont);
    if (s_fade_tmr != NULL) {
        lv_timer_pause(s_fade_tmr);
    }

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
            /* 选中态：60% 而非全不透明，背景图能透上来一点 */
            lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_CHECKED);
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

    /* 息屏 / 唤醒统一在这里驱动：LVGL 定时器已经在跑，不需要再建一个。
     * 这一步只读「无操作时长」，不碰任何跨任务共享状态。 */
    app_settings_tick();
}

/* ---------------------------------------------------------------- 背景层 */

/* 全屏背景层句柄。app_ui_build() 时 background_dsc() 通常还是 NULL
 * （UI 在 wall_sd_init() 之前就建好了，SD 还没挂载），所以先留着句柄，
 * 等 main.c 解码完成后用 app_ui_background_refresh() 补挂图。 */
static lv_obj_t *s_bg_obj = NULL;

/**
 * 全屏背景图（/sdcard/bg.jpg 解码后由 background_dsc() 提供）。
 *
 * 它是 scr 的第一个子对象，再 move_background() 一次保证压在最底：
 * 状态栏、tabview、页签栏都盖在它上面，滑动页面时只有 content 动，
 * 这一层完全静止，不会被重复贴图。
 *
 * 没有图（没插卡 / 没 bg.jpg / 解码失败）时不设源，靠 bg_color + COVER
 * 兜底成和改造前一样的纯色底。lv_image 本身不接收点击、不滚动，
 * 不会干扰页面手势。
 */
static void build_background(lv_obj_t *scr)
{
    lv_obj_t *bg_obj = lv_image_create(scr);
    lv_obj_set_size(bg_obj, 1024, 600);
    lv_obj_align(bg_obj, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bg_obj, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(bg_obj, LV_OPA_COVER, 0);
    lv_obj_set_clickable(bg_obj, false);
    lv_obj_set_scrollable(bg_obj, false);

    const lv_image_dsc_t *bg = background_dsc();
    if (bg != NULL) {
        lv_image_set_src(bg_obj, bg);
    }

    lv_obj_move_background(bg_obj);
    s_bg_obj = bg_obj;
}

void app_ui_background_refresh(void)
{
    if (s_bg_obj == NULL) {
        return;
    }
    const lv_image_dsc_t *bg = background_dsc();
    if (bg == NULL) {
        /* 没图：保持纯色底，什么也不做 */
        return;
    }
    lv_image_set_src(s_bg_obj, bg);
    /* 设源会重算尺寸/重绘，再压回最底一次，防止以后有人插对象进来 */
    lv_obj_move_background(s_bg_obj);
}

/* ---------------------------------------------------------------- 界面构建 */

void app_ui_build(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    /* 旧界面（含 content）马上要被清掉，先把引用它的停稳定时器删掉 */
    if (s_fade_tmr != NULL) {
        lv_timer_delete(s_fade_tmr);
        s_fade_tmr = NULL;
    }
    lv_obj_clean(scr);
    /* lv_obj_clean 已删掉旧卡片，登记表里的指针全部失效；重新构建时清零 */
    s_card_cnt = 0;
    s_cards_live = false;
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 背景层必须最先建、并且压在最底，后面的状态栏和 tabview 都盖在它上面 */
    build_background(scr);

    build_status_bar(scr);
    build_pages(scr);

    /* 页面（以及卡片登记表）到这里才是完整可用的，之后才允许滚动回调改不透明度；
     * 否则 build_pages 末尾的 set_active(0, OFF) 会让卡片先闪一下全实再淡回。 */
    s_cards_live = true;

    lv_timer_create(status_poll_cb, STATUS_POLL_PERIOD_MS, NULL);

    /* 首帧就把真实状态填上，避免开机先闪一下占位文字 */
    status_poll_cb(NULL);
}
