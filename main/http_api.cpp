/*
 * http_api.cpp — REST API + OTA for Wired Detector
 *
 * Provides HTTP endpoints for device info, detection status, OTA firmware
 * update, NVS configuration, and reboot.  Runs on the same core as
 * EthMqttTask (Core 0) using esp_http_server's internal task.
 */

#include "http_api.h"
#include "sdkconfig.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

static const char *TAG = "http_api";

/* ================================================================
 * Shared detection status
 * ================================================================ */

static SemaphoreHandle_t s_status_mutex;
static HttpDroneStatus_t s_status;

void http_api_update_status(const HttpDroneStatus_t *status)
{
    if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50))) {
        s_status = *status;
        xSemaphoreGive(s_status_mutex);
    }
}

static void get_status_snapshot(HttpDroneStatus_t *out)
{
    if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50))) {
        *out = s_status;
        xSemaphoreGive(s_status_mutex);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

/* ================================================================
 * Auth token
 * ================================================================ */

static char s_auth_token[128];

static void load_auth_token(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_auth_token);
    if (nvs_open("wired_cfg", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "http_token", s_auth_token, &len) != ESP_OK) {
            strncpy(s_auth_token, CONFIG_BATEAR_HTTP_AUTH_TOKEN,
                    sizeof(s_auth_token) - 1);
        }
        nvs_close(h);
    } else {
        strncpy(s_auth_token, CONFIG_BATEAR_HTTP_AUTH_TOKEN,
                sizeof(s_auth_token) - 1);
    }
    s_auth_token[sizeof(s_auth_token) - 1] = '\0';
}

static bool check_auth(httpd_req_t *req)
{
    if (s_auth_token[0] == '\0') return true;

    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"missing Authorization header\"}");
        return false;
    }

    char expected[140];
    snprintf(expected, sizeof(expected), "Bearer %s", s_auth_token);
    if (strcmp(hdr, expected) != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"invalid token\"}");
        return false;
    }
    return true;
}

/* ================================================================
 * GET /api/info
 * ================================================================ */

static esp_err_t handle_info(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    int64_t uptime_s = esp_timer_get_time() / 1000000LL;

    char json[512];
    snprintf(json, sizeof(json),
        "{"
            "\"app_name\":\"%s\","
            "\"version\":\"%s\","
            "\"idf_version\":\"%s\","
            "\"compile_date\":\"%s %s\","
            "\"partition\":\"%s\","
            "\"free_heap\":%lu,"
            "\"uptime_s\":%lld"
        "}",
        app->project_name,
        app->version,
        app->idf_ver,
        app->date, app->time,
        running ? running->label : "unknown",
        (unsigned long)esp_get_free_heap_size(),
        (long long)uptime_s);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* ================================================================
 * GET /api/status
 * ================================================================ */

static esp_err_t handle_status(httpd_req_t *req)
{
    HttpDroneStatus_t st;
    get_status_snapshot(&st);

    char json[256];
    snprintf(json, sizeof(json),
        "{"
            "\"drone_detected\":%s,"
            "\"detector_id\":%u,"
            "\"rms_db\":%u,"
            "\"f0_bin\":%u,"
            "\"confidence\":%.4f,"
            "\"timestamp\":%lld"
        "}",
        st.drone_detected ? "true" : "false",
        (unsigned)st.detector_id,
        (unsigned)st.rms_db,
        (unsigned)st.f0_bin,
        (double)st.confidence,
        (long long)st.timestamp);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* ================================================================
 * POST /api/ota
 * ================================================================ */

static esp_err_t handle_ota(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"no OTA partition available\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA start → partition '%s' (%lu bytes incoming)",
             update->label, (unsigned long)req->content_len);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"error\":\"ota_begin: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len;
    int total = 0;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, sizeof(buf));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA recv error at offset %d", total);
            esp_ota_abort(ota_handle);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"error\":\"receive failed\"}");
            return ESP_OK;
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at offset %d: %s",
                     total, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_set_status(req, "500 Internal Server Error");
            char msg[128];
            snprintf(msg, sizeof(msg), "{\"error\":\"ota_write: %s\"}", esp_err_to_name(err));
            httpd_resp_sendstr(req, msg);
            return ESP_OK;
        }

        total += recv_len;
        remaining -= recv_len;

        if (total % (64 * 1024) < recv_len) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes", total, (int)req->content_len);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"error\":\"ota_end (image invalid?): %s\"}",
                 esp_err_to_name(err));
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"set_boot_partition failed\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA complete (%d bytes), rebooting in 1s", total);
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"bytes\":%d,\"message\":\"rebooting in 1s\"}", total);
    httpd_resp_sendstr(req, resp);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* ================================================================
 * POST /api/config
 * ================================================================ */

static esp_err_t handle_config(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    if (req->content_len == 0 || req->content_len > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"body empty or too large (max 1024)\"}");
        return ESP_OK;
    }

    char *body = (char *)malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
        return ESP_OK;
    }

    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(body);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"error\":\"receive failed\"}");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = '\0';

    /*
     * Minimal JSON parser for flat {"key":"value",...} objects.
     * Avoids pulling in a JSON library for this simple use case.
     */
    static const char *VALID_KEYS[] = {
        "mqtt_url", "mqtt_user", "mqtt_pass", "device_id",
        "eth_ip", "eth_gw", "eth_mask", "eth_dns", "http_token",
        NULL
    };

    nvs_handle_t h;
    esp_err_t err = nvs_open("wired_cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        free(body);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"NVS open failed\"}");
        return ESP_OK;
    }

    int count = 0;
    char *p = body;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        char *key_end = strchr(p, '"');
        if (!key_end) break;
        *key_end = '\0';
        char *key = p;
        p = key_end + 1;

        char *colon = strchr(p, ':');
        if (!colon) break;
        p = colon + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') break;
        p++;
        char *val_end = strchr(p, '"');
        if (!val_end) break;
        *val_end = '\0';
        char *val = p;
        p = val_end + 1;

        bool valid = false;
        for (int i = 0; VALID_KEYS[i]; i++) {
            if (strcmp(key, VALID_KEYS[i]) == 0) { valid = true; break; }
        }
        if (!valid) continue;

        nvs_set_str(h, key, val);
        count++;
        ESP_LOGI(TAG, "config: %s = \"%s\"", key, val);
    }

    nvs_commit(h);
    nvs_close(h);
    free(body);

    if (s_auth_token[0] != '\0') load_auth_token();

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"keys_written\":%d,\"note\":\"reboot to apply\"}", count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    return ESP_OK;
}

/* ================================================================
 * POST /api/reboot
 * ================================================================ */

static esp_err_t handle_reboot(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"rebooting in 1s\"}");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* ================================================================
 * Server start
 * ================================================================ */

void http_api_start(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));

    load_auth_token();
    if (s_auth_token[0]) {
        ESP_LOGI(TAG, "Bearer auth enabled for POST endpoints");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_BATEAR_HTTP_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    /* OTA uploads can be large; increase timeout */
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return;
    }

    static const httpd_uri_t uri_info = {
        .uri = "/api/info", .method = HTTP_GET,
        .handler = handle_info, .user_ctx = NULL
    };
    static const httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = handle_status, .user_ctx = NULL
    };
    static const httpd_uri_t uri_ota = {
        .uri = "/api/ota", .method = HTTP_POST,
        .handler = handle_ota, .user_ctx = NULL
    };
    static const httpd_uri_t uri_config = {
        .uri = "/api/config", .method = HTTP_POST,
        .handler = handle_config, .user_ctx = NULL
    };
    static const httpd_uri_t uri_reboot = {
        .uri = "/api/reboot", .method = HTTP_POST,
        .handler = handle_reboot, .user_ctx = NULL
    };

    httpd_register_uri_handler(server, &uri_info);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_ota);
    httpd_register_uri_handler(server, &uri_config);
    httpd_register_uri_handler(server, &uri_reboot);

    ESP_LOGI(TAG, "HTTP API listening on port %d", CONFIG_BATEAR_HTTP_PORT);
}
