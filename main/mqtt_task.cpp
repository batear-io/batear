/*
 * mqtt_task.cpp — WiFi + MQTT + HA Discovery for Batear Gateway
 *
 * Credentials are read from NVS namespace "gateway_cfg" first; if a key
 * is absent the compile-time Kconfig default is used instead.
 *
 * Runs on Core 1.  node_rx (LoRaTask, Core 0) sends MqttEvent_t items via
 * g_mqtt_event_queue; this task publishes them as JSON.
 *
 * Several bridges may hear the same detector, so per-detector topics and
 * HA discovery are bridge-independent and byte-identical across bridges:
 *   batear/det/<XX>/state   retained, ALARM/CLEAR only  → drone binary_sensor
 *   batear/det/<XX>/status  retained, every packet      → telemetry sensors
 * Each bridge subscribes to batear/det/+/state and never publishes a state
 * older (by boot_id, seq) than the retained one, so a bridge that hears an
 * event late can't overwrite a newer one.
 *
 * Per-bridge topics stay per-bridge:
 *   batear/nodes/<bridge>/status, batear/nodes/<bridge>/availability (LWT)
 */

#include "mqtt_task.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "mesh.h"

#include <cstring>
#include <cstdio>

static const char *TAG = "mqtt";

/* Max string length for NVS-backed config values */
#define CFG_STR_MAX  128
#define DEVID_MAX     32

/* ---- runtime config (populated from NVS → Kconfig fallback) ---- */
static char s_wifi_ssid[CFG_STR_MAX];
static char s_wifi_pass[CFG_STR_MAX];
static char s_mqtt_url[CFG_STR_MAX];
static char s_mqtt_user[CFG_STR_MAX];
static char s_mqtt_pass[CFG_STR_MAX];
static char s_device_id[DEVID_MAX];

/* ---- WiFi event synchronisation ---- */
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      10
static int s_retry_count;

/* ---- MQTT state ---- */
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_connected;

/* ---- pre-built topic strings ---- */
static char s_topic_avail[64];
static char s_topic_status[80];

/* Lazy per-detector HA discovery: one bit per device_id; we only publish the
 * MQTT discovery config the first time we see each detector. */
static bool s_det_discovered[256];

/* Newest (boot_id, seq) per detector on batear/det/<XX>/state, from the
 * broker (any bridge) or our own publishes. Written by the MQTT client task,
 * read by MqttTask. */
typedef struct {
    bool     seen;
    uint16_t boot_id;
    uint16_t seq;
} det_state_t;
static det_state_t  s_det_state[256];
static portMUX_TYPE s_det_state_mux = portMUX_INITIALIZER_UNLOCKED;

#define DET_EXPIRE_S  (CONFIG_BATEAR_DET_OFFLINE_MIN * 60)

/* ================================================================
 * NVS config loader
 * ================================================================ */

static void load_nvs_str(nvs_handle_t h, const char *key,
                          char *dst, size_t dst_sz, const char *fallback)
{
    size_t len = dst_sz;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strncpy(dst, fallback, dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    }
}

static void load_config(void)
{
    nvs_handle_t h;
    bool opened = (nvs_open("gateway_cfg", NVS_READONLY, &h) == ESP_OK);

    if (opened) {
        load_nvs_str(h, "wifi_ssid", s_wifi_ssid, sizeof(s_wifi_ssid),
                     CONFIG_BATEAR_WIFI_SSID);
        load_nvs_str(h, "wifi_pass", s_wifi_pass, sizeof(s_wifi_pass),
                     CONFIG_BATEAR_WIFI_PASS);
        load_nvs_str(h, "mqtt_url",  s_mqtt_url,  sizeof(s_mqtt_url),
                     CONFIG_BATEAR_MQTT_BROKER_URL);
        load_nvs_str(h, "mqtt_user", s_mqtt_user, sizeof(s_mqtt_user),
                     CONFIG_BATEAR_MQTT_USER);
        load_nvs_str(h, "mqtt_pass", s_mqtt_pass, sizeof(s_mqtt_pass),
                     CONFIG_BATEAR_MQTT_PASS);
        load_nvs_str(h, "device_id", s_device_id, DEVID_MAX,
                     CONFIG_BATEAR_GW_DEVICE_ID);
        nvs_close(h);
    } else {
        strncpy(s_wifi_ssid, CONFIG_BATEAR_WIFI_SSID, sizeof(s_wifi_ssid) - 1);
        strncpy(s_wifi_pass, CONFIG_BATEAR_WIFI_PASS, sizeof(s_wifi_pass) - 1);
        strncpy(s_mqtt_url,  CONFIG_BATEAR_MQTT_BROKER_URL, sizeof(s_mqtt_url) - 1);
        strncpy(s_mqtt_user, CONFIG_BATEAR_MQTT_USER, sizeof(s_mqtt_user) - 1);
        strncpy(s_mqtt_pass, CONFIG_BATEAR_MQTT_PASS, sizeof(s_mqtt_pass) - 1);
        strncpy(s_device_id, CONFIG_BATEAR_GW_DEVICE_ID, sizeof(s_device_id) - 1);
        ESP_LOGW(TAG, "NVS namespace 'gateway_cfg' not found — using Kconfig defaults");
    }

    snprintf(s_topic_avail, sizeof(s_topic_avail),
             "batear/nodes/%s/availability", s_device_id);
    snprintf(s_topic_status, sizeof(s_topic_status),
             "batear/nodes/%s/status", s_device_id);

    ESP_LOGI(TAG, "cfg: wifi_ssid=%s mqtt_url=%s device_id=%s",
             s_wifi_ssid, s_mqtt_url, s_device_id);
}

/* ================================================================
 * WiFi STA
 * ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h_ip));

    wifi_config_t sta_cfg = {};
    strncpy(reinterpret_cast<char *>(sta_cfg.sta.ssid),
            s_wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(sta_cfg.sta.password),
            s_wifi_pass, sizeof(sta_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected to %s", s_wifi_ssid);
        return true;
    }
    ESP_LOGE(TAG, "WiFi FAILED after %d retries", WIFI_MAX_RETRY);
    return false;
}

/* ================================================================
 * HA MQTT Discovery
 * ================================================================ */

static void publish_ha_discovery(void)
{
    char topic[128];
    char payload[768];

    /* Binary sensor — drone detection */
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/batear_%s/drone/config", s_device_id);

    snprintf(payload, sizeof(payload),
        "{"
            "\"name\":\"Batear %s Drone Detected\","
            "\"unique_id\":\"batear_%s_drone\","
            "\"device_class\":\"safety\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"{{ 'ON' if value_json.drone_detected else 'OFF' }}\","
            "\"availability_topic\":\"%s\","
            "\"payload_available\":\"online\","
            "\"payload_not_available\":\"offline\","
            "\"json_attributes_topic\":\"%s\","
            "\"device\":{"
                "\"identifiers\":[\"batear_%s\"],"
                "\"name\":\"Batear Gateway %s\","
                "\"manufacturer\":\"Batear\","
                "\"model\":\"ESP32-S3 LoRa Gateway\""
            "}"
        "}",
        s_device_id, s_device_id,
        s_topic_status, s_topic_avail, s_topic_status,
        s_device_id, s_device_id);

    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "HA discovery published → %s", topic);

    /* RSSI sensor */
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/batear_%s/rssi/config", s_device_id);

    snprintf(payload, sizeof(payload),
        "{"
            "\"name\":\"Batear %s RSSI\","
            "\"unique_id\":\"batear_%s_rssi\","
            "\"device_class\":\"signal_strength\","
            "\"unit_of_measurement\":\"dBm\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"{{ value_json.rssi }}\","
            "\"availability_topic\":\"%s\","
            "\"payload_available\":\"online\","
            "\"payload_not_available\":\"offline\","
            "\"entity_category\":\"diagnostic\","
            "\"device\":{"
                "\"identifiers\":[\"batear_%s\"]"
            "}"
        "}",
        s_device_id, s_device_id,
        s_topic_status, s_topic_avail,
        s_device_id);

    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);

    /* SNR sensor */
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/batear_%s/snr/config", s_device_id);

    snprintf(payload, sizeof(payload),
        "{"
            "\"name\":\"Batear %s SNR\","
            "\"unique_id\":\"batear_%s_snr\","
            "\"unit_of_measurement\":\"dB\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"{{ value_json.snr }}\","
            "\"availability_topic\":\"%s\","
            "\"payload_available\":\"online\","
            "\"payload_not_available\":\"offline\","
            "\"entity_category\":\"diagnostic\","
            "\"device\":{"
                "\"identifiers\":[\"batear_%s\"]"
            "}"
        "}",
        s_device_id, s_device_id,
        s_topic_status, s_topic_avail,
        s_device_id);

    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);
}

/* ================================================================
 * Per-detector HA Discovery (lazy; fired on first packet from each ID)
 *
 * Payloads must not depend on which bridge publishes them: no bridge ID,
 * no via_device, no bridge availability topic.
 * ================================================================ */

static void publish_detector_discovery(uint8_t device_id)
{
    char topic[160];
    char payload[1024];
    char state_topic[48];
    char status_topic[48];
    snprintf(state_topic,  sizeof(state_topic),  "batear/det/%02X/state",  device_id);
    snprintf(status_topic, sizeof(status_topic), "batear/det/%02X/status", device_id);

    char device_block[192];
    snprintf(device_block, sizeof(device_block),
        "\"device\":{"
            "\"identifiers\":[\"batear_det_%02X\"],"
            "\"name\":\"Batear Detector %02X\","
            "\"manufacturer\":\"Batear\","
            "\"model\":\"ESP32-S3 LoRa Detector\""
        "}",
        device_id, device_id);

    /* Drone state: only ALARM/CLEAR update it, so it never expires. */
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/batear_det_%02X/drone/config", device_id);
    snprintf(payload, sizeof(payload),
        "{"
            "\"name\":\"Drone Detected\","
            "\"unique_id\":\"batear_det_%02X_drone\","
            "\"device_class\":\"safety\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"{{ value_json.state }}\","
            "\"json_attributes_topic\":\"%s\","
            "%s"
        "}",
        device_id, state_topic, status_topic, device_block);
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);

    /* Online: ON on every packet, unavailable after DET_EXPIRE_S of silence. */
    snprintf(topic, sizeof(topic),
             "homeassistant/binary_sensor/batear_det_%02X/online/config", device_id);
    snprintf(payload, sizeof(payload),
        "{"
            "\"name\":\"Online\","
            "\"unique_id\":\"batear_det_%02X_online\","
            "\"device_class\":\"connectivity\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"ON\","
            "\"expire_after\":%d,"
            "\"entity_category\":\"diagnostic\","
            "%s"
        "}",
        device_id, status_topic, DET_EXPIRE_S, device_block);
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);

    /* Helper macro keeps the diagnostic sensor entries compact. */
    #define PUB_SENSOR(KEY, NAME, EXTRA) do {                                  \
        snprintf(topic, sizeof(topic),                                         \
                 "homeassistant/sensor/batear_det_%02X/" KEY "/config",        \
                 device_id);                                                   \
        snprintf(payload, sizeof(payload),                                     \
            "{"                                                                \
                "\"name\":\"" NAME "\","                                       \
                "\"unique_id\":\"batear_det_%02X_" KEY "\","                   \
                "\"state_topic\":\"%s\","                                      \
                "\"value_template\":\"{{ value_json." KEY " }}\","             \
                "\"expire_after\":%d,"                                         \
                "\"entity_category\":\"diagnostic\","                          \
                EXTRA                                                          \
                "%s"                                                           \
            "}",                                                               \
            device_id, status_topic, DET_EXPIRE_S, device_block);              \
        esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);              \
    } while (0)

    PUB_SENSOR("battery_v", "Battery",
        "\"device_class\":\"voltage\","
        "\"unit_of_measurement\":\"V\","
        "\"suggested_display_precision\":2,"
        "\"state_class\":\"measurement\",");

    PUB_SENSOR("fw_version", "Firmware Version", "");

    PUB_SENSOR("uptime_min", "Uptime",
        "\"device_class\":\"duration\","
        "\"unit_of_measurement\":\"min\","
        "\"state_class\":\"measurement\",");

    PUB_SENSOR("free_heap_kb", "Free Heap",
        "\"unit_of_measurement\":\"kB\","
        "\"state_class\":\"measurement\",");

    PUB_SENSOR("tx_fails", "TX Failures",
        "\"state_class\":\"total_increasing\",");

    PUB_SENSOR("rssi", "RSSI",
        "\"device_class\":\"signal_strength\","
        "\"unit_of_measurement\":\"dBm\","
        "\"state_class\":\"measurement\",");

    PUB_SENSOR("snr", "SNR",
        "\"unit_of_measurement\":\"dB\","
        "\"state_class\":\"measurement\",");

    #undef PUB_SENSOR

    ESP_LOGI(TAG, "HA discovery published for detector 0x%02X", device_id);
}

/* Pre-v3.0 bridges published per-bridge detector entities
 * (batear_<bridge>_det_<XX>_*). Clearing their retained discovery configs
 * makes HA remove them; the bridge-independent entities replace them. */
static void clear_legacy_detector_discovery(uint8_t device_id)
{
    static const char *const kEntities[][2] = {
        { "binary_sensor", "drone" },
        { "sensor", "battery_v" },  { "sensor", "fw_version" },
        { "sensor", "uptime_min" }, { "sensor", "free_heap_kb" },
        { "sensor", "tx_fails" },   { "sensor", "rssi" },
        { "sensor", "snr" },
    };
    char topic[128];
    for (size_t i = 0; i < sizeof(kEntities) / sizeof(kEntities[0]); i++) {
        snprintf(topic, sizeof(topic), "homeassistant/%s/batear_%s_det_%02X/%s/config",
                 kEntities[i][0], s_device_id, device_id, kEntities[i][1]);
        esp_mqtt_client_publish(s_mqtt, topic, "", 0, 1, 1);
    }
}

/* ================================================================
 * Cross-bridge ordering of detector state
 * ================================================================ */

/* Same rule as the radio replay check: newer boot_id (serial arithmetic),
 * or same boot with a higher seq. */
static bool newer_than(uint16_t boot, uint16_t seq, const det_state_t *last)
{
    if (!last->seen) return true;
    int16_t boot_delta = (int16_t)(boot - last->boot_id);
    if (boot_delta != 0) return boot_delta > 0;
    return seq > last->seq;
}

/* Record (boot, seq) for a detector if newer. Returns whether it was. */
static bool det_state_advance(uint8_t id, uint16_t boot, uint16_t seq)
{
    bool advanced;
    portENTER_CRITICAL(&s_det_state_mux);
    advanced = newer_than(boot, seq, &s_det_state[id]);
    if (advanced) {
        s_det_state[id].seen    = true;
        s_det_state[id].boot_id = boot;
        s_det_state[id].seq     = seq;
    }
    portEXIT_CRITICAL(&s_det_state_mux);
    return advanced;
}

/* batear/det/<XX>/state from the broker (retained or live, any bridge). */
static void on_det_state_message(const char *topic, int topic_len,
                                 const char *data, int data_len)
{
    unsigned id;
    char t[48];
    char d[160];
    if (topic_len <= 0 || topic_len >= (int)sizeof(t) ||
        data_len <= 0 || data_len >= (int)sizeof(d)) {
        return;
    }
    memcpy(t, topic, topic_len);
    t[topic_len] = '\0';
    memcpy(d, data, data_len);
    d[data_len] = '\0';
    if (sscanf(t, "batear/det/%2x/state", &id) != 1 || id > 255) {
        return;
    }
    const char *pb = strstr(d, "\"boot\":");
    const char *ps = strstr(d, "\"seq\":");
    unsigned boot, seq;
    if (!pb || !ps || sscanf(pb, "\"boot\":%u", &boot) != 1 ||
        sscanf(ps, "\"seq\":%u", &seq) != 1) {
        return;
    }
    det_state_advance((uint8_t)id, (uint16_t)boot, (uint16_t)seq);
}

/* ================================================================
 * MQTT event handler
 * ================================================================ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *data)
{
    auto *ev = static_cast<esp_mqtt_event_handle_t>(data);

    switch (ev->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to %s", s_mqtt_url);
        s_mqtt_connected = true;
        esp_mqtt_client_publish(s_mqtt, s_topic_avail, "online", 0, 1, 1);
        publish_ha_discovery();
        esp_mqtt_client_subscribe(s_mqtt, "batear/det/+/state", 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        /* Re-publish per-detector discovery on reconnect so a broker that
         * purged retained messages (or a freshly started HA) picks them up. */
        memset(s_det_discovered, 0, sizeof(s_det_discovered));
        break;

    case MQTT_EVENT_DATA:
        on_det_state_message(ev->topic, ev->topic_len, ev->data, ev->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type=%d", ev->error_handle->error_type);
        break;

    default:
        break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri  = s_mqtt_url;
    cfg.credentials.username = s_mqtt_user;
    cfg.credentials.authentication.password = s_mqtt_pass;

    cfg.session.last_will.topic   = s_topic_avail;
    cfg.session.last_will.msg     = "offline";
    cfg.session.last_will.msg_len = 7;
    cfg.session.last_will.qos     = 1;
    cfg.session.last_will.retain  = 1;

    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

/* ================================================================
 * Task entry
 * ================================================================ */

QueueHandle_t g_mqtt_event_queue = NULL;

extern "C" void MqttTask(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "MqttTask start (core %d)", xPortGetCoreID());

    load_config();

    if (!wifi_init_sta()) {
        ESP_LOGE(TAG, "WiFi failed — MQTT task suspending");
        vTaskSuspend(NULL);
        return;
    }

    mqtt_start();

    MqttEvent_t ev;
    char json[512];

    for (;;) {
        if (xQueueReceive(g_mqtt_event_queue, &ev, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (!s_mqtt_connected) {
                ESP_LOGW(TAG, "MQTT not connected — dropping event");
                continue;
            }

            int64_t ts = esp_timer_get_time() / 1000000LL;

            /* First sight of this detector → publish HA discovery bundle. */
            if (!s_det_discovered[ev.device_id]) {
                clear_legacy_detector_discovery(ev.device_id);
                publish_detector_discovery(ev.device_id);
                s_det_discovered[ev.device_id] = true;
            }

            const char *event_name = (ev.event_type == 0x02) ? "heartbeat" :
                                     (ev.event_type == 0x01) ? "alarm"     :
                                                               "clear";
            const mesh_stats_t *ms = mesh_stats();

            snprintf(json, sizeof(json),
                "{"
                    "\"drone_detected\":%s,"
                    "\"event\":\"%s\","
                    "\"detector_id\":%u,"
                    "\"rssi\":%.0f,"
                    "\"snr\":%.1f,"
                    "\"rms_db\":%u,"
                    "\"f0_bin\":%u,"
                    "\"boot\":%u,"
                    "\"seq\":%u,"
                    "\"battery_v\":%u.%02u,"
                    "\"fw_version\":\"%u.%u.%u\","
                    "\"uptime_min\":%u,"
                    "\"free_heap_kb\":%u,"
                    "\"tx_fails\":%u,"
                    "\"flags\":%u,"
                    "\"bridge\":\"%s\","
                    "\"timestamp\":%lld"
                "}",
                ev.alarm ? "true" : "false",
                event_name,
                ev.device_id,
                ev.rssi, ev.snr,
                ev.rms_db, ev.f0_bin, ev.boot_id, ev.seq,
                (unsigned)(ev.vbat_mv / 1000U),
                (unsigned)((ev.vbat_mv / 10U) % 100U),
                ev.fw_major, ev.fw_minor, ev.fw_patch,
                ev.uptime_min, ev.free_heap_kb,
                ev.tx_fail_count, ev.flags,
                s_device_id,
                (long long)ts);

            char det_topic[48];

            /* Telemetry: any bridge may refresh it; last writer wins. */
            snprintf(det_topic, sizeof(det_topic), "batear/det/%02X/status", ev.device_id);
            esp_mqtt_client_publish(s_mqtt, det_topic, json, 0, 1, 1);

            /* Drone state: only ALARM/CLEAR, and only if newer than what any
             * bridge already published. */
            if (ev.event_type == 0x00 || ev.event_type == 0x01) {
                if (det_state_advance(ev.device_id, ev.boot_id, ev.seq)) {
                    char state[96];
                    snprintf(state, sizeof(state),
                             "{\"state\":\"%s\",\"boot\":%u,\"seq\":%u,\"bridge\":\"%s\"}",
                             ev.event_type == 0x01 ? "ON" : "OFF",
                             ev.boot_id, ev.seq, s_device_id);
                    snprintf(det_topic, sizeof(det_topic), "batear/det/%02X/state", ev.device_id);
                    esp_mqtt_client_publish(s_mqtt, det_topic, state, 0, 1, 1);
                    ESP_LOGI(TAG, "pub → %s : %s", det_topic, state);
                } else {
                    ESP_LOGI(TAG, "det 0x%02X %s boot=%u seq=%u is older than the "
                                  "published state — not overriding",
                             ev.device_id, event_name, ev.boot_id, ev.seq);
                }
            }

            /* Bridge-wide topic keeps the original per-bridge HA entities;
             * mesh counters ride along as attributes. */
            char bridge_json[640];
            snprintf(bridge_json, sizeof(bridge_json),
                     "%.*s,\"mesh_relayed\":%lu,\"mesh_dups\":%lu,"
                     "\"mesh_suppressed\":%lu,\"mesh_rate_limited\":%lu}",
                     (int)strlen(json) - 1, json,
                     (unsigned long)ms->relayed, (unsigned long)ms->dup_dropped,
                     (unsigned long)ms->suppressed, (unsigned long)ms->rate_limited);
            esp_mqtt_client_publish(s_mqtt, s_topic_status, bridge_json, 0, 1, 0);

            ESP_LOGI(TAG, "pub → batear/det/%02X/status : %s", ev.device_id, json);
        }
    }
}
