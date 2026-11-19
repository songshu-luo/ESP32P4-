/*
 * 界面内部共用声明（只给 main/ 下的 UI 源文件用，不对外暴露）
 *
 * 拆分原因：S2-d 加入顶部状态栏和三个页面后，app_ui.c 会超过 800 行。
 * 这里集中放配色、控件句柄和跨文件辅助函数声明。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    /* ---------- 顶部常驻状态栏 ---------- */
    lv_obj_t *lamp;
    lv_obj_t *lbl_status;      /* 在线 / 离线 / 过期 / 未知 */
    lv_obj_t *lbl_page;        /* 当前页面名 */
    lv_obj_t *lbl_clock_warn;  /* clock_synced=false 的提示，平时隐藏 */

    /* ---------- 总览页：指标卡 ---------- */
    lv_obj_t *val_cpu;
    lv_obj_t *det_cpu;
    lv_obj_t *val_mem;
    lv_obj_t *det_mem;
    lv_obj_t *val_temp;
    lv_obj_t *det_temp;
    lv_obj_t *val_disk;
    lv_obj_t *det_disk;

    /* ---------- 总览页：网络 / MQTT 卡 ---------- */
    lv_obj_t *lbl_net_state;
    lv_obj_t *lbl_net_ip;
    lv_obj_t *lbl_net_detail;
    lv_obj_t *lbl_mqtt_state;
    lv_obj_t *lbl_mqtt_msg;
    lv_obj_t *lbl_mqtt_detail;

    /* ---------- 监控页：设备标题 ---------- */
    lv_obj_t *lbl_mon_device;      /* "泰山派 3M" */
    lv_obj_t *lbl_mon_link;        /* 在线 / 过期 / 等待数据 */

    /* ---------- 监控页：六个温区 ---------- */
    lv_obj_t *val_zone[6];
    lv_obj_t *det_zone[6];         /* 温区名，例如 soc-thermal */

    /* ---------- 监控页：负载 1·5·15 ---------- */
    lv_obj_t *val_load1;
    lv_obj_t *val_load5;
    lv_obj_t *val_load15;

    /* ---------- 监控页：内存 / 交换 / 磁盘详情 ---------- */
    lv_obj_t *lbl_mon_disk_title;
    lv_obj_t *val_mon_mem;
    lv_obj_t *det_mon_mem;
    lv_obj_t *val_mon_swap;
    lv_obj_t *val_mon_disk;
    lv_obj_t *det_mon_disk;

    /* ---------- 监控页：服务 ---------- */
    lv_obj_t *lbl_svc[6];
    lv_obj_t *lbl_svc_state[6];
} app_ui_ctx_t;

extern app_ui_ctx_t s_ctx;

/* 配色：深色背景 + 高对比前景，1024x600 远距离可读 */
#define COLOR_BG        lv_color_hex(0x101418)
#define COLOR_BAR       lv_color_hex(0x151a23)   /* 状态栏 / 页签栏，比卡片暗一档 */
#define COLOR_CARD      lv_color_hex(0x1c2230)
#define COLOR_ACCENT    lv_color_hex(0x29b6f6)
#define COLOR_TEXT      lv_color_hex(0xe8eef6)
#define COLOR_MUTED     lv_color_hex(0x8a97a8)
#define COLOR_OK        lv_color_hex(0x43a047)
#define COLOR_WARN      lv_color_hex(0xffb300)
#define COLOR_ERR       lv_color_hex(0xef5350)
#define COLOR_STALE     lv_color_hex(0x55606e)   /* 过期数据的灰，比 MUTED 更暗 */

/* 顶部常驻状态栏高度：跨页面固定，页签栏从这一行之下开始 */
#define STATUS_BAR_H    (56)
#define TAB_BAR_H       (56)

/** 圆角卡片：去掉默认样式、不滚动、内边距 16 */
lv_obj_t *ui_card(lv_obj_t *parent, int32_t w, int32_t h);

/** 指定字体与颜色的标签 */
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font,
                   lv_color_t color, const char *text);

/** 字节数 → "1.7 / 3.8 GB"，避免在屏幕上出现 10 位数字。
 *  total <= 0（字段缺失）时返回 false，让调用方换成别的说明，
 *  而不是显示成 "0 / 0 MB"。 */
bool ui_format_bytes_pair(char *buf, size_t len, double used, double total);

/* 页面构建（每个页面自己往传进来的 page 里塞控件） */
void ui_page_overview_build(lv_obj_t *page);
void ui_page_monitor_build(lv_obj_t *page);
void ui_page_placeholder_build(lv_obj_t *page, const char *title, const char *note);