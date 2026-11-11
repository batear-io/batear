/*
 * node_rx.cpp — received-packet handling for LoRa nodes with rx enabled:
 * AES-GCM decrypt + replay check, OLED + LED, hand-off to MqttTask.
 *
 * The radio itself is owned by lora_radio.cpp, which calls in here.
 *
 * Board: Heltec WiFi LoRa 32 V3 / V4 (ESP32-S3 + SX1262 + SSD1306)
 */

#include "node_rx.h"
#include "mqtt_task.h"
#include "lora_provision.h"
#include "pin_config.h"
#include "oled.h"
#include "mesh.h"
#include "node_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "rx";

/* Per-device state */
typedef struct {
    bool     seen;
    bool     alarm;
    uint16_t last_boot_id;
    uint16_t last_seq;
    uint16_t last_vbat_mv;
} device_state_t;

#define MAX_DEVICES 256
static device_state_t s_devices[MAX_DEVICES];
static uint32_t s_rx_count;
static uint32_t s_reject_count;

/* A packet is fresh if its boot_id is newer (serial-number arithmetic, so the
 * 16-bit counter may wrap) or it's the same boot with a higher seq. */
static bool is_fresh(const device_state_t *dev, const lora_header_t *hdr)
{
    if (!dev->seen) return true;
    int16_t boot_delta = (int16_t)(hdr->boot_id - dev->last_boot_id);
    if (boot_delta != 0) return boot_delta > 0;
    return hdr->seq > dev->last_seq;
}

/* ---- display helpers ---- */

/* Bottom status line: received, rejected, and relayed (relay nodes only). */
static void print_status_line(void)
{
    char line[32];
    if (node_caps()->relay) {
        snprintf(line, sizeof(line), "rx:%lu bad:%lu rl:%lu",
                 (unsigned long)s_rx_count, (unsigned long)s_reject_count,
                 (unsigned long)mesh_stats()->relayed);
    } else {
        snprintf(line, sizeof(line), "rx:%lu bad:%lu",
                 (unsigned long)s_rx_count, (unsigned long)s_reject_count);
    }
    oled_print(0, 7, line);
}

static void display_idle(void)
{
    oled_clear();
    oled_print(0, 0, "BATEAR GATEWAY");
    oled_print(0, 2, "Listening...");
    print_status_line();
    oled_flush();
}

static const char *event_label(uint8_t ev)
{
    switch (ev) {
        case LORA_EVENT_ALARM:     return "!! ALARM !!";
        case LORA_EVENT_CLEAR:     return "CLEAR";
        case LORA_EVENT_TELEMETRY: return "heartbeat";
        default:                   return "??";
    }
}

static void display_event(const lora_plaintext_t *pt, float rssi, float snr)
{
    char line[32];
    oled_clear();
    oled_print(0, 0, "BATEAR GATEWAY");

    snprintf(line, sizeof(line), "Dev %02X: %s",
             pt->device_id, event_label(pt->event_type));
    oled_print(0, 2, line);

    snprintf(line, sizeof(line), "RSSI:%d  SNR:%.1f", (int)rssi, snr);
    oled_print(0, 4, line);

    uint32_t vbat_mv = lora_vbat_decode_mv(pt->vbat_cv);
    if (pt->event_type == LORA_EVENT_TELEMETRY) {
        snprintf(line, sizeof(line), "vbat:%u.%02uV up:%um",
                 (unsigned)(vbat_mv / 1000), (unsigned)((vbat_mv / 10) % 100),
                 (unsigned)pt->uptime_min);
    } else {
        snprintf(line, sizeof(line), "rms:%udB  f0bin:%u",
                 pt->rms_db, pt->f0_bin);
    }
    oled_print(0, 5, line);

    snprintf(line, sizeof(line), "seq:%u v%u.%u.%u",
             pt->seq, pt->fw_major, pt->fw_minor, pt->fw_patch);
    oled_print(0, 6, line);

    print_status_line();
    oled_flush();
}

static void update_led(void)
{
    bool any = false;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].seen && s_devices[i].alarm) { any = true; break; }
    }
    gpio_set_level((gpio_num_t)PIN_LED, any ? 1 : 0);
}

/* ---- entry points ---- */

static void deliver(const lora_header_t *hdr, const lora_plaintext_t *pt,
                    float rssi, float snr)
{
    device_state_t *dev = &s_devices[pt->device_id];

    s_rx_count++;
    dev->seen         = true;
    dev->last_boot_id = hdr->boot_id;
    dev->last_seq     = pt->seq;
    /* Heartbeat packets carry telemetry only — never clobber the cached
     * alarm state with them, so an ALARM→heartbeat sequence keeps the
     * LED and MQTT discovery binary_sensor in the ALARM state. */
    if (pt->event_type == LORA_EVENT_ALARM) {
        dev->alarm = true;
    } else if (pt->event_type == LORA_EVENT_CLEAR) {
        dev->alarm = false;
    }
    uint32_t vbat_mv32 = lora_vbat_decode_mv(pt->vbat_cv);
    dev->last_vbat_mv = (vbat_mv32 > 0xFFFFU) ? 0xFFFFU : (uint16_t)vbat_mv32;

    ESP_LOGI(TAG,
             "dev=0x%02X %s boot=%u seq=%u f0bin=%u rms=%udB RSSI=%.0f SNR=%.1f  "
             "vbat=%umV fw=%u.%u.%u up=%umin heap=%ukB txfail=%u fl=0x%02X",
             pt->device_id, event_label(pt->event_type),
             hdr->boot_id, pt->seq, pt->f0_bin, pt->rms_db, rssi, snr,
             (unsigned)vbat_mv32,
             pt->fw_major, pt->fw_minor, pt->fw_patch,
             pt->uptime_min, pt->free_heap_kb,
             pt->tx_fail_count, pt->flags);

    update_led();
    display_event(pt, rssi, snr);

    if (g_mqtt_event_queue) {
        MqttEvent_t mev = {};
        mev.device_id     = pt->device_id;
        mev.event_type    = pt->event_type;
        mev.alarm         = dev->alarm;
        mev.rssi          = rssi;
        mev.snr           = snr;
        mev.rms_db        = pt->rms_db;
        mev.f0_bin        = pt->f0_bin;
        mev.boot_id       = hdr->boot_id;
        mev.seq           = pt->seq;
        mev.vbat_mv       = dev->last_vbat_mv;
        mev.fw_major      = pt->fw_major;
        mev.fw_minor      = pt->fw_minor;
        mev.fw_patch      = pt->fw_patch;
        mev.uptime_min    = pt->uptime_min;
        mev.free_heap_kb  = pt->free_heap_kb;
        mev.tx_fail_count = pt->tx_fail_count;
        mev.flags         = pt->flags;
        if (xQueueSend(g_mqtt_event_queue, &mev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "MQTT queue full — event dropped");
        }
    }
}

extern "C" void node_rx_init(void)
{
#if BOARD_HAS_VEXT
    gpio_config_t vext_cfg = {};
    vext_cfg.pin_bit_mask = (1ULL << PIN_VEXT);
    vext_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&vext_cfg);
    gpio_set_level((gpio_num_t)PIN_VEXT, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    oled_init(PIN_OLED_SDA, PIN_OLED_SCL, PIN_OLED_RST);

    gpio_config_t led_cfg = {};
    led_cfg.pin_bit_mask = (1ULL << PIN_LED);
    led_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&led_cfg);
    gpio_set_level((gpio_num_t)PIN_LED, 0);

    display_idle();
}

extern "C" void node_rx_fatal(const char *msg)
{
    oled_clear();
    oled_print(0, 0, msg);
    oled_flush();
}

extern "C" void node_rx_error(void)
{
    s_reject_count++;
    display_idle();
}

extern "C" node_rx_result_t node_rx_packet(const uint8_t *buf, size_t len,
                                           float rssi, float snr, uint8_t *event_type)
{
    if (len != sizeof(lora_packet_t)) {
        if (len == LORA_PACKET_LEN_V1) {
            ESP_LOGW(TAG, "v1 frame (%u B) — detector runs pre-v3.0 firmware, upgrade it",
                     (unsigned)len);
        }
        node_rx_error();
        return NODE_RX_BAD;
    }

    const lora_packet_t *pkt = reinterpret_cast<const lora_packet_t *>(buf);
    lora_plaintext_t pt;
    const uint8_t *net_key = lora_get_keys()->net_key;
    if (!lora_decrypt(net_key, pkt, &pt)) {
        ESP_LOGW(TAG, "decrypt FAILED — wrong key, forged, or bad frame version");
        node_rx_error();
        return NODE_RX_BAD;
    }

    const device_state_t *dev = &s_devices[pt.device_id];
    if (!is_fresh(dev, &pkt->hdr)) {
        ESP_LOGW(TAG, "REPLAY dev=0x%02X boot=%u seq=%u (last boot=%u seq=%u)",
                 pt.device_id, pkt->hdr.boot_id, pkt->hdr.seq,
                 dev->last_boot_id, dev->last_seq);
        node_rx_error();
        return NODE_RX_STALE;
    }

    deliver(&pkt->hdr, &pt, rssi, snr);
    *event_type = pt.event_type;
    return NODE_RX_OK;
}

extern "C" void node_rx_local(const lora_header_t *hdr, const lora_plaintext_t *pt)
{
    deliver(hdr, pt, 0.0f, 0.0f);
}
