/*
 * 运行日志环形缓冲（诊断日志卡）
 *
 * 设计理由：ESP32-P4 的屏幕在设备现场，而串口只有调试时接着。出问题时
 * 最想知道「刚才发生了什么」，所以把最近几条日志留在内存里给设置页看。
 *
 * 实现约束：
 *   - 16 行 × 160 字节的静态环形缓冲 + 静态互斥量，运行期零动态分配；
 *   - 钩子里先调用保存的原始 vprintf，串口输出完全保持原样；
 *   - 钩子内绝不调用 ESP_LOG（会递归进自己）；
 *   - 只保留最近 16 行，屏幕一次取 6 行，足够覆盖一次断线重连的过程。
 *
 * 容量取舍：160 字节一行只能放下「时间戳 + 级别 + TAG + 一句短说明」，
 * 例如 "I (12345) eth_link: link up 100M full"。更长的行按 UTF-8
 * 边界截到 155 字节并补 "..."，不硬切半个汉字（半个 UTF-8 序列会让
 * LVGL 显示异常）。
 */

#include "log_ring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#define LOG_RING_LINES      (16)
#define LOG_RING_LINE_LEN   (160)

/* 截断时给 "..." 留 3 字节，且保证是 UTF-8 边界 */
#define LOG_RING_TEXT_MAX   (LOG_RING_LINE_LEN - 5)

static char              s_lines[LOG_RING_LINES][LOG_RING_LINE_LEN];
static int               s_head;        /* 下一条写入的位置 */
static int               s_count;       /* 已存行数，最多 LOG_RING_LINES */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static vprintf_like_t    s_orig_vprintf;
static bool              s_initialized;

/* ------------------------------------------------------------ 内部工具 */

/**
 * 把截断点回退到 UTF-8 字符边界，避免出现半个汉字。
 *
 * s 是合法 UTF-8，max < strlen(s)。若 s[max] 是续字节（10xxxxxx），
 * 说明第 max 字节处正落在一个多字节字符中间，往前退到该字符的起始字节。
 */
static size_t utf8_truncate(const char *s, size_t max)
{
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    return n;
}

/** 把一段（不含 '\n' 的）文本放进环形缓冲 */
static void push_line(const char *text, size_t len)
{
    /* 去掉行尾 '\r'（日志经串口工具回传时可能出现） */
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == ' ')) {
        len--;
    }
    if (len == 0) {
        return;
    }

    char *dst = s_lines[s_head];
    size_t keep = len;

    if (keep > LOG_RING_TEXT_MAX) {
        keep = utf8_truncate(text, LOG_RING_TEXT_MAX);
        memcpy(dst, text, keep);
        memcpy(dst + keep, "...", 3);
        dst[keep + 3] = '\0';
    } else {
        memcpy(dst, text, keep);
        dst[keep] = '\0';
    }

    s_head = (s_head + 1) % LOG_RING_LINES;
    if (s_count < LOG_RING_LINES) {
        s_count++;
    }
}

/**
 * 日志钩子：先原样转发给串口，再把完整行按 '\n' 切开放进环形缓冲。
 *
 * 注意：这里的 fmt 已经是 ESP_LOG 拼好的完整行（含时间戳、TAG 和结尾换行），
 * 所以一条日志可能包含多个 '\n'（例如调用方自己带换行），按段处理。
 */
static int log_ring_hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    int ret;
    if (s_orig_vprintf != NULL) {
        ret = s_orig_vprintf(fmt, args);
    } else {
        ret = vprintf(fmt, args);
    }

    char buf[LOG_RING_LINE_LEN];
    const int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    if (n > 0 && s_lock != NULL && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        /* 只处理实际写进缓冲的部分；超长行也按 vsnprintf 的截断结果入环 */
        size_t used = (size_t)n;
        if (used > sizeof(buf) - 1) {
            used = sizeof(buf) - 1;
        }

        size_t start = 0;
        for (size_t i = 0; i <= used; i++) {
            if (i == used || buf[i] == '\n') {
                if (i > start) {
                    push_line(buf + start, i - start);
                }
                start = i + 1;
            }
        }
        xSemaphoreGive(s_lock);
    }

    return ret;
}

/* ------------------------------------------------------------ 对外接口 */

void log_ring_init(void)
{
    if (s_initialized) {
        return;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    memset(s_lines, 0, sizeof(s_lines));
    s_head = 0;
    s_count = 0;
    s_initialized = true;

    /* esp_log_set_vprintf 返回原来的输出函数：保留它，
     * 这样串口日志的行为与安装钩子之前完全一致。 */
    if (s_orig_vprintf == NULL) {
        s_orig_vprintf = esp_log_set_vprintf(log_ring_hook);
    }
}

size_t log_ring_count(void)
{
    /* s_count 只在持锁时递增，但它是 32 位对齐整数，单个读者读到的
     * 旧值最多比实际少一行，不会撕裂；这里刻意不加锁，避免 UI 线程等待。 */
    return (size_t)s_count;
}

size_t log_ring_snapshot(char *out, size_t out_len, int max_lines)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }
    out[0] = '\0';

    if (max_lines <= 0 || s_lock == NULL) {
        return 0;
    }

    /* 取锁超时为 0：snapshot 由 LVGL 定时器调用，绝不能在 UI 线程等待。
     * 钩子里的临界区只有一次 memcpy，拿不到锁就这一拍不更新，下一拍再来。 */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return 0;
    }

    int lines = s_count;
    if (lines > max_lines) {
        lines = max_lines;
    }
    if (lines > LOG_RING_LINES) {
        lines = LOG_RING_LINES;
    }

    size_t pos = 0;
    int    written = 0;

    /* 最早一行在 s_head - lines 处（环形），依次往外拷贝，最新一行在最后 */
    for (int i = 0; i < lines; i++) {
        const int idx = (s_head - lines + i + LOG_RING_LINES) % LOG_RING_LINES;
        const char *src = s_lines[idx];

        size_t need = strlen(src);
        if (i > 0) {
            need += 1;      /* 分隔符 '\n' */
        }
        if (pos + need + 1 > out_len) {
            /* 放不下就停在这里：宁可少一行，也不留半行 */
            break;
        }
        if (i > 0) {
            out[pos++] = '\n';
        }
        memcpy(out + pos, src, strlen(src));
        pos += strlen(src);
        written++;
    }
    out[pos] = '\0';

    xSemaphoreGive(s_lock);
    return (size_t)written;
}