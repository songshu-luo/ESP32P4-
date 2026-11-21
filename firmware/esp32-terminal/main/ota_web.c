/*
 * ESP32 OTA 网页（S6）
 *
 * 页面只用原生 HTML/CSS/JS，不依赖外部 CDN；局域网内访问。
 */

#include "ota_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "eth_link.h"
#include "ota_update.h"

static const char *TAG = "ota_web";

static httpd_handle_t s_server;
static bool s_started;

static const char INDEX_HTML[] =
"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 OTA</title>"
"<style>"
"body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;margin:0;padding:24px;background:#10141c;color:#e8edf5}"
"h1{font-size:22px;margin:0 0 18px}"
".card{background:#1b2230;border:1px solid #2b3547;border-radius:12px;padding:18px;max-width:640px}"
".row{display:flex;justify-content:space-between;gap:16px;padding:8px 0;border-bottom:1px solid #263041}"
".row:last-child{border-bottom:0}"
".label{color:#93a1b8}.value{font-weight:600;text-align:right;word-break:break-all}"
".bar{height:14px;background:#0b0f16;border-radius:7px;overflow:hidden;margin:16px 0}"
"#prog{height:100%;width:0%;background:linear-gradient(90deg,#2f81f7,#39d353);transition:width .3s}"
".msg{color:#b9c6d8;min-height:24px;margin:8px 0}"
"button{appearance:none;border:0;border-radius:9px;padding:11px 18px;margin:4px 8px 4px 0;"
"font-size:15px;font-weight:600;cursor:pointer;color:#fff;background:#2f81f7}"
"button.alt{background:#2b3547}button:disabled{opacity:.45;cursor:not-allowed}"
"pre{white-space:pre-wrap;color:#8fa3bd;font-size:13px;margin:12px 0 0}"
"</style></head><body><div class='card'>"
"<h1>ESP32 OTA 控制台</h1>"
"<div class='row'><span class='label'>当前版本</span><span class='value' id='cur'>-</span></div>"
"<div class='row'><span class='label'>可用版本</span><span class='value' id='avail'>-</span></div>"
"<div class='row'><span class='label'>状态</span><span class='value' id='state'>-</span></div>"
"<div class='bar'><div id='prog'></div></div>"
"<div class='msg' id='msg'>读取中...</div>"
"<div><button id='check' onclick='check()'>检查更新</button>"
"<button id='update' class='alt' onclick='update()'>开始升级</button></div>"
"<pre id='log'></pre>"
"</div><script>"
"const $=id=>document.getElementById(id);"
"let busy=false;"
"function pct(n){return (n||0)+'%'}"
"async function refresh(){"
" try{const r=await fetch('/api/status',{cache:'no-store'});const j=await r.json();"
" $('cur').textContent=j.current_version||'-';"
" $('avail').textContent=j.available_version||'-';"
" $('state').textContent=j.state||'-';"
" $('msg').textContent=j.message||'';"
" $('prog').style.width=pct(j.progress_percent);"
" busy=(j.state==='checking'||j.state==='downloading'||j.state==='verifying');"
" $('check').disabled=busy;$('update').disabled=busy;"
" $('log').textContent=j.last_error?('last_error: '+j.last_error):'';"
" }catch(e){$('msg').textContent='无法连接 ESP32: '+e}"
"}"
"async function post(path){const r=await fetch(path,{method:'POST'});const j=await r.json();"
" if(!j.ok){$('msg').textContent=j.message||'请求失败'}refresh()}"
"function check(){post('/api/check')}function update(){if(confirm('确认升级？升级期间不要断电。'))post('/api/update')}"
"setInterval(refresh,1000);refresh();"
"</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t status_handler(httpd_req_t *req)
{
    ota_status_t st;
    ota_update_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    cJSON_AddStringToObject(root, "state", ota_update_state_name(st.state));
    cJSON_AddStringToObject(root, "current_version", st.current_version);
    cJSON_AddStringToObject(root, "available_version", st.available_version);
    cJSON_AddStringToObject(root, "message", st.message);
    cJSON_AddStringToObject(root, "last_error", st.last_error);
    cJSON_AddNumberToObject(root, "progress_percent", st.progress_percent);
    cJSON_AddNumberToObject(root, "downloaded", (double)st.downloaded);
    cJSON_AddNumberToObject(root, "total", (double)st.total);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, strlen(json));
    free(json);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message != NULL ? message : "");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, strlen(json));
    free(json);
    return err;
}

static esp_err_t check_handler(httpd_req_t *req)
{
    esp_err_t err = ota_update_request_check();
    if (err == ESP_OK) {
        return send_result(req, true, "已开始检查版本");
    }
    return send_result(req, false, "当前正在检查或升级，请稍后");
}

static esp_err_t update_handler(httpd_req_t *req)
{
    esp_err_t err = ota_update_request_update();
    if (err == ESP_OK) {
        return send_result(req, true, "已开始升级，升级期间不要断电");
    }
    return send_result(req, false, "当前正在检查或升级，请稍后");
}

static void ota_web_task(void *arg)
{
    (void)arg;
    char ip[16] = {0};

    for (;;) {
        eth_link_status_t st;
        eth_link_get_status(&st);
        if (st.ip[0] != '\0') {
            snprintf(ip, sizeof(ip), "%s", st.ip);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 4096;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t check_uri = {
        .uri = "/api/check",
        .method = HTTP_POST,
        .handler = check_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t update_uri = {
        .uri = "/api/update",
        .method = HTTP_POST,
        .handler = update_handler,
        .user_ctx = NULL,
    };

    if (httpd_register_uri_handler(s_server, &index_uri) != ESP_OK ||
        httpd_register_uri_handler(s_server, &status_uri) != ESP_OK ||
        httpd_register_uri_handler(s_server, &check_uri) != ESP_OK ||
        httpd_register_uri_handler(s_server, &update_uri) != ESP_OK) {
        ESP_LOGE(TAG, "register uri failed");
        httpd_stop(s_server);
        s_server = NULL;
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA 页面就绪: http://%s/", ip);
    vTaskDelete(NULL);
}

esp_err_t ota_web_start_async(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_started = true;
    BaseType_t ok = xTaskCreate(ota_web_task, "ota_web", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
