/*
 * 主界面背景图：/sdcard/bg.jpg
 *
 * SD 卡由 wall_sd 挂载（本模块不重复调用 bsp_sdcard_mount，否则会报
 * “already in use”），所以 main.c 必须在 wall_sd_init() 之后才调这里。
 *
 * 解码只做一次：用独立的 JPEG 引擎解成 RGB565 写进 PSRAM，随后立刻
 * 释放引擎，不占用 wall_sd 的常驻引擎。像素缓冲常驻，UI 层的全屏
 * lv_image 只持有 lv_image_dsc_t 指针，不复制帧。
 *
 * 失败路径（无卡 / 无图 / 图太大 / 解码失败）一律 return：
 * background_dsc() 返回 NULL，界面回落纯色底。
 */

#include "background.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/jpeg_decode.h"

static const char *TAG = "background";

#define BG_PATH          "/sdcard/bg.jpg"
#define BG_W             1024
#define BG_H             600
#define BG_STRIDE        (BG_W * 2)
#define BG_DATA_SIZE     (BG_W * BG_H * 2)

/* JPEG 硬件按 MCU 16 行对齐：600 行会被补到 608 行输出，缓冲必须按
 * 608 行申请，否则 jpeg_decoder_process() 以「缓冲小于实际输出」拒收。
 * lv_image_dsc_t 仍然只描述可见的 600 行（同 wall_sd.c 的写法）。 */
#define BG_DEC_H         608
#define BG_DEC_SIZE      (BG_W * BG_DEC_H * 2)

#define BG_MAX_FILE_SIZE (1024 * 1024)   /* 1 MB，超过直接放弃 */

/* 常驻描述符：解码成功后只填 data，其余字段固定不变。 */
static lv_image_dsc_t s_dsc = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565,
    .header.flags = 0,
    .header.w = BG_W,
    .header.h = BG_H,
    .header.stride = BG_STRIDE,
    .data_size = BG_DATA_SIZE,
    .data = NULL,
};

const lv_image_dsc_t *background_dsc(void)
{
    return (s_dsc.data != NULL) ? &s_dsc : NULL;
}

void background_init(void)
{
    /* 幂等：已经解过就不再重复占 1.2 MB PSRAM 和一次硬件解码 */
    if (s_dsc.data != NULL) {
        return;
    }

    /* 只 stat，不 mount：SD 由 wall_sd 负责挂载 */
    struct stat st;
    if (stat(BG_PATH, &st) != 0) {
        ESP_LOGI(TAG, "%s not found - main background stays solid color", BG_PATH);
        return;
    }
    if (st.st_size <= 0 || (unsigned long)st.st_size > BG_MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "skip %s: bad size %ld (limit %d)",
                 BG_PATH, (long)st.st_size, BG_MAX_FILE_SIZE);
        return;
    }

    FILE *fp = fopen(BG_PATH, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "skip %s: fopen failed", BG_PATH);
        return;
    }

    const size_t file_size = (size_t)st.st_size;

    /* 输入缓冲没有对齐要求，照 wall_sd 用解码器自带的分配器（PSRAM） */
    jpeg_decode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t in_alloc = 0;
    uint8_t *in_buf = (uint8_t *)jpeg_alloc_decoder_mem(file_size, &in_cfg, &in_alloc);
    if (in_buf == NULL) {
        ESP_LOGE(TAG, "skip %s: alloc input buffer (%u bytes) failed",
                 BG_PATH, (unsigned)file_size);
        fclose(fp);
        return;
    }

    const size_t rd = fread(in_buf, 1, file_size, fp);
    fclose(fp);
    if (rd != file_size) {
        ESP_LOGW(TAG, "skip %s: fread %u of %u bytes",
                 BG_PATH, (unsigned)rd, (unsigned)file_size);
        free(in_buf);
        return;
    }

    jpeg_decode_picture_info_t info = {0};
    esp_err_t err = jpeg_decoder_get_info(in_buf, (uint32_t)file_size, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "skip %s: get_info failed: %s", BG_PATH, esp_err_to_name(err));
        free(in_buf);
        return;
    }
    if (info.width != BG_W || info.height != BG_H) {
        ESP_LOGW(TAG, "skip %s: %ux%u, expect %dx%d",
                 BG_PATH, (unsigned)info.width, (unsigned)info.height, BG_W, BG_H);
        free(in_buf);
        return;
    }

    /* 1,245,184 字节（608 行）。CACHE_ALIGNED 保证缓冲地址也按 64 B
     * cache line 对齐：jpeg_decoder_process() 会校验地址和长度，裸
     * MALLOC_CAP_SPIRAM 只保证 4 字节对齐，实机会被 ESP_ERR_INVALID_ARG
     * 拒收（wall_sd 用 jpeg_alloc_decoder_mem 做的也是同一件事）。 */
    uint8_t *out_buf = (uint8_t *)heap_caps_malloc(
        BG_DEC_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (out_buf == NULL) {
        ESP_LOGE(TAG, "skip %s: alloc output buffer (%d bytes) failed",
                 BG_PATH, BG_DEC_SIZE);
        free(in_buf);
        return;
    }

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };
    jpeg_decoder_handle_t engine = NULL;
    err = jpeg_new_decoder_engine(&eng_cfg, &engine);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
        heap_caps_free(out_buf);
        free(in_buf);
        return;
    }

    const jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   /* 与 wall_sd 一致 */
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t out_size = 0;
    const int64_t t0_us = esp_timer_get_time();
    err = jpeg_decoder_process(engine, &dec_cfg, in_buf, (uint32_t)file_size,
                               out_buf, BG_DEC_SIZE, &out_size);
    const int cost_ms = (int)((esp_timer_get_time() - t0_us) / 1000);

    /* 独立引擎用完即删；wall_sd 的引擎仍由它自己持有 */
    (void)jpeg_del_decoder_engine(engine);
    free(in_buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "decode %s failed: %s (%d ms)", BG_PATH, esp_err_to_name(err), cost_ms);
        heap_caps_free(out_buf);
        return;
    }

    s_dsc.data = out_buf;
    ESP_LOGI(TAG, "decoded %s (%u bytes) in %d ms, out %u bytes",
             BG_PATH, (unsigned)file_size, cost_ms, (unsigned)out_size);
}