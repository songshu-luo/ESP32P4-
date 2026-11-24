/*
 * 屏幕设置状态（亮度 / 息屏）
 *
 * 设计理由：亮度滑块和息屏选项都必须「立即生效」，但设置本身不该跟某个
 * LVGL 对象绑定。这里保存唯一的一份状态，设置页只读它、只通过它改硬件，
 * 将来增加网页配置或按键调节也复用同一套接口。
 *
 * 息屏用 3% 而不是 0%：全黑屏幕从远处看和死机没有区别，3% 仍能看见
 * 「轻触唤醒」的界面轮廓，也不会在夜里刺眼。
 *
 * 无操作时长直接使用 lv_display_get_inactive_time()，它由 LVGL 在每次
 * 触摸/按键输入时复位，不需要自己维护时间戳。tick 由 app_ui.c 的
 * status_poll_cb（500 ms）调用，所以「变暗」的判定精度是 500 ms。
 *
 * 唤醒判定不能用固定窗口（例如 inactive < 500）：OTA 下载或日志大量
 * 输出时 LVGL 定时器可能被拖慢，触摸到下一次 tick 的间隔会超过 500 ms，
 * inactive 早已越过阈值，屏幕就永远停在 3%。这里改为比较相邻两拍的
 * inactive 是否「回落」——只有新输入才会让它归零，因此不管 tick 间隔
 * 被拖到多长都能可靠唤醒，也不存在正好卡在 500 ms 边界漏判的问题。
 */

#include "app_settings.h"

#include "ui_screensaver.h"
#include "wall_sd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lvgl.h"

#include "bsp/esp-bsp.h"

#define BRIGHTNESS_MIN      (10)
#define BRIGHTNESS_MAX      (100)

static SemaphoreHandle_t s_lock;
static int               s_brightness = APP_SETTINGS_DEFAULT_BRIGHTNESS;
static uint32_t          s_dim_timeout_ms;
static bool              s_dimmed;
static uint32_t          s_last_inactive;   /* 上一拍的 inactive 值 */
static uint32_t          s_rotate_next_ms;  /* 下次轮播的时刻（lv_tick_get 的 ms） */

/* 字模收集提示 —— 不要删！
 *
 * 3% 息屏时屏幕上看不到这句提示，但用户需要知道「轻触唤醒」；
 * tools/gen_cjk_font.sh 只扫描字符串字面量，所以把这两个词留在这里，
 * 让子集字体里始终有这四个字，方便以后在息屏页或文档里复用。 */
static const char k_settings_glyph_hint[] __attribute__((unused)) = "轻触唤醒";

/* ------------------------------------------------------------ 内部工具 */

/* 抑制下一拍 tick 的「回落即唤醒」判定。
 *
 * tick 的唤醒判定是「inactive 比上一拍回落」——任何触摸都会让 LVGL 把
 * inactive 归零（lv_indev.c 里 state==PRESSED 就刷新 last_activity_time）。
 * app_settings_sleep_now() / app_settings_sleep_advance() 都由点击直接触发，
 * 那一刻 inactive 已被这次点击清零；不动的话下一拍 tick 会把这次点击当成
 * 「用户想唤醒」，刚息屏就亮屏 / 切完图就亮屏。
 *
 * 不采用「把基线对齐到当前 inactive」的比较法：切图走 PRESSED，松手时的
 * RELEASED 会再刷新一次 inactive，若 tick 恰落在松手后几毫秒内，仍会满足
 * inactive < 基线而被误判。这里改为让紧接着的一拍整拍跳过 activity 判定，
 * 只更新基线，之后一切照旧（真实触摸的回落照常唤醒）。 */
static bool s_skip_activity_once = false;

static void suppress_next_activity(void)
{
    s_skip_activity_once = true;
}

static int clamp_brightness(int percent)
{
    if (percent < BRIGHTNESS_MIN) {
        return BRIGHTNESS_MIN;
    }
    if (percent > BRIGHTNESS_MAX) {
        return BRIGHTNESS_MAX;
    }
    return percent;
}

/* ------------------------------------------------------------ 对外接口 */

esp_err_t app_settings_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_brightness      = APP_SETTINGS_DEFAULT_BRIGHTNESS;
    s_dim_timeout_ms  = 300000;  /* 默认 5 分钟（0 才是常亮） */
    s_dimmed          = false;
    s_last_inactive   = 0;
    xSemaphoreGive(s_lock);

    /* 这里不调用 bsp_display_brightness_set()：main.c 的既有顺序是
     * 「先建界面、再开背光」，提前点亮会先闪一帧未初始化的画面。 */
    return ESP_OK;
}

void app_settings_set_brightness(int percent)
{
    const int value = clamp_brightness(percent);

    /* 这个函数会被亮度滑块的 LVGL 回调直接调用，所以取锁超时为 0：
     * 拿不到锁就按无锁路径写（int 对齐写不会撕裂），绝不让 UI 线程等待。 */
    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, 0) == pdTRUE) {
        s_brightness = value;
        /* 用户主动调亮度就视为唤醒，避免滑块和息屏状态互相打架 */
        s_dimmed = false;
        xSemaphoreGive(s_lock);
    } else {
        s_brightness = value;
        s_dimmed = false;
    }

    /* 立即生效：滑块拖动过程中每一帧都写一次 LEDC，开销很小，
     * 但手感比「松手才变亮」好得多。 */
    bsp_display_brightness_set(value);
}

int app_settings_get_brightness(void)
{
    int value = APP_SETTINGS_DEFAULT_BRIGHTNESS;

    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, 0) == pdTRUE) {
        value = s_brightness;
        xSemaphoreGive(s_lock);
    } else {
        value = s_brightness;
    }
    return value;
}

void app_settings_set_dim_timeout(uint32_t ms)
{
    bool wake = false;
    int  restore = APP_SETTINGS_DEFAULT_BRIGHTNESS;

    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, 0) == pdTRUE) {
        s_dim_timeout_ms = ms;
        /* 选「常亮」时如果屏幕正暗着，应立刻恢复，而不是等下一次触摸 */
        if (ms == 0 && s_dimmed) {
            s_dimmed = false;
            wake = true;
            restore = s_brightness;
        }
        xSemaphoreGive(s_lock);
    } else {
        s_dim_timeout_ms = ms;
    }

    if (wake) {
        bsp_display_brightness_set(restore);
        ui_screensaver_hide();
        wall_sd_prime();
    }
}

uint32_t app_settings_get_dim_timeout(void)
{
    uint32_t ms = 0;

    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, 0) == pdTRUE) {
        ms = s_dim_timeout_ms;
        xSemaphoreGive(s_lock);
    } else {
        ms = s_dim_timeout_ms;
    }
    return ms;
}

bool app_settings_is_dimmed(void)
{
    bool dimmed = false;

    if (s_lock != NULL &&
        xSemaphoreTake(s_lock, 0) == pdTRUE) {
        dimmed = s_dimmed;
        xSemaphoreGive(s_lock);
    } else {
        dimmed = s_dimmed;
    }
    return dimmed;
}

void app_settings_tick(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        return;
    }

    /* LVGL 的 inactive 时间：最近一次输入之后经过的毫秒数 */
    const uint32_t inactive = lv_display_get_inactive_time(disp);

    bool    dim     = false;
    bool    wake    = false;
    bool    rotate  = false;
    int     restore = APP_SETTINGS_DEFAULT_BRIGHTNESS;
    uint32_t timeout = 0;

    if (s_lock == NULL ||
        xSemaphoreTake(s_lock, 0) != pdTRUE) {
        /* UI 回调里不等待：这一拍拿不到锁就跳过，下一拍再判断 */
        return;
    }

    /* 新输入会让 inactive 归零，所以「比上一拍更小」就说明有操作。
     * 这比固定窗口可靠：tick 被拖慢时窗口会漏判，回落检测不会。
     * 基线只在成功拿到锁后才更新：万一这一拍抢锁失败，下一拍仍能
     * 相对旧基线看出回落，不会把一次触摸的唤醒信息丢掉。
     * s_skip_activity_once 由本文件手动息屏/手动切图置位：那一拍只看基线、
     * 不判唤醒，避免把「点按钮」当成唤醒信号。 */
    const bool activity = !s_skip_activity_once && (inactive < s_last_inactive);
    s_skip_activity_once = false;
    s_last_inactive = inactive;

    timeout = s_dim_timeout_ms;
    if (!s_dimmed && timeout > 0 && inactive >= timeout) {
        s_dimmed = true;
        dim = true;
        /* 进入息屏：第一张从这一拍起算，至少完整显示一个轮播周期 */
        if (APP_SETTINGS_SLEEP_ROTATE_MS > 0) {
            s_rotate_next_ms = lv_tick_get() + APP_SETTINGS_SLEEP_ROTATE_MS;
        }
    } else if (s_dimmed && activity) {
        s_dimmed = false;
        wake = true;
        restore = s_brightness;
        /* 已不是息屏状态，轮播计时作废；下次进息屏会重新设 */
        s_rotate_next_ms = 0;
    }
    /* 锁内拷贝一份状态给锁外使用，避免无锁读 s_dimmed */
    const bool dimmed_now = s_dimmed;
    xSemaphoreGive(s_lock);

    /* 硬件操作放在锁外，缩短临界区；LEDC 写入本身不阻塞 */
    if (dim) {
        bsp_display_brightness_set(APP_SETTINGS_DIM_BRIGHTNESS);
        ui_screensaver_show();
        /* 已在 LVGL 锁内（tick 由 UI 定时器调用），这里不再加锁；
         * 有就绪的 SD 图就换源，没有就继续用内置图。 */
        const lv_image_dsc_t *wall = wall_sd_take_ready();
        if (wall) {
            ui_screensaver_set_src(wall);
        }
    } else if (wake) {
        bsp_display_brightness_set(restore);
        ui_screensaver_hide();
        wall_sd_prime();
    }

    /* 息屏中按间隔换下一张；唤醒 / 常亮状态什么都不做 */
    if (APP_SETTINGS_SLEEP_ROTATE_MS > 0 && dimmed_now && ui_screensaver_is_visible()) {
        const uint32_t now = lv_tick_get();
        /* 有符号比较，处理 uint32 回绕（49 天） */
        if ((int32_t)(now - s_rotate_next_ms) >= 0) {
            rotate = true;
            s_rotate_next_ms = now + APP_SETTINGS_SLEEP_ROTATE_MS;
        }
    }
    if (rotate) {
        const lv_image_dsc_t *wall = wall_sd_take_ready();
        if (wall != NULL) {
            /* 已在 LVGL 锁内；走交叉淡入。prime 不在此处调用，统一由淡入
             * 完成回调触发——淡入期间预解码会覆写仍在显示的旧缓冲而撕裂。 */
            ui_screensaver_transition_to(wall);
        } else {
            /* 没有就绪图：本轮不进入淡入，照旧预解码下一张，下个周期再试 */
            wall_sd_prime();
        }
    }
}

void app_settings_wake_now(void)
{
    bool was_dimmed = false;
    int  restore    = APP_SETTINGS_DEFAULT_BRIGHTNESS;

    if (s_lock != NULL && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        was_dimmed = s_dimmed;
        if (was_dimmed) {
            s_dimmed  = false;
            restore   = s_brightness;
        }
        xSemaphoreGive(s_lock);
    } else {
        /* 拿不到锁时按无锁路径处理：s_dimmed 是 bool，单字节写不会撕裂 */
        was_dimmed = s_dimmed;
        s_dimmed   = false;
        restore    = s_brightness;
    }

    /* 没在息屏：不动背光也不动壁纸，保持幂等 */
    if (!was_dimmed) {
        return;
    }

    /* 同步唤醒路径不走 tick，这里也要把轮播计时清掉，与 tick 的 wake 分支一致 */
    s_rotate_next_ms = 0;

    /* 先隐藏壁纸再抬背光：避免 30% 的图在亮起来的一瞬间闪一下 */
    ui_screensaver_hide();
    bsp_display_brightness_set(restore);
    wall_sd_prime();
}

bool app_settings_sleep_now(void)
{
    bool enter = false;

    if (s_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;                       /* 拿不到锁就放弃，由用户再点一次 */
    }
    if (!s_dimmed) {
        s_dimmed = true;
        enter = true;
        if (APP_SETTINGS_SLEEP_ROTATE_MS > 0) {
            /* 与自动进息屏一致：第一张至少完整显示一个周期 */
            s_rotate_next_ms = lv_tick_get() + APP_SETTINGS_SLEEP_ROTATE_MS;
        }
    }
    xSemaphoreGive(s_lock);

    if (!enter) {
        return false;                       /* 已在息屏：幂等，不动画面/背光 */
    }

    /* 锁外做硬件与 UI，与 tick 的 dim 分支完全一致 */
    bsp_display_brightness_set(APP_SETTINGS_DIM_BRIGHTNESS);
    ui_screensaver_show();
    const lv_image_dsc_t *wall = wall_sd_take_ready();
    if (wall != NULL) {
        ui_screensaver_set_src(wall);
    }

    /* 这次「点一下息屏」的触摸本身已把 inactive 归零：对齐基线，
     * 否则下一次 tick 会把它误判成用户想唤醒，息屏立刻被打断。 */
    suppress_next_activity();
    return true;
}

bool app_settings_sleep_advance(void)
{
    bool dimmed = false;

    if (s_lock != NULL && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        dimmed = s_dimmed;
        xSemaphoreGive(s_lock);
    } else {
        dimmed = s_dimmed;                  /* 无锁路径：bool 单字节读，与既有写法一致 */
    }

    if (!dimmed || !ui_screensaver_is_visible()) {
        return false;                       /* 不在休眠：按钮点击不该有副作用 */
    }

    /* 手动切图后重新计时，否则可能刚切完就撞上自动轮播到点 */
    if (APP_SETTINGS_SLEEP_ROTATE_MS > 0) {
        s_rotate_next_ms = lv_tick_get() + APP_SETTINGS_SLEEP_ROTATE_MS;
    }

    const lv_image_dsc_t *wall = wall_sd_take_ready();
    if (wall != NULL) {
        ui_screensaver_transition_to(wall); /* 走 300ms 交叉淡入，与自动轮播同一条路径 */
        /* 切图这次触摸同样重置了 inactive，必须对齐基线，
         * 否则下一拍 tick 会把它当成唤醒信号，点一下按钮就亮屏。 */
        suppress_next_activity();
        return true;
    }

    /* 还没解好：触发预解码，这次点击不出图，下一次再按就能切 */
    suppress_next_activity();
    wall_sd_prime();
    return false;
}
