/*
 * 总览页（S2-d 单设备 → S2-e 多设备）
 *
 * 布局：上半区 4 张「每设备一张」的摘要卡，下半区保留本机网络卡和实时时钟卡。
 * 摘要卡只回答「这台机器大概怎么样」：在线状态 + CPU + 内存；
 * 细节（温区、服务、磁盘）在监控页，用 ◀ ▶ 切换设备查看。
 *
 * 页面尺寸 = 1024 x (600 - 状态栏 56 - 页签栏 56) = 1024 x 488。
 *
 * 新鲜度与「--」的规则沿用 S2-b：
 *   - 解析成功且未过期才显示数值，否则显示 "--"；
 *   - 说明文字按「等待 Hub 数据 / 数据已过期 / 未上报」区分，绝不把没数据说成 0。
 */

#include "app_ui_internal.h"

#include <stdio.h>
#include <time.h>

#include "device_metrics.h"
#include "device_registry.h"
#include "device_status.h"
#include "eth_link.h"

#include "fonts/cjk_fonts.h"

/* 轮询周期：数据 2 s 一帧、500 ms 轮询足够跟上，且过期判断及时 */
#define METRICS_POLL_PERIOD_MS  (500)
#define NET_POLL_PERIOD_MS      (500)
#define CLOCK_POLL_PERIOD_MS    (1000)

/* SNTP 没对时之前，time() 从 1970 起算。低于 2023-11 的时间一律当成「还没对上」，
 * 屏幕上显示占位符而不是 1970 年 —— 后者会让人以为时钟坏了。 */
#define CLOCK_VALID_EPOCH       ((time_t)1700000000)

/* 上面四张设备卡：4×222 + 3×24 = 960，正好铺满 1024 减去左右 32 边距 */
#define DEV_CARD_W      (222)
#define DEV_CARD_H      (260)
#define DEV_CARD_GAP    (24)
#define DEV_CARD_X0     (32)
#define DEV_CARD_Y      (24)

/* ---------------------------------------------------------------- 工具函数 */

/** 没有数据时的说明文字。绝不能把「没有数据」说成「0」。 */
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

/** 设备在线状态 → 屏幕上的短词 + 颜色（协议 §3 的五级判定在 device_status 里） */
static const char *state_text(const device_status_t *s, lv_color_t *color)
{
    if (!s->ever_received) {
        *color = COLOR_MUTED;
        return "未知";
    }
    switch (s->state) {
    case DEVICE_STATUS_ONLINE:
        *color = COLOR_OK;
        return "在线";
    case DEVICE_STATUS_OFFLINE:
        *color = COLOR_ERR;
        return "离线";
    default:
        /* UNKNOWN 且收到过：说明是心跳超时，说「过期」比「未知」准确 */
        *color = COLOR_WARN;
        return s->expired ? "过期" : "未知";
    }
}

/* ---------------------------------------------------------------- 设备摘要 */

/** 单张设备摘要卡：设备名 + 状态 + CPU 大字 + 内存 + 底部说明 */
static void create_device_card(lv_obj_t *parent, int slot, int32_t x)
{
    lv_obj_t *card = ui_card(parent, DEV_CARD_W, DEV_CARD_H);
    lv_obj_set_pos(card, x, DEV_CARD_Y);

    /* 设备名可能是中文显示名，也可能是未登记设备的原始 id（ASCII） */
    s_ctx.lbl_dev_name[slot] = ui_label(card, &lv_font_cjk_20, COLOR_TEXT, "等待设备");
    lv_obj_align(s_ctx.lbl_dev_name[slot], LV_ALIGN_TOP_LEFT, 0, 0);

    s_ctx.lbl_dev_state[slot] = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "未知");
    lv_obj_align(s_ctx.lbl_dev_state[slot], LV_ALIGN_TOP_RIGHT, 0, 2);

    lv_obj_t *cpu_cap = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "CPU 占用");
    lv_obj_set_pos(cpu_cap, 0, 36);

    /* 数值只有数字和 %，用 Montserrat 36 更有冲击力 */
    s_ctx.lbl_dev_cpu[slot] = ui_label(card, &lv_font_montserrat_36, COLOR_STALE, "--");
    lv_obj_set_pos(s_ctx.lbl_dev_cpu[slot], 0, 56);

    s_ctx.lbl_dev_mem[slot] = ui_label(card, &lv_font_cjk_20, COLOR_MUTED, "内存 --");
    lv_obj_set_pos(s_ctx.lbl_dev_mem[slot], 0, 112);

    s_ctx.lbl_dev_detail[slot] = ui_label(card, &lv_font_cjk_16, COLOR_MUTED, "");
    lv_obj_align(s_ctx.lbl_dev_detail[slot], LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/** 槽位里没有设备：整张卡降为灰色提示，别让空槽看起来像「离线」 */
static void apply_empty_slot(int slot)
{
    lv_obj_set_style_text_color(s_ctx.lbl_dev_name[slot], COLOR_STALE, 0);
    lv_label_set_text(s_ctx.lbl_dev_name[slot], "未接入设备");
    lv_label_set_text(s_ctx.lbl_dev_state[slot], "");
    lv_label_set_text(s_ctx.lbl_dev_cpu[slot], "--");
    lv_obj_set_style_text_color(s_ctx.lbl_dev_cpu[slot], COLOR_STALE, 0);
    lv_label_set_text(s_ctx.lbl_dev_mem[slot], "");
    lv_label_set_text(s_ctx.lbl_dev_detail[slot], "");
}

static void apply_device_slot(int slot, const device_registry_entry_t *entry)
{
    device_metrics_t m;
    device_metrics_get(slot, &m);

    device_status_t st;
    device_status_get(slot, &st);

    /* 未登记的设备没有中文名，直接显示协议 id（协议保证是 ASCII 小写） */
    lv_obj_set_style_text_color(s_ctx.lbl_dev_name[slot], COLOR_TEXT, 0);
    lv_label_set_text(s_ctx.lbl_dev_name[slot],
                      entry->name[0] ? entry->name : entry->id);

    lv_color_t state_color = COLOR_MUTED;
    const char *state = state_text(&st, &state_color);
    lv_label_set_text(s_ctx.lbl_dev_state[slot], state);
    lv_obj_set_style_text_color(s_ctx.lbl_dev_state[slot], state_color, 0);

    /* 「可以当当前值显示」= 解析成功且未过期 */
    const bool fresh = m.valid && !m.expired;
    char value[32];
    char detail[64];

    if (fresh && m.has_cpu) {
        snprintf(value, sizeof(value), "%.1f%%", m.cpu_percent);
    } else {
        snprintf(value, sizeof(value), "--");
    }
    lv_obj_set_style_text_color(s_ctx.lbl_dev_cpu[slot],
                                fresh && m.has_cpu ? COLOR_ACCENT : COLOR_STALE, 0);
    lv_label_set_text(s_ctx.lbl_dev_cpu[slot], value);

    if (fresh && m.has_mem) {
        lv_label_set_text_fmt(s_ctx.lbl_dev_mem[slot], "内存 %.1f%%", m.mem_percent);
    } else {
        lv_label_set_text(s_ctx.lbl_dev_mem[slot], "内存 --");
    }

    /* 底部一行：有数据时给内存的绝对用量，没数据时说清楚是哪种「没数据」 */
    if (fresh && ui_format_bytes_pair(detail, sizeof(detail),
                                      m.mem_used_bytes, m.mem_total_bytes)) {
        lv_label_set_text(s_ctx.lbl_dev_detail[slot], detail);
    } else {
        lv_label_set_text(s_ctx.lbl_dev_detail[slot], placeholder_detail(&m));
    }
}

static void devices_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    const int count = device_registry_count();
    for (int slot = 0; slot < UI_DEVICE_SLOTS; slot++) {
        if (slot < count) {
            device_registry_entry_t entry;
            device_registry_get(slot, &entry);
            apply_device_slot(slot, &entry);
        } else {
            apply_empty_slot(slot);
        }
    }
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

/* ---------------------------------------------------------------- 实时时钟 */

/* 时钟只有这两个标签需要跨回调访问；页面常驻、只构建一次，
 * 所以用文件级静态句柄即可，不必往 app_ui_internal.h 里加字段。 */
static lv_obj_t *s_lbl_clock_time;
static lv_obj_t *s_lbl_clock_date;

static void clock_poll_cb(lv_timer_t *timer)
{
    (void)timer;

    const time_t now = time(NULL);
    if (now < CLOCK_VALID_EPOCH) {
        /* SNTP 还没同步：显示占位符，绝不显示 1970 */
        lv_label_set_text(s_lbl_clock_time, "--:--:--");
        lv_obj_set_style_text_color(s_lbl_clock_time, COLOR_STALE, 0);
        lv_label_set_text(s_lbl_clock_date, "对时中");
        return;
    }

    struct tm tm_now;
    localtime_r(&now, &tm_now);   /* 时区由 main.c 里的 setenv(TZ, CST-8) 决定 */

    static const char *const WEEKDAY[] = {
        "日", "一", "二", "三", "四", "五", "六",
    };

    char line[32];
    snprintf(line, sizeof(line), "%02d:%02d:%02d",
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    lv_label_set_text(s_lbl_clock_time, line);
    lv_obj_set_style_text_color(s_lbl_clock_time, COLOR_ACCENT, 0);

    /* 日期和星期分两步拼接：%04d 在 tm_year 异常时可能展开成负数，
     * 编译器会按「最长整数」估算 %04d 的宽度并对 32 字节缓冲报截断告警。
     * 先落 16 字节日期串，再拼星期，宽度就确定了。 */
    char date[16];
    snprintf(date, sizeof(date), "%04u-%02u-%02u",
             (unsigned)(tm_now.tm_year + 1900),
             (unsigned)(tm_now.tm_mon + 1),
             (unsigned)tm_now.tm_mday);
    snprintf(line, sizeof(line), "%s 星期%s", date,
             WEEKDAY[tm_now.tm_wday % 7]);
    lv_label_set_text(s_lbl_clock_date, line);
}

/* ---------------------------------------------------------------- 页面构建 */

void ui_page_overview_build(lv_obj_t *page)
{
    /* ---- 上半区：每设备一张摘要卡 ---- */
    for (int slot = 0; slot < UI_DEVICE_SLOTS; slot++) {
        create_device_card(page, slot, DEV_CARD_X0 + slot * (DEV_CARD_W + DEV_CARD_GAP));
    }

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

    /* ---- 底部右：实时时钟（沿用原 MQTT 卡那一格） ---- */
    lv_obj_t *card_clock = ui_card(page, 468, 150);
    lv_obj_align(card_clock, LV_ALIGN_BOTTOM_RIGHT, -32, -24);

    lv_obj_t *clock_title = ui_label(card_clock, &lv_font_cjk_16, COLOR_MUTED, "当前时间");
    lv_obj_set_pos(clock_title, 0, 0);

    /* HH:MM:SS 全是 ASCII，cjk_28 里本来就是完整的 0x20-0x7f 子集 */
    s_lbl_clock_time = ui_label(card_clock, &lv_font_cjk_28, COLOR_STALE, "--:--:--");
    lv_obj_set_pos(s_lbl_clock_time, 0, 24);

    s_lbl_clock_date = ui_label(card_clock, &lv_font_cjk_16, COLOR_MUTED, "对时中");
    lv_obj_align(s_lbl_clock_date, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* ---- 定时器 ---- */
    lv_timer_create(devices_poll_cb, METRICS_POLL_PERIOD_MS, NULL);
    lv_timer_create(net_poll_cb, NET_POLL_PERIOD_MS, NULL);
    lv_timer_create(clock_poll_cb, CLOCK_POLL_PERIOD_MS, NULL);

    /* 首帧就填上真实状态，避免开机先闪一下占位文字 */
    devices_poll_cb(NULL);
    net_poll_cb(NULL);
    clock_poll_cb(NULL);
}