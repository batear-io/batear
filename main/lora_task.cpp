/*
 * lora_task.cpp — SX1262 LoRa transmitter via RadioLib (Core 0)
 *
 * Blocks indefinitely on g_drone_event_queue.
 * On each event: wake SX1262 → transmit 1-byte payload → deep sleep.
 *
 * Board: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
 * Framework: ESP-IDF 5.x with RadioLib (idf_component.yml)
 */

#include "lora_task.h"
#include "drone_detector.h"
#include "lora_crypto.h"
#include "lorawan_provision.h"
#include "EspIdfHal.h"
#include "pin_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <inttypes.h>

#include <RadioLib.h>

static const char *TAG = "lora";

/* LoRa RF parameters (freq and sync_word come from NVS at runtime) */
#define LORA_BW_KHZ         125.0f
#define LORA_SF             10
#define LORA_CR             5
#define LORA_TX_DBM         22
#define LORA_TCXO_DELAY_MS  5

/* =========================================================================
 * RadioLib objects — heap-allocated so constructors don't run before
 * esp-idf initialises the SPI peripheral.
 * ====================================================================== */
static EspIdfHal *s_hal    = nullptr;
static Module    *s_module = nullptr;
static SX1262    *s_radio  = nullptr;

static uint16_t s_tx_seq = 0;

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

static bool lora_init(void)
{
    const lorawan_keys_t *keys = lorawan_get_keys();
    float freq_mhz = keys->lora_freq_khz / 1000.0f;
    uint8_t sync_word = keys->lora_sync_word;

    s_hal    = new EspIdfHal(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
    s_module = new Module(s_hal, PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
    s_radio  = new SX1262(s_module);

    int16_t state = s_radio->begin(
        freq_mhz,
        LORA_BW_KHZ,
        LORA_SF,
        LORA_CR,
        sync_word,
        LORA_TX_DBM
    );
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "begin() failed: %d", state);
        return false;
    }

    /*
     * TCXO: the Heltec V3 powers the SX1262 TCXO via DIO3 at 1.8 V.
     * Without this call the radio transmits but drifts badly under thermal load.
     */
    state = s_radio->setTCXO(BOARD_LORA_TCXO_V);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "setTCXO() failed: %d", state);
        return false;
    }

    state = s_radio->setDio2AsRfSwitch(BOARD_LORA_DIO2_AS_RF);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "setDio2AsRfSwitch() failed: %d", state);
        return false;
    }

    ESP_LOGI(TAG, "SX1262 ready  freq=%.1f MHz  SF=%d  BW=%.0f kHz  pwr=%d dBm  sw=0x%02X",
             freq_mhz, LORA_SF, LORA_BW_KHZ, LORA_TX_DBM, sync_word);
    return true;
}

/*
 * lora_wake — transition from deep sleep to standby, then wait for the
 * TCXO to stabilise before we let RadioLib's transmit() fire.
 *
 * SX1262 wakes automatically when NSS is asserted, but RadioLib's
 * standby() call issues an explicit STDBY_RC command and resets the
 * busy-wait timeout, giving a clean, deterministic start state.
 */
static void lora_wake(void)
{
    s_radio->standby();
    vTaskDelay(pdMS_TO_TICKS(LORA_TCXO_DELAY_MS));
}

static bool lora_transmit(const uint8_t *data, size_t len)
{
    int16_t state = s_radio->transmit(const_cast<uint8_t *>(data), len);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "transmit() failed: %d", state);
        return false;
    }
    return true;
}

static void lora_sleep(void)
{
    /*
     * SX1262 deep sleep draws ~0.9 µA vs ~5 mA in RX-continuous.
     * Always return to sleep immediately after TX to save power.
     */
    int16_t state = s_radio->sleep();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "sleep() failed: %d", state);
    }
}

/* =========================================================================
 * LoRaTask — entry point, pinned to Core 0 by xTaskCreatePinnedToCore()
 * ====================================================================== */
extern "C" void LoRaTask(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "LoRaTask start (core %d)", xPortGetCoreID());

    if (!lora_init()) {
        ESP_LOGE(TAG, "FATAL: radio init failed — suspending LoRaTask");
        vTaskSuspend(NULL);
        return;
    }

    lora_sleep(); /* park in deep sleep; wake only when a packet must go out */

    DroneEvent_t ev;
    for (;;) {
        /*
         * Block here with zero CPU consumption until AudioTask enqueues an
         * event.  portMAX_DELAY means LoRaTask never burns cycles polling.
         */
        if (xQueueReceive(g_drone_event_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "event 0x%02X  f0bin=%d  conf=%.4f  rms=%.5f  t=%" PRIu32 "ms",
                 (unsigned)ev.type, ev.f0_bin,
                 ev.peak_ratio, ev.rms, ev.timestamp_ms);

        lora_wake();

        lora_plaintext_t pt = {};
        pt.seq          = s_tx_seq++;
        pt.device_id    = lorawan_get_keys()->device_id;
        pt.event_type   = static_cast<uint8_t>(ev.type);
        pt.f0_bin       = static_cast<uint8_t>(ev.f0_bin);
        pt.rms_db       = lora_rms_to_db(ev.rms);

        lora_packet_t pkt;
        bool ok = false;
        const uint8_t *net_key = lorawan_get_keys()->app_key;
        if (lora_encrypt(net_key, pt.seq, &pt, &pkt)) {
            ok = lora_transmit(reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
        } else {
            ESP_LOGE(TAG, "encrypt failed");
        }

        ESP_LOGI(TAG, "TX %s  seq=%u  dev=%u  type=0x%02X  rms_db=%u  (%u bytes)",
                 ok ? "OK" : "FAIL",
                 (unsigned)pt.seq, (unsigned)pt.device_id,
                 (unsigned)pt.event_type, (unsigned)pt.rms_db,
                 (unsigned)sizeof(pkt));

        lora_sleep();
    }
}
