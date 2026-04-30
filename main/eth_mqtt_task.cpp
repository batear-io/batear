/*
 * eth_mqtt_task.cpp — W5500 Ethernet + MQTT + HA Discovery for Wired Detector
 *
 * Credentials are read from NVS namespace "wired_cfg" first; if a key
 * is absent the compile-time Kconfig default is used instead.
 *
 * Runs on Core 0.  AudioTask (Core 1) sends DroneEvent_t items via
 * g_drone_event_queue; this task publishes them as JSON to:
 *   batear/nodes/<device_id>/status
 *
 * LWT ensures HA marks the detector offline if it disconnects.
 */

#include "eth_mqtt_task.h"
#include "http_api.h"
#include "drone_detector.h"
#include "pin_config.h"
#include "lorawan_provision.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "mqtt_client.h"

#include "esp_netif_ip_addr.h"
#include "lwip/ip4_addr.h"

#include <cstring>
#include <cstdio>
#include <cmath>

static const char *TAG = "eth_mqtt";

#define CFG_STR_MAX  128
#define DEVID_MAX     32

/* ---- runtime config (populated from NVS → Kconfig fallback) ---- */
static char s_mqtt_url[CFG_STR_MAX];
static char s_mqtt_user[CFG_STR_MAX];
static char s_mqtt_pass[CFG_STR_MAX];
static char s_device_id[DEVID_MAX];

#define IP_STR_MAX 16
static char s_eth_ip[IP_STR_MAX];
static char s_eth_gw[IP_STR_MAX];
static char s_eth_mask[IP_STR_MAX];
static char s_eth_dns[IP_STR_MAX];

/* ---- Ethernet event synchronisation ---- */
static EventGroupHandle_t s_eth_eg;
#define ETH_CONNECTED_BIT  BIT0
#define ETH_FAIL_BIT       BIT1

/* ---- MQTT state ---- */
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_connected;

/* ---- pre-built topic strings ---- */
static char s_topic_avail[64];
static char s_topic_status[80];

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
    bool opened = (nvs_open("wired_cfg", NVS_READONLY, &h) == ESP_OK);

    if (opened) {
        load_nvs_str(h, "mqtt_url",  s_mqtt_url,  sizeof(s_mqtt_url),
                     CONFIG_BATEAR_MQTT_BROKER_URL);
        load_nvs_str(h, "mqtt_user", s_mqtt_user, sizeof(s_mqtt_user),
                     CONFIG_BATEAR_MQTT_USER);
        load_nvs_str(h, "mqtt_pass", s_mqtt_pass, sizeof(s_mqtt_pass),
                     CONFIG_BATEAR_MQTT_PASS);
        load_nvs_str(h, "device_id", s_device_id, DEVID_MAX,
                     CONFIG_BATEAR_WIRED_DEVICE_ID);
        load_nvs_str(h, "eth_ip",   s_eth_ip,   sizeof(s_eth_ip),
                     CONFIG_BATEAR_ETH_STATIC_IP);
        load_nvs_str(h, "eth_gw",   s_eth_gw,   sizeof(s_eth_gw),
                     CONFIG_BATEAR_ETH_GATEWAY);
        load_nvs_str(h, "eth_mask", s_eth_mask,  sizeof(s_eth_mask),
                     CONFIG_BATEAR_ETH_NETMASK);
        load_nvs_str(h, "eth_dns",  s_eth_dns,   sizeof(s_eth_dns),
                     CONFIG_BATEAR_ETH_DNS);
        nvs_close(h);
    } else {
        strncpy(s_mqtt_url,  CONFIG_BATEAR_MQTT_BROKER_URL, sizeof(s_mqtt_url) - 1);
        strncpy(s_mqtt_user, CONFIG_BATEAR_MQTT_USER, sizeof(s_mqtt_user) - 1);
        strncpy(s_mqtt_pass, CONFIG_BATEAR_MQTT_PASS, sizeof(s_mqtt_pass) - 1);
        strncpy(s_device_id, CONFIG_BATEAR_WIRED_DEVICE_ID, sizeof(s_device_id) - 1);
        strncpy(s_eth_ip,    CONFIG_BATEAR_ETH_STATIC_IP,   sizeof(s_eth_ip) - 1);
        strncpy(s_eth_gw,    CONFIG_BATEAR_ETH_GATEWAY,     sizeof(s_eth_gw) - 1);
        strncpy(s_eth_mask,  CONFIG_BATEAR_ETH_NETMASK,     sizeof(s_eth_mask) - 1);
        strncpy(s_eth_dns,   CONFIG_BATEAR_ETH_DNS,         sizeof(s_eth_dns) - 1);
        ESP_LOGW(TAG, "NVS namespace 'wired_cfg' not found — using Kconfig defaults");
    }

    snprintf(s_topic_avail, sizeof(s_topic_avail),
             "batear/nodes/%s/availability", s_device_id);
    snprintf(s_topic_status, sizeof(s_topic_status),
             "batear/nodes/%s/status", s_device_id);

    ESP_LOGI(TAG, "cfg: mqtt_url=%s device_id=%s", s_mqtt_url, s_device_id);
}

/* ================================================================
 * W5500 Ethernet
 * ================================================================ */

static void eth_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Ethernet link down");
        xEventGroupClearBits(s_eth_eg, ETH_CONNECTED_BIT);
        xEventGroupSetBits(s_eth_eg, ETH_FAIL_BIT);
    } else if (id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "Ethernet started");
    } else if (id == ETHERNET_EVENT_STOP) {
        ESP_LOGW(TAG, "Ethernet stopped");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupClearBits(s_eth_eg, ETH_FAIL_BIT);
        xEventGroupSetBits(s_eth_eg, ETH_CONNECTED_BIT);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "IP lost");
        xEventGroupClearBits(s_eth_eg, ETH_CONNECTED_BIT);
    }
}

static bool eth_init(void)
{
    s_eth_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_inherent_config_t netif_base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t netif_cfg = {
        .base   = &netif_base,
        .driver = NULL,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *netif = esp_netif_new(&netif_cfg);

    /* W5500 driver registers an ISR on its INT pin via gpio_isr_handler_add.
     * That requires the global GPIO ISR service to already be installed.
     * Treat ESP_ERR_INVALID_STATE as success in case another component
     * (e.g. RadioLib HAL on co-deployed builds) installed it first. */
    {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
    }

    /* SPI bus for W5500 */
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num   = PIN_ETH_MISO;
    buscfg.mosi_io_num   = PIN_ETH_MOSI;
    buscfg.sclk_io_num   = PIN_ETH_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits   = 16;
    devcfg.address_bits   = 8;
    devcfg.mode           = 0;
    devcfg.clock_speed_hz = 36 * 1000 * 1000;  /* 36 MHz */
    devcfg.spics_io_num   = PIN_ETH_CS;
    devcfg.queue_size     = 20;

    /* W5500 MAC */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    w5500_config.int_gpio_num = PIN_ETH_INT;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    /* W5500 PHY */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = PIN_ETH_ADDR;
    phy_config.reset_gpio_num = PIN_ETH_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    /* Install driver */
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    /* W5500 has no built-in MAC: derive a locally-administered address from
     * the ESP32 base MAC so DHCP servers/switches see a valid Ethernet ID.
     * Without this the W5500 transmits with 00:00:00:00:00:00 and DHCP
     * silently fails. */
    uint8_t eth_mac[6] = {};
    ESP_ERROR_CHECK(esp_read_mac(eth_mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));
    ESP_LOGI(TAG, "W5500 MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             eth_mac[0], eth_mac[1], eth_mac[2],
             eth_mac[3], eth_mac[4], eth_mac[5]);

    /* Attach to TCP/IP stack */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(netif, glue));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                ip_event_handler, NULL));

    /* Apply static IP or fall back to DHCP */
    bool use_static = (s_eth_ip[0] != '\0');
    if (use_static) {
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));

        esp_netif_ip_info_t ip_info = {};
        esp_netif_str_to_ip4(s_eth_ip, &ip_info.ip);
        esp_netif_str_to_ip4(s_eth_mask[0] ? s_eth_mask : "255.255.255.0",
                             &ip_info.netmask);
        if (s_eth_gw[0]) {
            esp_netif_str_to_ip4(s_eth_gw, &ip_info.gw);
        }
        ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));

        /* DNS: use explicit value, or fall back to gateway */
        esp_netif_dns_info_t dns_info = {};
        const char *dns_src = s_eth_dns[0] ? s_eth_dns : s_eth_gw;
        if (dns_src[0]) {
            esp_netif_str_to_ip4(dns_src, &dns_info.ip.u_addr.ip4);
            dns_info.ip.type = ESP_IPADDR_TYPE_V4;
            ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN,
                                                    &dns_info));
        }

        ESP_LOGI(TAG, "Static IP: %s  GW: %s  Mask: %s  DNS: %s",
                 s_eth_ip,
                 s_eth_gw[0] ? s_eth_gw : "(none)",
                 s_eth_mask[0] ? s_eth_mask : "255.255.255.0",
                 dns_src[0] ? dns_src : "(none)");
    }

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Waiting for Ethernet link%s...",
             use_static ? "" : " + DHCP");
    EventBits_t bits = xEventGroupWaitBits(
        s_eth_eg, ETH_CONNECTED_BIT | ETH_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & ETH_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Ethernet connected%s",
                 use_static ? " (static IP)" : " (DHCP)");
        return true;
    }
    ESP_LOGE(TAG, "Ethernet FAILED — no link%s",
             use_static ? "" : " or DHCP timeout");
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
                "\"name\":\"Batear Wired Detector %s\","
                "\"manufacturer\":\"Batear\","
                "\"model\":\"ESP32-S3 Wired Detector\""
            "}"
        "}",
        s_device_id, s_device_id,
        s_topic_status, s_topic_avail, s_topic_status,
        s_device_id, s_device_id);

    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "HA discovery published → %s", topic);

    /* Confidence sensor */
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/batear_%s/confidence/config", s_device_id);

    snprintf(payload, sizeof(payload),
        "{"
            "\"name\":\"Batear %s Confidence\","
            "\"unique_id\":\"batear_%s_confidence\","
            "\"unit_of_measurement\":\"%%\","
            "\"state_topic\":\"%s\","
            "\"value_template\":\"{{ (value_json.confidence * 100) | round(1) }}\","
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
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
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
 * RMS → dB helper (same scale as LoRa detector)
 * ================================================================ */

static uint8_t rms_to_db(float rms)
{
    if (rms <= 0.0f) return 0;
    float db = 20.0f * log10f(rms) + 120.0f;
    if (db < 0.0f) return 0;
    if (db > 255.0f) return 255;
    return (uint8_t)db;
}

/* ================================================================
 * Task entry
 * ================================================================ */

/* ---- helpers for link state ---- */

static bool eth_is_connected(void)
{
    return (xEventGroupGetBits(s_eth_eg) & ETH_CONNECTED_BIT) != 0;
}

static void wait_for_link(void)
{
    while (!eth_is_connected()) {
        ESP_LOGW(TAG, "Ethernet down — waiting for link...");
        xEventGroupWaitBits(s_eth_eg, ETH_CONNECTED_BIT,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    }
}

/* ================================================================
 * Task entry
 * ================================================================ */

extern "C" void EthMqttTask(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "EthMqttTask start (core %d)", xPortGetCoreID());

    load_config();

    if (!eth_init()) {
        ESP_LOGE(TAG, "Ethernet init failed — retrying every 30s");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            if (eth_is_connected()) break;
        }
    }

    mqtt_start();
    http_api_start();

    DroneEvent_t ev;
    char json[256];
    uint16_t tx_seq = 0;

    for (;;) {
        if (xQueueReceive(g_drone_event_queue, &ev, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (!eth_is_connected()) {
                ESP_LOGW(TAG, "Ethernet down — waiting for reconnect");
                wait_for_link();
            }

            if (!s_mqtt_connected) {
                ESP_LOGW(TAG, "MQTT not connected — dropping event");
                continue;
            }

            int64_t ts = esp_timer_get_time() / 1000000LL;
            uint8_t det_id = lorawan_get_keys()->device_id;

            HttpDroneStatus_t hst = {
                .drone_detected = (ev.type == DRONE_EVENT_ALARM),
                .detector_id    = det_id,
                .rms_db         = rms_to_db(ev.rms),
                .f0_bin         = static_cast<uint16_t>(ev.f0_bin),
                .confidence     = ev.peak_ratio,
                .timestamp      = ts,
            };
            http_api_update_status(&hst);

            snprintf(json, sizeof(json),
                "{"
                    "\"drone_detected\":%s,"
                    "\"detector_id\":%u,"
                    "\"rms_db\":%u,"
                    "\"f0_bin\":%u,"
                    "\"seq\":%u,"
                    "\"confidence\":%.4f,"
                    "\"timestamp\":%lld"
                "}",
                (ev.type == DRONE_EVENT_ALARM) ? "true" : "false",
                (unsigned)det_id,
                (unsigned)rms_to_db(ev.rms), (unsigned)ev.f0_bin,
                (unsigned)tx_seq++,
                (double)ev.peak_ratio,
                (long long)ts);

            esp_mqtt_client_publish(s_mqtt, s_topic_status, json, 0, 1, 0);

            ESP_LOGI(TAG, "pub → %s : %s", s_topic_status, json);
        }
    }
}
