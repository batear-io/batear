/*
 * gateway_task.cpp — LoRa RX + AES-GCM decrypt + OLED + LED (Core 0)
 *
 * Listens for encrypted packets from Batear detectors.
 * Shows status on SSD1306 OLED, lights LED on ALARM.
 *
 * Board: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262 + SSD1306)
 */

#include "gateway_task.h"
#include "mqtt_task.h"
#include "lora_crypto.h"
#include "lorawan_provision.h"
#include "EspIdfHal.h"
#include "pin_config.h"
#include "oled.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include <RadioLib.h>

static const char *TAG = "gw";

/* LoRa RF — must match detector (freq and sync_word come from NVS at runtime) */
#define LORA_BW_KHZ         125.0f
#define LORA_SF             10
#define LORA_CR             5
#define LORA_TX_DBM         22

#define GW_MAX_CONSECUTIVE_ERRORS  5

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

/* ---- radio management ---- */

static EspIdfHal *s_hal   = nullptr;
static Module    *s_mod   = nullptr;
static SX1262    *s_radio = nullptr;

static bool gw_radio_init(void)
{
    const lorawan_keys_t *keys = lorawan_get_keys();
    float freq_mhz = keys->lora_freq_khz / 1000.0f;
    uint8_t sync_word = keys->lora_sync_word;

    s_hal   = new EspIdfHal(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
    s_mod   = new Module(s_hal, PIN_LORA_CS, PIN_LORA_DIO1,
                         PIN_LORA_RST, PIN_LORA_BUSY);
    s_radio = new SX1262(s_mod);

    int16_t state = s_radio->begin(freq_mhz, LORA_BW_KHZ, LORA_SF, LORA_CR,
                                    sync_word, LORA_TX_DBM);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "begin() failed: %d", state);
        return false;
    }
    s_radio->setTCXO(BOARD_LORA_TCXO_V);
    s_radio->setDio2AsRfSwitch(BOARD_LORA_DIO2_AS_RF);

    ESP_LOGI(TAG, "SX1262 ready  freq=%.1f MHz  SF=%d  BW=%.0f kHz  sw=0x%02X",
             freq_mhz, LORA_SF, LORA_BW_KHZ, sync_word);
    return true;
}

static bool gw_radio_reinit(void)
{
    ESP_LOGW(TAG, "attempting radio reinit after consecutive errors");

    delete s_radio; s_radio = nullptr;
    delete s_mod;   s_mod   = nullptr;
    delete s_hal;   s_hal   = nullptr;

    vTaskDelay(pdMS_TO_TICKS(100));
    return gw_radio_init();
}

/* ---- display helpers ---- */

static void display_idle(void)
{
    oled_clear();
    oled_print(0, 0, "BATEAR GATEWAY");
    oled_print(0, 2, "Listening...");
    char line[22];
    snprintf(line, sizeof(line), "rx:%lu bad:%lu",
             (unsigned long)s_rx_count, (unsigned long)s_reject_count);
    oled_print(0, 7, line);
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

    snprintf(line, sizeof(line), "rx:%lu bad:%lu",
             (unsigned long)s_rx_count, (unsigned long)s_reject_count);
    oled_print(0, 7, line);
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

/* ---- task entry ---- */

extern "C" void GatewayTask(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "GatewayTask start (core %d)", xPortGetCoreID());

#if BOARD_HAS_VEXT
    gpio_config_t vext_cfg = {};
    vext_cfg.pin_bit_mask = (1ULL << PIN_VEXT);
    vext_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&vext_cfg);
    gpio_set_level((gpio_num_t)PIN_VEXT, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    /* OLED */
    oled_init(PIN_OLED_SDA, PIN_OLED_SCL, PIN_OLED_RST);
    oled_clear();
    oled_print(0, 0, "BATEAR GATEWAY");
    oled_print(0, 2, "Initializing...");
    oled_flush();

    /* LED */
    gpio_config_t led_cfg = {};
    led_cfg.pin_bit_mask = (1ULL << PIN_LED);
    led_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&led_cfg);
    gpio_set_level((gpio_num_t)PIN_LED, 0);

    /* LoRa */
    if (!gw_radio_init()) {
        oled_clear();
        oled_print(0, 0, "RADIO FAIL");
        oled_flush();
        vTaskSuspend(NULL);
        return;
    }

    display_idle();

    uint8_t rx_buf[64];
    int consecutive_errors = 0;

    for (;;) {
        int16_t state = s_radio->receive(rx_buf, sizeof(rx_buf));

        if (state == RADIOLIB_ERR_RX_TIMEOUT) {
            consecutive_errors = 0;
            continue;
        }

        if (state != RADIOLIB_ERR_NONE) {
            ESP_LOGW(TAG, "receive error: %d", state);
            s_reject_count++;
            consecutive_errors++;
            if (consecutive_errors >= GW_MAX_CONSECUTIVE_ERRORS) {
                if (gw_radio_reinit()) {
                    consecutive_errors = 0;
                    display_idle();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        consecutive_errors = 0;

        size_t len  = s_radio->getPacketLength();
        float  rssi = s_radio->getRSSI();
        float  snr  = s_radio->getSNR();

        if (len != sizeof(lora_packet_t)) {
            if (len == LORA_PACKET_LEN_V1) {
                ESP_LOGW(TAG, "v1 frame (%u B) — detector runs pre-v3.0 firmware, upgrade it",
                         (unsigned)len);
            }
            s_reject_count++;
            display_idle();
            continue;
        }

        const lora_packet_t *pkt = reinterpret_cast<const lora_packet_t *>(rx_buf);
        lora_plaintext_t pt;
        const uint8_t *net_key = lorawan_get_keys()->app_key;
        if (!lora_decrypt(net_key, pkt, &pt)) {
            ESP_LOGW(TAG, "decrypt FAILED — wrong key, forged, or bad frame version");
            s_reject_count++;
            display_idle();
            continue;
        }

        device_state_t *dev = &s_devices[pt.device_id];
        if (!is_fresh(dev, &pkt->hdr)) {
            ESP_LOGW(TAG, "REPLAY dev=0x%02X boot=%u seq=%u (last boot=%u seq=%u)",
                     pt.device_id, pkt->hdr.boot_id, pkt->hdr.seq,
                     dev->last_boot_id, dev->last_seq);
            s_reject_count++;
            display_idle();
            continue;
        }

        s_rx_count++;
        dev->seen         = true;
        dev->last_boot_id = pkt->hdr.boot_id;
        dev->last_seq     = pt.seq;
        /* Heartbeat packets carry telemetry only — never clobber the cached
         * alarm state with them, so an ALARM→heartbeat sequence keeps the
         * LED and MQTT discovery binary_sensor in the ALARM state. */
        if (pt.event_type == LORA_EVENT_ALARM) {
            dev->alarm = true;
        } else if (pt.event_type == LORA_EVENT_CLEAR) {
            dev->alarm = false;
        }
        uint32_t vbat_mv32 = lora_vbat_decode_mv(pt.vbat_cv);
        dev->last_vbat_mv = (vbat_mv32 > 0xFFFFU) ? 0xFFFFU : (uint16_t)vbat_mv32;

        ESP_LOGI(TAG,
                 "dev=0x%02X %s boot=%u seq=%u f0bin=%u rms=%udB RSSI=%.0f SNR=%.1f  "
                 "vbat=%umV fw=%u.%u.%u up=%umin heap=%ukB txfail=%u fl=0x%02X",
                 pt.device_id, event_label(pt.event_type),
                 pkt->hdr.boot_id, pt.seq, pt.f0_bin, pt.rms_db, rssi, snr,
                 (unsigned)vbat_mv32,
                 pt.fw_major, pt.fw_minor, pt.fw_patch,
                 pt.uptime_min, pt.free_heap_kb,
                 pt.tx_fail_count, pt.flags);

        update_led();
        display_event(&pt, rssi, snr);

        if (g_mqtt_event_queue) {
            MqttEvent_t mev = {};
            mev.device_id     = pt.device_id;
            mev.event_type    = pt.event_type;
            mev.alarm         = dev->alarm;
            mev.rssi          = rssi;
            mev.snr           = snr;
            mev.rms_db        = pt.rms_db;
            mev.f0_bin        = pt.f0_bin;
            mev.seq           = pt.seq;
            mev.vbat_mv       = dev->last_vbat_mv;
            mev.fw_major      = pt.fw_major;
            mev.fw_minor      = pt.fw_minor;
            mev.fw_patch      = pt.fw_patch;
            mev.uptime_min    = pt.uptime_min;
            mev.free_heap_kb  = pt.free_heap_kb;
            mev.tx_fail_count = pt.tx_fail_count;
            mev.flags         = pt.flags;
            if (xQueueSend(g_mqtt_event_queue, &mev, 0) != pdTRUE) {
                ESP_LOGW(TAG, "MQTT queue full — event dropped");
            }
        }
    }
}
