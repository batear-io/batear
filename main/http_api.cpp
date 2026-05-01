/*
 * http_api.cpp — REST API + OTA for Wired Detector
 *
 * Provides HTTP endpoints for device info, detection status, OTA firmware
 * update, NVS configuration, and reboot.  Runs on the same core as
 * EthMqttTask (Core 0) using esp_http_server's internal task.
 */

#include "http_api.h"
#include "sdkconfig.h"

#if CONFIG_BATEAR_ROLE_WIRED_DETECTOR
#include "tf_recorder.h"
#endif

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
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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
        *out = HttpDroneStatus_t{};
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

    char *body = static_cast<char *>(malloc(req->content_len + 1));
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
        "ntp_server",
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
 * GET /api/recordings — list WAV files
 * GET /api/recordings/<file> — stream one WAV
 * DELETE /api/recordings/<file> — unlink
 * GET /api/recordings/storage — used/free MiB + recording state
 *
 * All gated on CONFIG_BATEAR_ROLE_WIRED_DETECTOR so the symbols never link
 * in builds without the recorder.
 * ================================================================ */

#if CONFIG_BATEAR_ROLE_WIRED_DETECTOR

/* Pull the trailing path segment from a URI of the form "/api/recordings/<X>".
 * Returns NULL if the URI is exactly "/api/recordings" or "/api/recordings/". */
static const char *uri_tail(const httpd_req_t *req)
{
    static const char prefix[] = "/api/recordings";
    const size_t plen = sizeof(prefix) - 1;
    if (strncmp(req->uri, prefix, plen) != 0) return NULL;
    const char *p = req->uri + plen;
    if (*p == '\0') return NULL;
    if (*p != '/') return NULL;
    p++;
    if (*p == '\0') return NULL;
    return p;
}

static esp_err_t handle_recordings_list(httpd_req_t *req)
{
    if (!tf_recorder_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"SD not mounted\"}");
        return ESP_OK;
    }
    const char *dir = tf_recorder_dir();
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"opendir failed\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);

    bool first = true;
    char entry[256];
    char full[160];
    const struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_type != DT_REG) continue;
        size_t nlen = strlen(de->d_name);
        if (nlen < 5 || strcmp(&de->d_name[nlen - 4], ".wav") != 0) continue;
        if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= (int)sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;

        int n = snprintf(entry, sizeof(entry),
            "%s{\"name\":\"%s\",\"size\":%llu,\"mtime\":%lld}",
            first ? "" : ",",
            de->d_name,
            (unsigned long long)st.st_size,
            (long long)st.st_mtime);
        if (n > 0 && (size_t)n < sizeof(entry)) {
            httpd_resp_send_chunk(req, entry, n);
            first = false;
        }
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_recordings_storage(httpd_req_t *req)
{
    TfRecorderStats st;
    tf_recorder_get_stats(&st);
    char json[256];
    snprintf(json, sizeof(json),
        "{"
            "\"mounted\":%s,"
            "\"recording\":%s,"
            "\"used_mb\":%u,"
            "\"free_mb\":%u,"
            "\"total_mb\":%u,"
            "\"files\":%u,"
            "\"drops\":%u,"
            "\"last_recording\":\"%s\""
        "}",
        st.mounted ? "true" : "false",
        st.recording ? "true" : "false",
        (unsigned)st.used_mb, (unsigned)st.free_mb, (unsigned)st.total_mb,
        (unsigned)st.files, (unsigned)st.drops, st.last_file);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_recording_get(httpd_req_t *req)
{
    if (!tf_recorder_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"SD not mounted\"}");
        return ESP_OK;
    }
    const char *name = uri_tail(req);
    if (!name) return handle_recordings_list(req);
    if (strcmp(name, "storage") == 0) return handle_recordings_storage(req);

    char path[160];
    if (!tf_recorder_resolve_path(name, path, sizeof(path))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid filename\"}");
        return ESP_OK;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"file not found\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "audio/wav");
    char dispo[128];
    snprintf(dispo, sizeof(dispo), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", dispo);

    char *buf = static_cast<char *>(malloc(4096));
    if (!buf) {
        fclose(fp);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
        return ESP_OK;
    }
    for (;;) {
        size_t n = fread(buf, 1, 4096, fp);
        if (n == 0) break;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            ESP_LOGW(TAG, "client disconnected mid-stream: %s", name);
            break;
        }
    }
    free(buf);
    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_recording_delete(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!tf_recorder_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"SD not mounted\"}");
        return ESP_OK;
    }
    const char *name = uri_tail(req);
    if (!name) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"missing filename\"}");
        return ESP_OK;
    }
    char path[160];
    if (!tf_recorder_resolve_path(name, path, sizeof(path))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid filename\"}");
        return ESP_OK;
    }
    if (unlink(path) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"unlink failed\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"deleted\":\"%s\"}", name);
    return httpd_resp_sendstr(req, resp);
}

#endif /* CONFIG_BATEAR_ROLE_WIRED_DETECTOR */

/* ================================================================
 * Server start
 * ================================================================ */

void http_api_start(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    s_status = HttpDroneStatus_t{};

    load_auth_token();
    if (s_auth_token[0]) {
        ESP_LOGI(TAG, "Bearer auth enabled for POST endpoints");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_BATEAR_HTTP_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12;
    /* OTA uploads can be large; increase timeout */
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
#if CONFIG_BATEAR_ROLE_WIRED_DETECTOR
    /* /api/recordings/<file> needs wildcard match for the trailing path. */
    config.uri_match_fn = httpd_uri_match_wildcard;
#endif

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

#if CONFIG_BATEAR_ROLE_WIRED_DETECTOR
    /* The order matters with wildcard matching: register the exact paths
     * first so /api/recordings (no slash) hits the list handler directly
     * and the wildcard catches /api/recordings/<file>. */
    static const httpd_uri_t uri_rec_list = {
        .uri = "/api/recordings", .method = HTTP_GET,
        .handler = handle_recordings_list, .user_ctx = NULL
    };
    static const httpd_uri_t uri_rec_get = {
        .uri = "/api/recordings/*", .method = HTTP_GET,
        .handler = handle_recording_get, .user_ctx = NULL
    };
    static const httpd_uri_t uri_rec_del = {
        .uri = "/api/recordings/*", .method = HTTP_DELETE,
        .handler = handle_recording_delete, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_rec_list);
    httpd_register_uri_handler(server, &uri_rec_get);
    httpd_register_uri_handler(server, &uri_rec_del);
#endif

    ESP_LOGI(TAG, "HTTP API listening on port %d", CONFIG_BATEAR_HTTP_PORT);
}
