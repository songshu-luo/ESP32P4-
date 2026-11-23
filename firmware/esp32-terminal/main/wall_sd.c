/*
 * SD 卡休眠壁纸模块
 *
 * 从 /sdcard/wall 下读取 1024x600 baseline JPEG，用 ESP32-P4 硬件 JPEG
 * 解码器解成 RGB565；后台任务预解码到 back buffer，息屏时由 UI 取走。
 *
 * 线程模型：
 *   - 一个后台任务负责挂载 / 扫描 / 解码，绝不碰 LVGL；
 *   - wall_sd_take_ready() 从 LVGL 任务调用，只用 portMUX 做一次极短的
 *     指针交换，不加 LVGL 锁、不做文件 IO；
 *   - ready / front / back 由 s_state_mux 保护，其余（列表、挂载状态）
 *     只由任务写，take/available 只读。
 */

#include "wall_sd.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/jpeg_decode.h"

#include "bsp/esp32_p4_function_ev_board.h"

static const char *TAG = "wall_sd";

#define WALL_DIR            "/sdcard/wall"
#define WALL_W              1024
#define WALL_H              600
#define WALL_STRIDE         (WALL_W * 2)
#define WALL_DATA_SIZE      (WALL_W * WALL_H * 2)

/* JPEG 硬件按 MCU 对齐：600 行不是 16 的倍数，驱动会把输出行数补到 608，
 * 因此输出缓冲要按 608 行分配；lv_image_dsc_t 仍只描述可见的 600 行。 */
#define WALL_DEC_H          608
#define WALL_DEC_SIZE       (WALL_W * WALL_DEC_H * 2)

#define WALL_MAX_FILES      32
#define WALL_MAX_FILE_SIZE  (1024 * 1024)   /* 1 MB，超过直接跳过 */
#define WALL_MOUNT_RETRY_MS 5000
#define WALL_DECODE_TRIES   3
#define WALL_TASK_STACK     4096
#define WALL_TASK_PRIO      4

typedef struct {
    char name[16];
    int  index;
} wall_file_t;

/* 图描述符字段顺序照抄 assets/sleep_wall.c，只换成本模块的缓冲。 */
static lv_image_dsc_t s_dsc[2] = {
    {
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.flags = 0,
        .header.w = WALL_W,
        .header.h = WALL_H,
        .header.stride = WALL_STRIDE,
        .data_size = WALL_DATA_SIZE,
        .data = NULL,
    },
    {
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.flags = 0,
        .header.w = WALL_W,
        .header.h = WALL_H,
        .header.stride = WALL_STRIDE,
        .data_size = WALL_DATA_SIZE,
        .data = NULL,
    },
};

static uint8_t *s_buf[2];
static size_t   s_buf_size[2];              /* jpeg_alloc_decoder_mem 实际对齐后的尺寸 */

static SemaphoreHandle_t s_prime_sem;
static portMUX_TYPE      s_state_mux = portMUX_INITIALIZER_UNLOCKED;

/* 由 s_state_mux 保护 */
static bool s_ready;
static int  s_front_idx = 1;     /* 首次解码写到 buf0，取走后 front 才变成 0 */
static int  s_back_idx;          /* 默认 0：第一次解码写 buf0 */
static bool s_avail;

/* 只由任务/init 写，take 不读 */
static wall_file_t s_files[WALL_MAX_FILES];
static int         s_count;
static bool        s_mounted;
static int         s_last_idx = -1;         /* 上次成功解码的下标 */
static int64_t     s_last_mount_try_ms;
static int         s_mount_fail_count;
static jpeg_decoder_handle_t s_engine;

/* 空目录会每 5 s 重扫一次：只有结果变化时才重复 WARN，避免串口刷屏 */
static int         s_last_warn_count = -1;

/* ------------------------------------------------------------ 小工具 */

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* 只接受 8.3 短名 wNN.jpg（大小写不敏感），路径由调用方拼接 */
static bool is_wall_name(const char *name, int *out_index)
{
    if (name == NULL || strlen(name) != 7) {
        return false;
    }
    if (name[0] != 'w' && name[0] != 'W') {
        return false;
    }
    if (!is_digit(name[1]) || !is_digit(name[2])) {
        return false;
    }
    if (name[3] != '.') {
        return false;
    }
    if ((name[4] != 'j' && name[4] != 'J') ||
        (name[5] != 'p' && name[5] != 'P') ||
        (name[6] != 'g' && name[6] != 'G')) {
        return false;
    }
    if (out_index != NULL) {
        *out_index = (name[1] - '0') * 10 + (name[2] - '0');
    }
    return true;
}

static void scan_dir(void)
{
    DIR *dir = opendir(WALL_DIR);
    if (dir == NULL) {
        s_count = 0;
        taskENTER_CRITICAL(&s_state_mux);
        s_avail = false;
        taskEXIT_CRITICAL(&s_state_mux);
        if (s_last_warn_count != 0) {
            s_last_warn_count = 0;
            ESP_LOGW(TAG, "opendir(%s) failed - fallback to built-in wallpaper", WALL_DIR);
        }
        return;
    }

    int count = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        int index = 0;
        if (!is_wall_name(ent->d_name, &index)) {
            continue;
        }
        if (count < WALL_MAX_FILES) {
            strncpy(s_files[count].name, ent->d_name, sizeof(s_files[count].name) - 1);
            s_files[count].name[sizeof(s_files[count].name) - 1] = '\0';
            s_files[count].index = index;
            count++;
        }
    }
    closedir(dir);

    /* 按序号升序；同序号按名字稳定区分（正常不会重复） */
    for (int i = 1; i < count; i++) {
        wall_file_t key = s_files[i];
        int j = i - 1;
        while (j >= 0 && (s_files[j].index > key.index ||
                          (s_files[j].index == key.index &&
                           strcmp(s_files[j].name, key.name) > 0))) {
            s_files[j + 1] = s_files[j];
            j--;
        }
        s_files[j + 1] = key;
    }

    s_count = count;
    taskENTER_CRITICAL(&s_state_mux);
    s_avail = (count > 0);
    taskEXIT_CRITICAL(&s_state_mux);

    if (count == 0) {
        if (s_last_warn_count != 0) {
            ESP_LOGW(TAG, "no wNN.jpg found in %s - fallback to built-in wallpaper", WALL_DIR);
        }
    } else {
        ESP_LOGI(TAG, "scan %s: %d file(s), first=%s last=%s", WALL_DIR, count,
                 s_files[0].name, s_files[count - 1].name);
    }
    s_last_warn_count = count;
}

/* 未挂载时尝试挂载并扫描；返回 true 表示列表可用 */
static bool mount_and_scan(bool from_init)
{
    if (s_mounted) {
        if (s_count == 0) {
            scan_dir();
        }
        return s_count > 0;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (!from_init && s_last_mount_try_ms != 0 &&
        (now_ms - s_last_mount_try_ms) < WALL_MOUNT_RETRY_MS) {
        return false;
    }
    s_last_mount_try_ms = now_ms;

    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_ERR_INVALID_STATE) {
        /* BSP 内部的 LDO / 挂载点已存在：视为已挂载 */
        ESP_LOGI(TAG, "sdcard already mounted (%s)", esp_err_to_name(err));
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        s_mount_fail_count++;
        /* 首次失败和之后每 12 次（约 1 分钟）打一条 WARN，其余降为 DEBUG：
         * 没插卡时不能每 5 s 刷一行，把诊断日志环刷掉。 */
        if (s_mount_fail_count == 1 || (s_mount_fail_count % 12) == 0) {
            ESP_LOGW(TAG, "sdcard mount failed: %s (attempt %d, retry every %d ms)",
                     esp_err_to_name(err), s_mount_fail_count, WALL_MOUNT_RETRY_MS);
        } else {
            ESP_LOGD(TAG, "sdcard mount failed: %s (attempt %d)",
                     esp_err_to_name(err), s_mount_fail_count);
        }
        /* BSP 的 mount 在失败路径上不会释放自己刚申请的 LDO 通道；
         * 不回收的话下一次 sd_pwr_ctrl_new_on_chip_ldo() 会一直报
         * “already in use”，5 s 重试就永远不可能成功。 */
        (void)bsp_sdcard_unmount();
        s_mounted = false;
        s_count = 0;
        taskENTER_CRITICAL(&s_state_mux);
        s_avail = false;
        taskEXIT_CRITICAL(&s_state_mux);
        return false;
    }

    s_mount_fail_count = 0;
    s_mounted = true;
    ESP_LOGI(TAG, "sdcard mounted at %s", BSP_SD_MOUNT_POINT);
    scan_dir();
    return s_count > 0;
}

static bool ensure_buffers(void)
{
    if (s_buf[0] != NULL && s_buf[1] != NULL) {
        return true;
    }

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    for (int i = 0; i < 2; i++) {
        if (s_buf[i] != NULL) {
            continue;
        }
        size_t allocated = 0;
        s_buf[i] = (uint8_t *)jpeg_alloc_decoder_mem(WALL_DEC_SIZE, &mem_cfg, &allocated);
        if (s_buf[i] == NULL) {
            ESP_LOGE(TAG, "alloc output buffer %d (%d bytes) failed", i, WALL_DEC_SIZE);
            return false;
        }
        s_buf_size[i] = allocated;
        s_dsc[i].data = s_buf[i];
        ESP_LOGI(TAG, "output buffer %d: %u bytes (requested %d)",
                 i, (unsigned)allocated, WALL_DEC_SIZE);
    }
    return true;
}

static esp_err_t ensure_engine(void)
{
    if (s_engine != NULL) {
        return ESP_OK;
    }

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };
    esp_err_t err = jpeg_new_decoder_engine(&eng_cfg, &s_engine);
    if (err != ESP_OK) {
        s_engine = NULL;
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "jpeg decoder engine created");
    return ESP_OK;
}

/* 随机挑一张；列表 >1 时避开 avoid_idx */
static int pick_index(int avoid_idx)
{
    if (s_count <= 0) {
        return -1;
    }
    if (s_count == 1) {
        return 0;
    }
    int idx = (int)(esp_random() % (uint32_t)s_count);
    if (idx == avoid_idx) {
        idx = (idx + 1) % s_count;
    }
    return idx;
}

/* 解一张到 s_back；成功返回 true（此时 ready 已置位） */
static bool decode_one(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return false;
    }

    /* dirent 名字上限 256，路径缓冲留足空间，免得 -Wformat-truncation 报错；
     * 实际名字是校验过的 8.3 短名 */
    char path[320];
    snprintf(path, sizeof(path), WALL_DIR "/%s", s_files[idx].name);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "skip %s: fopen failed", path);
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "skip %s: fseek failed", path);
        fclose(fp);
        return false;
    }
    long file_size = ftell(fp);
    if (file_size <= 0 || file_size > WALL_MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "skip %s: bad size %ld (limit %d)", path, file_size, WALL_MAX_FILE_SIZE);
        fclose(fp);
        return false;
    }
    rewind(fp);

    jpeg_decode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t in_alloc = 0;
    uint8_t *in_buf = (uint8_t *)jpeg_alloc_decoder_mem((size_t)file_size, &in_cfg, &in_alloc);
    if (in_buf == NULL) {
        ESP_LOGE(TAG, "skip %s: alloc input buffer (%ld bytes) failed", path, file_size);
        fclose(fp);
        return false;
    }

    const size_t rd = fread(in_buf, 1, (size_t)file_size, fp);
    fclose(fp);
    if (rd != (size_t)file_size) {
        ESP_LOGW(TAG, "skip %s: fread %u of %ld bytes", path, (unsigned)rd, file_size);
        free(in_buf);
        return false;
    }

    jpeg_decode_picture_info_t info = {0};
    esp_err_t err = jpeg_decoder_get_info(in_buf, (uint32_t)file_size, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "skip %s: get_info failed: %s", path, esp_err_to_name(err));
        free(in_buf);
        return false;
    }
    if (info.width != WALL_W || info.height != WALL_H) {
        ESP_LOGW(TAG, "skip %s: %ux%u, expect %dx%d (no scaling)",
                 path, (unsigned)info.width, (unsigned)info.height, WALL_W, WALL_H);
        free(in_buf);
        return false;
    }

    if (ensure_engine() != ESP_OK) {
        free(in_buf);
        return false;
    }

    const jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    /* 此处只有任务/init 会解码，s_back_idx 也不会被 take 改成同一个值：
     * take 只会在 ready=true 时调用，而 ready=true 时任务不会解码。 */
    int back = 0;
    taskENTER_CRITICAL(&s_state_mux);
    back = s_back_idx;
    taskEXIT_CRITICAL(&s_state_mux);

    uint32_t out_size = 0;
    const int64_t t0_us = esp_timer_get_time();
    err = jpeg_decoder_process(s_engine, &dec_cfg, in_buf, (uint32_t)file_size,
                               s_buf[back], (uint32_t)s_buf_size[back], &out_size);
    const int64_t cost_ms = (esp_timer_get_time() - t0_us) / 1000;
    free(in_buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "decode %s failed: %s (%lld ms)", path, esp_err_to_name(err),
                 (long long)cost_ms);
        return false;
    }

    taskENTER_CRITICAL(&s_state_mux);
    s_ready = true;
    taskEXIT_CRITICAL(&s_state_mux);

    s_last_idx = idx;
    ESP_LOGI(TAG, "decoded %s (%ld bytes) in %lld ms, out %u bytes",
             path, file_size, (long long)cost_ms, (unsigned)out_size);
    return true;
}

/* 连续失败最多 WALL_DECODE_TRIES 次；成功或次数用尽返回 */
static bool decode_next(void)
{
    int avoid = s_last_idx;
    for (int attempt = 0; attempt < WALL_DECODE_TRIES; attempt++) {
        const int idx = pick_index(avoid);
        if (idx < 0) {
            return false;
        }
        if (decode_one(idx)) {
            return true;
        }
        avoid = idx;    /* 下一轮别在同一张上反复撞 */
    }
    ESP_LOGE(TAG, "decode failed after %d attempts", WALL_DECODE_TRIES);
    return false;
}

static void wall_sd_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 卡不在位（或目录为空）时用 5 s 超时自己醒来重试挂载/扫描；
         * 正常就绪后 portMAX_DELAY，只由 PRIME 事件驱动解码。
         * 多等 1 个 tick，避免和 mount_and_scan 里的 5 s 限频卡在同一时刻。 */
        const TickType_t wait_ticks =
            (s_mounted && s_count > 0) ? portMAX_DELAY
                                       : pdMS_TO_TICKS(WALL_MOUNT_RETRY_MS) + 1;

        if (xSemaphoreTake(s_prime_sem, wait_ticks) != pdTRUE) {
            (void)mount_and_scan(false);
            continue;
        }

        if (!mount_and_scan(false)) {
            continue;
        }
        if (!ensure_buffers()) {
            continue;
        }

        bool ready = false;
        taskENTER_CRITICAL(&s_state_mux);
        ready = s_ready;
        taskEXIT_CRITICAL(&s_state_mux);
        if (ready) {
            /* 已有一张没被取走：prime 幂等，不再覆盖 back */
            continue;
        }

        (void)decode_next();
    }
}

/* ------------------------------------------------------------ 对外接口 */

void wall_sd_init(void)
{
    if (s_prime_sem != NULL) {
        /* 幂等：重复调用直接忽略（比如上层误触发两次 init） */
        return;
    }
    s_prime_sem = xSemaphoreCreateBinary();
    if (s_prime_sem == NULL) {
        ESP_LOGE(TAG, "create prime semaphore failed");
        return;
    }

    /* 先同步挂载 / 扫描 / 解第一张，再起任务：避免 init 与任务同时进解码器。
     * 卡不在位或目录为空时这一步只留下日志，不阻塞后续启动流程，
     * 输出缓冲也留到确认有可用壁纸之后才分配，省 2.5 MB PSRAM。 */
    const bool have_wall = mount_and_scan(true);
    if (have_wall) {
        if (!ensure_buffers()) {
            ESP_LOGE(TAG, "output buffers unavailable, module disabled");
        } else {
            (void)decode_next();
        }
    }

    if (xTaskCreate(wall_sd_task, "wall_sd", WALL_TASK_STACK, NULL,
                    WALL_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create wall_sd task failed");
    }
}

void wall_sd_prime(void)
{
    if (s_prime_sem != NULL) {
        xSemaphoreGive(s_prime_sem);
    }
}

const lv_image_dsc_t *wall_sd_take_ready(void)
{
    const lv_image_dsc_t *ret = NULL;

    taskENTER_CRITICAL(&s_state_mux);
    if (s_ready) {
        /* 刚解码完的 back 变成 front；旧 front 变成下一张的 back */
        s_front_idx = s_back_idx;
        s_back_idx = 1 - s_front_idx;
        s_ready = false;
        ret = &s_dsc[s_front_idx];
    }
    taskEXIT_CRITICAL(&s_state_mux);

    return ret;
}

bool wall_sd_available(void)
{
    bool avail = false;

    taskENTER_CRITICAL(&s_state_mux);
    avail = s_avail;
    taskEXIT_CRITICAL(&s_state_mux);

    return avail;
}