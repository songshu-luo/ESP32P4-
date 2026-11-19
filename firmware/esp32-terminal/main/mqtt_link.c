/*
 * MQTT 链路（S2-a）
 *
 * 目标只有一个：让 ESP32 连上泰山派 Hub，并把「连到哪一步了」做成屏幕现象。
 * 本阶段不解析任何业务载荷，只关心 解析 → 连接 → 鉴权 → 订阅 是否成立。
 *
 * 与 eth_link.c 保持同一套设计约束（方案 §6.3「启动不得阻塞在网络」）：
 *   - mqtt_link_start_async() 只建任务就返回；
 *   - 解析主机名、TCP 连接、重连全部在后台任务里；
 *   - UI 线程只调 mqtt_link_get_status() 取互斥保护快照，绝不等待网络。
 *
 * 地址解析：泰山派没做 DHCP 保留，所以每次重连都重新解析，按顺序试：
 *   squirrel-lckfb.local（mDNS）→ squirrel-lckfb（路由器 DHCP 名）→ 写死 IP。
 * lwIP 的 CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES 对 ".local" 结尾的名字会发一次性
 * 组播查询，收到 A 记录就返回，因此不需要 espressif/mdns 组件。
 *
 * 重连由本模块自己管（关闭 esp-mqtt 的 auto reconnect）：只有这样才能在每次
 * 重连前重新解析，DHCP 换地址后不至于一直连旧 IP。
 */

#include "mqtt_link.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "eth_link.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_link";

/* ------------------------------------------------------------ 配置 */

/* S2-a 只读订阅：设备状态与指标都在这棵子树下（见 protocol/topics-v1.md） */
#define MQTT_SUB_TOPIC          "hub/v1/devices/#"
#define MQTT_SUB_QOS            (1)
#define MQTT_KEEPALIVE_S        (30)
#define MQTT_BUFFER_BYTES       (2048)

/* 后端任务参数：解析 + 建连的栈需求比单纯轮询大，给足余量 */
#define MQTT_TASK_STACK         (6144)
#define MQTT_TASK_PRIO          (5)

/* 重连退避：3s 起，翻倍，封顶 30s。断网期间不做无意义的高频重试。 */
#define BACKOFF_START_MS        (3000)
#define BACKOFF_MAX_MS          (30000)

/* 单次建连的上限：超过认为失败，避免握手半死卡住任务 */
#define CONNECT_TIMEOUT_MS      (60000)

/* 以太网就绪轮询 */
#define NET_WAIT_TICK_MS        (1000)

/* ------------------------------------------------------------ 状态存放 */

#define BIT_ONLINE  (1u << 0)   /* 订阅成功 */
#define BIT_FAILED  (1u << 1)   /* 出错或断开 */

static SemaphoreHandle_t   s_lock;
static mqtt_link_status_t  s_status;

static EventGroupHandle_t  s_events;
static esp_mqtt_client_handle_t s_client;
static int                 s_sub_msg_id;

/* 数据回调：由各解析模块（metrics / status）注册进来，在 esp-mqtt 任务里被调用。
 * 每个模块自己按主题过滤，这里只负责按顺序转发。 */
#define MQTT_MAX_DATA_CBS   (4)
static mqtt_link_data_cb_t s_data_cbs[MQTT_MAX_DATA_CBS];
static int                 s_data_cb_count;

void mqtt_link_add_data_cb(mqtt_link_data_cb_t cb)
{
    if (cb == NULL || s_data_cb_count >= MQTT_MAX_DATA_CBS) {
        return;
    }
    s_data_cbs[s_data_cb_count++] = cb;
}

void mqtt_link_get_status(mqtt_link_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    *out = s_status;
    xSemaphoreGive(s_lock);
}

/** 带锁更新：所有写操作都走这里，避免 UI 读到半截字符串 */
static void status_update(void (*fn)(mqtt_link_status_t *st, const void *arg), const void *arg)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    fn(&s_status, arg);
    xSemaphoreGive(s_lock);
}

/* ---------- 状态写入器 ---------- */

static void write_state(mqtt_link_status_t *st, const void *arg)
{
    st->state = *(const mqtt_link_state_t *)arg;
}

static void set_state(mqtt_link_state_t state)
{
    status_update(write_state, &state);
}

typedef struct {
    mqtt_link_state_t state;
    const char *host;
    const char *ip;
    const char *detail;
} attempt_arg_t;

/** 一次写入状态 + 主机名 + IP + 说明。
 *
 * host/ip/detail 传 NULL 表示「保持原值」：断线时不该把已经解析出来的地址擦掉，
 * 否则屏幕上就看不到「连的是谁」了。
 */
static void write_attempt(mqtt_link_status_t *st, const void *arg)
{
    const attempt_arg_t *a = (const attempt_arg_t *)arg;
    st->state = a->state;
    if (a->host   != NULL) { snprintf(st->host,   sizeof(st->host),   "%s", a->host); }
    if (a->ip     != NULL) { snprintf(st->ip,     sizeof(st->ip),     "%s", a->ip); }
    if (a->detail != NULL) { snprintf(st->detail, sizeof(st->detail), "%s", a->detail); }
}

static void set_attempt(mqtt_link_state_t state, const char *host,
                        const char *ip, const char *detail)
{
    attempt_arg_t a = { .state = state, .host = host, .ip = ip, .detail = detail };
    status_update(write_attempt, &a);
}

/* ------------------------------------------------------------ 主机名解析 */

/** 依次尝试候选主机名，第一个解析成功即返回。
 *
 * 这里刻意用阻塞式 getaddrinfo()：它在 lwIP 的 tcpip 线程里跑，本任务只等着，
 * 不会拖慢 UI；而 esp-mqtt 内部解析失败时只给一个笼统的错误码，分不清是
 * "名字查不到" 还是 "TCP 被拒"，屏幕上的诊断就没法做细。
 */
static bool resolve_broker(char *ip_out, size_t ip_len, const char **used_host)
{
    static const char *candidates[] = {
        CONFIG_TERMINAL_MQTT_BROKER_HOST,           /* mDNS 名 */
        CONFIG_TERMINAL_MQTT_HOST_FALLBACK_NAME,    /* 路由器 DHCP 名 */
        CONFIG_TERMINAL_MQTT_HOST_FALLBACK_IP,      /* 写死 IP 兜底 */
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *host = candidates[i];
        if (host == NULL || host[0] == '\0') {
            continue;
        }

        struct addrinfo hints = {0};
        struct addrinfo *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        int rc = getaddrinfo(host, NULL, &hints, &res);
        if (rc != 0 || res == NULL) {
            ESP_LOGW(TAG, "resolve %s failed (rc=%d)", host, rc);
            if (res != NULL) {
                freeaddrinfo(res);
            }
            continue;
        }

        const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
        const char *ip = inet_ntoa(sa->sin_addr);
        snprintf(ip_out, ip_len, "%s", ip);
        freeaddrinfo(res);

        ESP_LOGI(TAG, "resolved %s -> %s", host, ip_out);
        *used_host = host;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ MQTT 事件 */

/** topic 是否以 suffix 结尾（topic 没有 NUL，长度单独给出） */
static bool topic_ends_with(const char *topic, int topic_len, const char *suffix)
{
    const int n = (int)strlen(suffix);
    if (topic == NULL || topic_len < n) {
        return false;
    }
    return memcmp(topic + topic_len - n, suffix, (size_t)n) == 0;
}

/** 把 CONNACK 的拒绝原因翻成一句人话 */
static void handle_connack_refused(const esp_mqtt_error_codes_t *err)
{
    switch (err->connect_return_code) {
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
        ESP_LOGE(TAG, "broker refused: bad username");
        set_attempt(MQTT_LINK_AUTH_FAIL, NULL, NULL, "username rejected");
        break;
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
        ESP_LOGE(TAG, "broker refused: not authorized");
        set_attempt(MQTT_LINK_AUTH_FAIL, NULL, NULL, "not authorized");
        break;
    case MQTT_CONNECTION_REFUSE_PROTOCOL:
        set_attempt(MQTT_LINK_ERROR, NULL, NULL, "protocol rejected");
        break;
    case MQTT_CONNECTION_REFUSE_ID_REJECTED:
        set_attempt(MQTT_LINK_ERROR, NULL, NULL, "client id rejected");
        break;
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
        set_attempt(MQTT_LINK_ERROR, NULL, NULL, "server unavailable");
        break;
    default:
        set_attempt(MQTT_LINK_ERROR, NULL, NULL, "connection refused");
        break;
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected, protocol v%d", (int)event->protocol_ver);
        set_state(MQTT_LINK_SUBSCRIBING);
        s_sub_msg_id = esp_mqtt_client_subscribe(event->client, MQTT_SUB_TOPIC, MQTT_SUB_QOS);
        if (s_sub_msg_id < 0) {
            ESP_LOGE(TAG, "subscribe() failed");
            set_attempt(MQTT_LINK_ERROR, NULL, NULL, "subscribe failed");
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
        break;
    }

    case MQTT_EVENT_SUBSCRIBED:
        if (event->msg_id == s_sub_msg_id) {
            ESP_LOGI(TAG, "subscribed to %s", MQTT_SUB_TOPIC);
            set_state(MQTT_LINK_ONLINE);
            xEventGroupSetBits(s_events, BIT_ONLINE);
        }
        break;

    case MQTT_EVENT_DATA: {
        /* 计数 + 转发给解析层。这里仍不碰 LVGL：esp-mqtt 任务不是 UI 线程。 */
        if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_status.msg_count++;
            /* 按主题后缀分类计数：只看总数分不清是 metrics 停了还是 status 停了 */
            if (topic_ends_with(event->topic, event->topic_len, "/metrics")) {
                s_status.metrics_count++;
            } else if (topic_ends_with(event->topic, event->topic_len, "/status")) {
                s_status.status_count++;
            }
            xSemaphoreGive(s_lock);
        }
        for (int i = 0; i < s_data_cb_count; i++) {
            s_data_cbs[i](event->topic, event->topic_len, event->data, event->data_len);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        /* 只有已经上线过才算「掉线」；握手阶段断开由 ERROR 分支给出更具体的原因 */
        if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool was_online = (s_status.state == MQTT_LINK_ONLINE);
            xSemaphoreGive(s_lock);
            if (was_online) {
                set_attempt(MQTT_LINK_DISCONNECTED, NULL, NULL, "link lost, retrying");
            }
        }
        xEventGroupSetBits(s_events, BIT_FAILED);
        break;

    case MQTT_EVENT_ERROR: {
        const esp_mqtt_error_codes_t *err = event->error_handle;
        if (err == NULL) {
            set_attempt(MQTT_LINK_ERROR, NULL, NULL, "unknown error");
            break;
        }
        switch (err->error_type) {
        case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
            handle_connack_refused(err);
            break;
        case MQTT_ERROR_TYPE_TCP_TRANSPORT:
            ESP_LOGE(TAG, "transport error: esp_tls=%s sock_errno=%d",
                     esp_err_to_name(err->esp_tls_last_esp_err), err->esp_transport_sock_errno);
            set_attempt(MQTT_LINK_ERROR, NULL, NULL, "tcp connect failed");
            break;
        default:
            set_attempt(MQTT_LINK_ERROR, NULL, NULL, "mqtt error");
            break;
        }
        xEventGroupSetBits(s_events, BIT_FAILED);
        break;
    }

    case MQTT_EVENT_BEFORE_CONNECT:
    default:
        break;
    }
}

/* ------------------------------------------------------------ 单次连接 */

/** 建一个客户端并等它上线；返回是否成功上线 */
static bool connect_once(const char *host, const char *ip)
{
    const esp_mqtt_client_config_t cfg = {
        .broker.address.hostname  = ip,          /* 已解析成点分 IP，省掉二次 DNS */
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .broker.address.port      = CONFIG_TERMINAL_MQTT_PORT,
        .credentials.username     = CONFIG_TERMINAL_MQTT_USERNAME,
        .credentials.client_id    = CONFIG_TERMINAL_MQTT_CLIENT_ID,
        .credentials.authentication.password = CONFIG_TERMINAL_MQTT_PASSWORD,
        .session.protocol_ver     = MQTT_PROTOCOL_V_5,
        .session.keepalive        = MQTT_KEEPALIVE_S,
        .session.disable_clean_session = false,
        .network.disable_auto_reconnect = true,  /* 重连由本模块负责，好重新解析 */
        .buffer.size              = MQTT_BUFFER_BYTES,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        set_attempt(MQTT_LINK_ERROR, host, ip, "client init failed");
        return false;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    xEventGroupClearBits(s_events, BIT_ONLINE | BIT_FAILED);

    set_attempt(MQTT_LINK_CONNECTING, host, ip, "tcp + mqtt handshake");
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start: %s", esp_err_to_name(err));
        set_attempt(MQTT_LINK_ERROR, host, ip, "client start failed");
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_ONLINE | BIT_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if ((bits & BIT_ONLINE) == 0) {
        if ((bits & BIT_FAILED) == 0) {
            ESP_LOGE(TAG, "connect timeout (%d ms)", CONNECT_TIMEOUT_MS);
            set_attempt(MQTT_LINK_ERROR, host, ip, "connect timeout");
        }
        return false;
    }

    ESP_LOGI(TAG, "online via %s (%s)", host, ip);

    /* 在线期间就停在这里等断开；esp-mqtt 自己的任务在处理心跳和收包 */
    xEventGroupWaitBits(s_events, BIT_FAILED, pdTRUE, pdFALSE, portMAX_DELAY);
    return true;
}

static void client_teardown(void)
{
    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
}

/* ------------------------------------------------------------ 后台任务 */

static bool eth_has_ip(eth_link_status_t *out)
{
    eth_link_get_status(out);
    return out->ip[0] != '\0';
}

static void mqtt_task(void *arg)
{
    (void)arg;

    uint32_t backoff = BACKOFF_START_MS;

    for (;;) {
        eth_link_status_t net;
        if (!eth_has_ip(&net)) {
            set_attempt(MQTT_LINK_WAIT_IP, NULL, NULL, "waiting for ethernet ip");
            vTaskDelay(pdMS_TO_TICKS(NET_WAIT_TICK_MS));
            continue;
        }

        set_attempt(MQTT_LINK_RESOLVING, NULL, NULL, "resolving broker");

        char ip[16] = {0};
        const char *host = NULL;
        if (!resolve_broker(ip, sizeof(ip), &host)) {
            set_attempt(MQTT_LINK_RESOLVE_FAIL, NULL, NULL, "broker name not found");
            ESP_LOGW(TAG, "no candidate host resolved, retry in %u ms", (unsigned)backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
            continue;
        }

        bool was_online = connect_once(host, ip);
        client_teardown();

        if (was_online) {
            backoff = BACKOFF_START_MS;   /* 正常掉线，按最短间隔重连 */
        } else {
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
        }

        ESP_LOGI(TAG, "reconnect in %u ms", (unsigned)backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }
}

esp_err_t mqtt_link_start_async(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = MQTT_LINK_IDLE;

    BaseType_t ok = xTaskCreate(mqtt_task, "mqtt_link", MQTT_TASK_STACK, NULL,
                                MQTT_TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
