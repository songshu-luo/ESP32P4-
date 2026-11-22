/*
 * 界面内部共用声明（只给 main/ 下的 UI 源文件用，不对外暴露）
 *
 * 拆分原因：S2-d 加入顶部状态栏和三个页面后，app_ui.c 会超过 800 行。
 * 这里集中放配色、控件句柄和跨文件辅助函数声明。
 *
 * S2-e：总览页从「一张设备的四张指标卡」改成「每设备一张摘要卡」，
 * 卡片控件改成按槽位的数组。槽位上限与 device_registry 一致（4）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/* 与 DEVICE_REGISTRY_MAX_DEVICES 保持一致；UI 层不 include registry 头，
 * 只用同一个数字，改了那边记得同步这里（有编译期一致性靠不了编译器，靠约定）。 */
#define UI_DEVICE_SLOTS (4)

typedef struct {
    /* ---------- 顶部常驻状态栏 ---------- */
    lv_obj_t *lamp;
    lv_obj_t *lbl_status;      /* 全部在线 / N 台离线 / 等待设备 / 未知 */
    lv_obj_t *lbl_page;        /* 当前页面名 */
    lv_obj_t *lbl_clock_warn;  /* 有设备 clock_synced=false 时的提示，平时隐藏 */

    /* ---------- 总览页：每设备一张摘要卡 ---------- */
    lv_obj_t *lbl_dev_name[UI_DEVICE_SLOTS];    /* 泰山派 3M / Windows 主机 / 原始 id */
    lv_obj_t *lbl_dev_state[UI_DEVICE_SLOTS];   /* 在线 / 离线 / 过期 / 未知 / 等待 */
    lv_obj_t *lbl_dev_cpu[UI_DEVICE_SLOTS];     /* CPU 大字，未上报显示 -- */
    lv_obj_t *lbl_dev_mem[UI_DEVICE_SLOTS];     /* 内存大字 */
    lv_obj_t *lbl_dev_detail[UI_DEVICE_SLOTS];  /* 等待 Hub 数据 / 数据已过期 / 未上报 */

    /* ---------- 总览页：网络 / MQTT 卡 ---------- */
    lv_obj_t *lbl_net_state;
    lv_obj_t *lbl_net_ip;
    lv_obj_t *lbl_net_detail;
    lv_obj_t *lbl_mqtt_state;
    lv_obj_t *lbl_mqtt_msg;
    lv_obj_t *lbl_mqtt_detail;

    /* ---------- 监控页：设备标题 ---------- */
    lv_obj_t *lbl_mon_device;      /* 当前设备名（中文名，未登记时为原始 id） */
    lv_obj_t *lbl_mon_id;          /* 协议 device_id，跟在名字下面，便于对日志 */
    lv_obj_t *lbl_mon_link;        /* 数据在线 / 数据已过期 / 设备离线 / 等待数据 */
    lv_obj_t *lbl_mon_age;         /* 更新于 N s 前，让人知道数字有多旧 */
    lv_obj_t *btn_mon_prev;        /* ◀ 上一个设备 */
    lv_obj_t *btn_mon_next;        /* ▶ 下一个设备 */

    /* ---------- 监控页：CPU / GPU 大卡（480x240） ---------- */
    lv_obj_t *lbl_cg_title;        /* 「CPU / GPU」或「CPU / 容器」 */
    lv_obj_t *lbl_cg_gpu_name;     /* 右上角显卡短名，例如 RTX 4060 Laptop */
    lv_obj_t *val_cpu_big;         /* CPU 占用大字 */
    lv_obj_t *val_gpu_big;         /* GPU 利用率大字 */
    lv_obj_t *bar_cpu;             /* CPU 占用进度条（0~100） */
    lv_obj_t *bar_gpu;             /* GPU 利用率进度条（0~100） */
    lv_obj_t *det_cpu;             /* 「16 线程」 */
    lv_obj_t *det_cpu2;            /* 「采样 2 s」 */
    lv_obj_t *lbl_gpu_temp_cap;    /* 「温度」两个字的标签 */
    lv_obj_t *val_gpu_temp;        /* 「43 ℃」 */
    lv_obj_t *det_gpu_mem;         /* 「显存 1.2 / 8.0 GB」 */

    /* 右半列：有 GPU 显示 GPU，没有 GPU 的机器显示运行中的容器 */
    lv_obj_t *gpu_col;             /* 216x208 容器（GPU 那一列） */
    lv_obj_t *cont_col;            /* 同坐标的容器列，默认隐藏 */
    lv_obj_t *lbl_cont_count;      /* 右上「2 个」 */
    lv_obj_t *lbl_cont_name[4];    /* 容器名（优先 compose service 名） */
    lv_obj_t *dot_cont[4];         /* 每行前面的绿点 */
    lv_obj_t *lbl_cont_note;       /* 无数据时的说明 */

    /* ---------- 监控页：右侧卡（进程 / 温区，二选一显示） ---------- */
    lv_obj_t *card_proc;           /* 464x240 进程卡 */
    lv_obj_t *lbl_proc_title;      /* 「进程」+ 右上「按 CPU 排序」 */
    lv_obj_t *lbl_proc_hint;
    lv_obj_t *lbl_proc_th[3];      /* 表头：进程 / CPU / 内存 */
    lv_obj_t *lbl_proc_name[3];
    lv_obj_t *lbl_proc_cpu[3];
    lv_obj_t *lbl_proc_mem[3];
    lv_obj_t *lbl_proc_foot;       /* 「共 214 个进程 · 仅列出前 3」 */

    lv_obj_t *card_zone;           /* 同一坐标的温区卡（Linux 回退） */
    lv_obj_t *lbl_zone_title;
    lv_obj_t *lbl_zone_name[6];
    lv_obj_t *lbl_zone_val[6];

    /* ---------- 监控页：左下卡（网络 / 平均负载 / 运行时长，按能力切换） ---------- */
    lv_obj_t *card_net;            /* 304x140 网络卡 */
    /* 旧单行样式：IP + 单卡 ↓/↑（v1–v5 设备，或没有分网卡明细时） */
    lv_obj_t *lbl_mon_net_ip;
    lv_obj_t *lbl_mon_net_rx;
    lv_obj_t *lbl_mon_net_tx;
    lv_obj_t *lbl_mon_net_note;
    /* 新两行样式（v6 netifs[]）：右上角「当前 有线/无线」，两行分别是有线 / 无线 */
    lv_obj_t *lbl_net_active;
    lv_obj_t *lbl_net_if[2];       /* 左侧「有线 1000M」/「无线 未接入」 */
    lv_obj_t *lbl_net_rate[2];     /* 同一行右侧「↓1.2K ↑3.4K」 */

    lv_obj_t *card_load;           /* 同一坐标的负载/运行时长卡（网络缺失时的回退） */
    lv_obj_t *lbl_load_title;      /* 「平均负载」或「运行时长」 */
    lv_obj_t *val_load1;
    lv_obj_t *val_load5;
    lv_obj_t *val_load15;
    lv_obj_t *val_uptime;          /* 「3 天 5 小时」，与 load 三值互斥显示 */
    lv_obj_t *lbl_load_note;       /* 底部说明：1/5/15 分钟 或 开机至今 */

    /* ---------- 监控页：内存 / 交换 / 磁盘详情 ---------- */
    lv_obj_t *lbl_mon_disk_title;
    lv_obj_t *lbl_disk_parts;      /* 标题右侧「共 3 个分区」 */
    lv_obj_t *det_disk_parts;      /* 旧底部行，已不再使用，保留字段避免影响别处 */
    lv_obj_t *lbl_disk_row[3];     /* 磁盘卡三行：左侧「C: 48.7%」 */
    lv_obj_t *lbl_disk_val[3];     /* 磁盘卡三行：右侧「243.6 / 500.0 GB」 */
    lv_obj_t *val_mon_mem;
    lv_obj_t *det_mon_mem;
    lv_obj_t *val_mon_swap;
    lv_obj_t *val_mon_disk;
    lv_obj_t *det_mon_disk;
} app_ui_ctx_t;

extern app_ui_ctx_t s_ctx;

/* 监控页当前显示的设备槽位（◀ ▶ 按钮改它，轮询回调读它） */
extern int s_mon_slot;

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
void ui_page_settings_build(lv_obj_t *page);
void ui_page_placeholder_build(lv_obj_t *page, const char *title, const char *note);