/*
 * lora_radio.cpp — the one task that owns the SX1262 (Core 0)
 *
 * TX (node has a mic): blocks on g_drone_event_queue, with a jittered
 * heartbeat timeout; each event is encrypted and transmitted.
 * RX (node has rx):    the radio sits in continuous receive; DIO1 wakes
 * the task, which hands the frame to node_rx.
 *
 * Relay (node has relay): accepted packets from other nodes are queued in
 * mesh.c and re-transmitted with TTL - 1 after a slotted random backoff.
 *
 * The SX1262 is half-duplex, so a node with both pauses RX for its own TX
 * and resumes afterwards. A TX-only node parks the radio in deep sleep.
 *
 * Board: Heltec WiFi LoRa 32 V3 / V4 (ESP32-S3 + SX1262)
 */

#include "lora_radio.h"
#include "node_caps.h"
#include "node_rx.h"
#include "mesh.h"
#include "drone_detector.h"
#include "lora_crypto.h"
#include "lora_provision.h"
#include "battery.h"
#include "EspIdfHal.h"
#include "pin_config.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include <inttypes.h>
#include <cstdio>

#include <RadioLib.h>

static const char *TAG = "lora";

/* LoRa RF parameters (freq and sync_word come from NVS at runtime) */
#define LORA_BW_KHZ         125.0f
#define LORA_SF             10
#define LORA_CR             5
#define LORA_TX_DBM         22
#define LORA_TCXO_DELAY_MS  5

#define LORA_MAX_CONSECUTIVE_FAILS  3
#define LORA_MAX_RX_ERRORS          5
#define LORA_RX_BUF_LEN            64

/* One local retry for alarm/clear on a randomised 150–300 ms backoff. The
 * second attempt reuses the same seq + ciphertext so the gateway's replay
 * counter dedupes if the first packet actually made it over the air. */
#define LORA_EVENT_RETRIES          1
#define LORA_RETRY_BACKOFF_MIN_MS  150
#define LORA_RETRY_BACKOFF_MAX_MS  300

/* Low-battery flag threshold (LiPo knee) */
#define LORA_LOW_BATT_MV  3400

#ifndef CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN
#define CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN  30
#endif
#define LORA_HEARTBEAT_BASE_MS \
    ((uint32_t)CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN * 60U * 1000U)
/* ±10% uniform jitter so a fleet booted together desynchronises over time. */
#define LORA_HEARTBEAT_JITTER_MS  (LORA_HEARTBEAT_BASE_MS / 10U)

/* Returns a jittered heartbeat interval in the range
 *   [base - 10%, base + 10%]
 * using ESP32's HW RNG (cheap, non-blocking). */
static uint32_t heartbeat_next_ms(void)
{
    uint32_t span = 2U * LORA_HEARTBEAT_JITTER_MS + 1U;
    uint32_t offset = esp_random() % span;
    return LORA_HEARTBEAT_BASE_MS - LORA_HEARTBEAT_JITTER_MS + offset;
}

/* =========================================================================
 * RadioLib objects — heap-allocated so constructors don't run before
 * esp-idf initialises the SPI peripheral.
 * ====================================================================== */
static EspIdfHal *s_hal    = nullptr;
static Module    *s_module = nullptr;
static SX1262    *s_radio  = nullptr;

static bool s_tx_enabled = false;
static bool s_rx_enabled = false;
static bool s_relay_enabled = false;
static SemaphoreHandle_t s_rx_sem = nullptr;

static uint16_t s_boot_id = 0;
static uint16_t s_tx_seq = 0;
static uint32_t s_tx_fail_total = 0;   /* monotonic, clamped to 255 on the wire */
static uint32_t s_boot_ms = 0;

static uint8_t s_fw_major = 0;
static uint8_t s_fw_minor = 0;
static uint8_t s_fw_patch = 0;

/* Parse app version string (expected "vX.Y.Z" or "X.Y.Z" — git-describe output
 * may append "-gHASH" or "-dirty" which we ignore). Failure → 0.0.0. */
static void parse_fw_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc) return;

    const char *s = desc->version;
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;

    unsigned a = 0, b = 0, c = 0;
    if (sscanf(s, "%u.%u.%u", &a, &b, &c) >= 1) {
        s_fw_major = (a > 255) ? 255 : (uint8_t)a;
        s_fw_minor = (b > 255) ? 255 : (uint8_t)b;
        s_fw_patch = (c > 255) ? 255 : (uint8_t)c;
    }
    ESP_LOGI(TAG, "firmware version %u.%u.%u (from \"%s\")",
             s_fw_major, s_fw_minor, s_fw_patch, desc->version);
}

/* Fill every telemetry field in the plaintext. Called for every TX so that
 * alarm/clear packets piggyback the same diagnostics as heartbeat packets. */
static void fill_telemetry(lora_plaintext_t *pt)
{
    uint32_t vbat_mv = battery_read_mv();
    pt->vbat_cv = lora_vbat_encode(vbat_mv);

    pt->fw_major = s_fw_major;
    pt->fw_minor = s_fw_minor;
    pt->fw_patch = s_fw_patch;

    uint64_t now_ms  = (uint64_t)esp_timer_get_time() / 1000ULL;
    uint64_t up_ms   = (now_ms > s_boot_ms) ? (now_ms - s_boot_ms) : 0ULL;
    uint64_t up_min  = up_ms / 60000ULL;
    pt->uptime_min = (up_min > 0xFFFFULL) ? 0xFFFFU : (uint16_t)up_min;

    uint32_t heap_kb = esp_get_free_heap_size() / 1024U;
    pt->free_heap_kb = (heap_kb > 0xFFFFU) ? 0xFFFFU : (uint16_t)heap_kb;

    pt->tx_fail_count = (s_tx_fail_total > 255U) ? 255U : (uint8_t)s_tx_fail_total;

    uint8_t flags = 0;
    if (vbat_mv > 0 && vbat_mv < LORA_LOW_BATT_MV) {
        flags |= LORA_FLAG_LOW_BATT;
    }
    pt->flags = flags;
}

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

static bool lora_init(void)
{
    const lora_keys_t *keys = lora_get_keys();
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


static void IRAM_ATTR on_dio1(void)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_rx_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void lora_sleep(void)
{
    int16_t state = s_radio->sleep();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "sleep() failed: %d", state);
    }
}

/* Put the radio in its resting state: continuous RX, or deep sleep. */
static void radio_idle(void)
{
    if (!s_rx_enabled) {
        lora_sleep();
        return;
    }
    s_radio->setPacketReceivedAction(on_dio1);
    int16_t state = s_radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "startReceive() failed: %d", state);
    }
}

static bool lora_reinit(void)
{
    ESP_LOGW(TAG, "attempting radio reinit after consecutive failures");

    delete s_radio;  s_radio  = nullptr;
    delete s_module; s_module = nullptr;
    delete s_hal;    s_hal    = nullptr;

    vTaskDelay(pdMS_TO_TICKS(100));

    bool ok = lora_init();
    if (ok) {
        radio_idle();
    }
    return ok;
}

/*
 * lora_wake — leave sleep / RX for standby, then wait for the TCXO to
 * stabilise before we let RadioLib's transmit() fire. DIO1 is detached so
 * the TX-done interrupt isn't mistaken for a received packet.
 */
static bool lora_wake(void)
{
    if (s_rx_enabled) {
        s_radio->clearPacketReceivedAction();
    }
    int16_t state = s_radio->standby();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "standby() failed: %d", state);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(LORA_TCXO_DELAY_MS));
    return true;
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

static uint32_t uptime_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}

/* =========================================================================
 * TX path — encrypt and send one locally originated event
 * ====================================================================== */
static void send_event(const DroneEvent_t &ev, int &consecutive_fails)
{
    const bool is_heartbeat = (ev.type == DRONE_EVENT_TELEMETRY);

    if (is_heartbeat) {
        ESP_LOGI(TAG, "heartbeat tick");
    } else {
        ESP_LOGI(TAG, "event 0x%02X  f0bin=%d  conf=%.4f  rms=%.5f  t=%" PRIu32 "ms",
                 (unsigned)ev.type, ev.f0_bin,
                 ev.peak_ratio, ev.rms, ev.timestamp_ms);
    }

    /* (boot_id, seq) must never repeat: on seq wrap, start a new boot_id. */
    if (s_tx_seq == UINT16_MAX) {
        s_boot_id = lora_next_boot_id();
        s_tx_seq  = 0;
        ESP_LOGI(TAG, "seq wrapped — new boot_id=%u", (unsigned)s_boot_id);
    }

    /* Prepare the payload once — seq, plaintext and ciphertext are all
     * reused across the retry, so a successful over-air delivery that was
     * mis-reported locally is dedup'd by the gateway's replay counter. */
    lora_plaintext_t pt = {};
    pt.seq        = s_tx_seq++;
    pt.device_id  = lora_get_keys()->device_id;
    pt.event_type = static_cast<uint8_t>(ev.type);
    pt.f0_bin     = is_heartbeat ? 0 : static_cast<uint8_t>(ev.f0_bin);
    pt.rms_db     = is_heartbeat ? 0 : lora_rms_to_db(ev.rms);
    fill_telemetry(&pt);

    lora_header_t hdr;
    lora_hdr_init(&hdr, pt.device_id, s_boot_id, pt.seq, LORA_TTL_DEFAULT);

    /* A node that also listens can't hear its own TX — deliver it locally,
     * and remember it so relayed echoes of it are dropped as duplicates. */
    if (s_rx_enabled) {
        node_rx_local(&hdr, &pt);
        mesh_key_t key = { hdr.orig_id, hdr.boot_id, hdr.seq };
        mesh_record(&key);
    }

    lora_packet_t pkt;
    const uint8_t *net_key = lora_get_keys()->net_key;
    if (!lora_encrypt(net_key, &hdr, &pt, &pkt)) {
        /* Encrypt failures are software-level (PSA init, key import) —
         * retrying will not help and burns airtime budget. Bail early. */
        ESP_LOGE(TAG, "encrypt failed — dropping seq=%u", (unsigned)pt.seq);
        s_tx_fail_total++;
        consecutive_fails++;
        if (consecutive_fails >= LORA_MAX_CONSECUTIVE_FAILS) {
            if (lora_reinit()) {
                consecutive_fails = 0;
            }
        }
        return;
    }

    const int max_attempts = is_heartbeat ? 1 : (1 + LORA_EVENT_RETRIES);
    bool ok       = false;
    int  attempts = 0;

    for (int a = 0; a < max_attempts; a++) {
        if (a > 0) {
            uint32_t span    = LORA_RETRY_BACKOFF_MAX_MS - LORA_RETRY_BACKOFF_MIN_MS + 1U;
            uint32_t backoff = LORA_RETRY_BACKOFF_MIN_MS + (esp_random() % span);
            ESP_LOGW(TAG, "retry seq=%u attempt=%d after %ums",
                     (unsigned)pt.seq, a, (unsigned)backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
        }

        attempts++;

        if (!lora_wake()) {
            continue;  /* loop back to retry (if any attempts left) */
        }

        ok = lora_transmit(reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
        radio_idle();

        if (ok) break;
    }

    ESP_LOGI(TAG,
             "TX %s  boot=%u seq=%u  dev=%u  type=0x%02X  rms_db=%u  attempts=%d  "
             "vbat=%umV  fw=%u.%u.%u  up=%umin  heap=%ukB  txfail=%u",
             ok ? "OK" : "FAIL",
             (unsigned)s_boot_id, (unsigned)pt.seq, (unsigned)pt.device_id,
             (unsigned)pt.event_type, (unsigned)pt.rms_db, attempts,
             (unsigned)lora_vbat_decode_mv(pt.vbat_cv),
             (unsigned)pt.fw_major, (unsigned)pt.fw_minor, (unsigned)pt.fw_patch,
             (unsigned)pt.uptime_min, (unsigned)pt.free_heap_kb,
             (unsigned)pt.tx_fail_count);

    if (ok) {
        consecutive_fails = 0;
    } else {
        s_tx_fail_total++;
        consecutive_fails++;
        if (consecutive_fails >= LORA_MAX_CONSECUTIVE_FAILS) {
            if (lora_reinit()) {
                consecutive_fails = 0;
            }
        }
    }
}

/* =========================================================================
 * RX path — read one frame after DIO1 fired
 * ====================================================================== */
static void receive_frame(int &rx_errors)
{
    uint8_t buf[LORA_RX_BUF_LEN];
    size_t  len  = s_radio->getPacketLength();
    float   rssi = s_radio->getRSSI();
    float   snr  = s_radio->getSNR();

    int16_t state = RADIOLIB_ERR_PACKET_TOO_LONG;
    if (len <= sizeof(buf)) {
        state = s_radio->readData(buf, len);
    }
    /* Re-arm continuous RX whatever happened to this frame. */
    s_radio->startReceive();

    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "receive error: %d (len=%u)", state, (unsigned)len);
        node_rx_error();
        if (++rx_errors >= LORA_MAX_RX_ERRORS && lora_reinit()) {
            rx_errors = 0;
        }
        return;
    }

    rx_errors = 0;

    /* Copies of a packet we already accepted (direct, or via another relay)
     * are dropped before decryption. The ring only holds authenticated
     * keys, so a forged header can't make us drop a genuine packet. */
    const lora_header_t *hdr = reinterpret_cast<const lora_header_t *>(buf);
    const bool framed = (len == sizeof(lora_packet_t) &&
                         lora_hdr_version(hdr) == LORA_FRAME_VERSION);
    mesh_key_t key = {};
    if (framed) {
        key = { hdr->orig_id, hdr->boot_id, hdr->seq };
        if (mesh_is_dup(&key, lora_hdr_ttl(hdr))) {
            ESP_LOGD(TAG, "dup orig=0x%02X boot=%u seq=%u ttl=%u",
                     key.orig_id, key.boot_id, key.seq, lora_hdr_ttl(hdr));
            return;
        }
    }

    uint8_t event_type = 0;
    if (node_rx_packet(buf, len, rssi, snr, &event_type) != NODE_RX_OK) {
        return;
    }
    mesh_record(&key);

    /* Relay: other nodes' fresh, authenticated packets with TTL left. TTL is
     * outside the GCM AAD, so rewriting it keeps the tag valid. */
    uint8_t ttl = lora_hdr_ttl(hdr);
    if (!s_relay_enabled || ttl == 0 || key.orig_id == lora_get_keys()->device_id) {
        return;
    }
    buf[0] = (uint8_t)((buf[0] & ~LORA_TTL_MASK) | (ttl - 1));
    if (mesh_schedule(&key, buf, len, ttl - 1, event_type == LORA_EVENT_TELEMETRY,
                      uptime_ms(), esp_random())) {
        ESP_LOGI(TAG, "relay queued orig=0x%02X boot=%u seq=%u ttl=%u->%u in %ldms",
                 key.orig_id, key.boot_id, key.seq, ttl, ttl - 1,
                 (long)mesh_next_due_ms(uptime_ms()));
    } else {
        ESP_LOGW(TAG, "relay queue full — dropped orig=0x%02X seq=%u",
                 key.orig_id, key.seq);
    }
}

/* =========================================================================
 * Relay TX — send every queued relay that is due
 * ====================================================================== */
static void send_due_relays(void)
{
    uint8_t frame[MESH_FRAME_MAX];
    size_t  len;
    while ((len = mesh_pop_due(uptime_ms(), frame)) > 0) {
        const lora_header_t *hdr = reinterpret_cast<const lora_header_t *>(frame);
        bool ok = lora_wake() && lora_transmit(frame, len);
        radio_idle();
        ESP_LOGI(TAG, "RELAY %s orig=0x%02X boot=%u seq=%u ttl=%u",
                 ok ? "OK" : "FAIL", hdr->orig_id, hdr->boot_id, hdr->seq,
                 lora_hdr_ttl(hdr));
    }
}

/* =========================================================================
 * LoRaTask — entry point, pinned to Core 0 by xTaskCreatePinnedToCore()
 * ====================================================================== */
extern "C" void LoRaTask(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "LoRaTask start (core %d)", xPortGetCoreID());

    s_tx_enabled    = node_caps()->mic;
    s_rx_enabled    = node_caps()->rx;
    s_relay_enabled = node_caps()->relay;
    if (!s_tx_enabled && !s_rx_enabled) {
        ESP_LOGW(TAG, "neither mic nor rx enabled — radio unused, suspending");
        vTaskSuspend(NULL);
        return;
    }

    /* Wait on own events (TX) and DIO1 (RX) with one blocking call. A queue
     * must be empty when added to a set, so do this before anything slow —
     * AudioTask needs several frames of sustained detection to post. */
    QueueSetHandle_t set = xQueueCreateSet(8 + 1);
    if (s_tx_enabled && xQueueAddToSet(g_drone_event_queue, set) != pdPASS) {
        ESP_LOGE(TAG, "event queue not empty when added to set");
    }
    if (s_rx_enabled) {
        s_rx_sem = xSemaphoreCreateBinary();
        xQueueAddToSet(s_rx_sem, set);
    }

    s_boot_ms = (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    parse_fw_version();
    battery_init();

    if (s_rx_enabled) {
        node_rx_init();
    }

    s_boot_id = lora_next_boot_id();
    ESP_LOGI(TAG, "frame v%d  boot_id=%u  tx=%d rx=%d relay=%d", LORA_FRAME_VERSION,
             (unsigned)s_boot_id, s_tx_enabled, s_rx_enabled, s_relay_enabled);

    if (s_rx_enabled) {
        const mesh_config_t mcfg = {
            .slot_ms         = CONFIG_BATEAR_MESH_SLOT_MS,
            .event_slots     = CONFIG_BATEAR_MESH_EVENT_SLOTS,
            .heartbeat_slots = CONFIG_BATEAR_MESH_HEARTBEAT_SLOTS,
            .suppress_dups   = CONFIG_BATEAR_MESH_SUPPRESS_DUPS,
            .per_hour        = CONFIG_BATEAR_MESH_RELAY_PER_HOUR,
            .burst           = CONFIG_BATEAR_MESH_RELAY_BURST,
        };
        mesh_init(&mcfg, uptime_ms());
    }

    if (!lora_init()) {
        ESP_LOGE(TAG, "FATAL: radio init failed — suspending LoRaTask");
        if (s_rx_enabled) {
            node_rx_fatal("RADIO FAIL");
        }
        vTaskSuspend(NULL);
        return;
    }

    radio_idle();

    if (s_tx_enabled) {
        ESP_LOGI(TAG, "telemetry heartbeat every %u min (±10%% jitter)",
                 (unsigned)CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN);
    }

    int consecutive_fails = 0;
    int rx_errors = 0;
    TickType_t hb_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(heartbeat_next_ms());

    for (;;) {
        if (s_relay_enabled) {
            send_due_relays();
        }

        /*
         * Heartbeat deadline passed: synthesise a TELEMETRY packet so the
         * gateway (and HA) keep seeing fresh battery / uptime / heap numbers
         * in silent periods.
         */
        if (s_tx_enabled && (int32_t)(hb_deadline - xTaskGetTickCount()) <= 0) {
            DroneEvent_t hb = {};
            hb.type         = DRONE_EVENT_TELEMETRY;
            hb.timestamp_ms = uptime_ms();
            send_event(hb, consecutive_fails);
            hb_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(heartbeat_next_ms());
        }

        /* Sleep until an own event, a received frame, the heartbeat deadline
         * or the next relay — whichever comes first. */
        TickType_t wait = portMAX_DELAY;
        if (s_tx_enabled) {
            TickType_t now = xTaskGetTickCount();
            wait = ((int32_t)(hb_deadline - now) > 0) ? hb_deadline - now : 0;
        }
        if (s_relay_enabled) {
            int32_t relay_ms = mesh_next_due_ms(uptime_ms());
            if (relay_ms >= 0 && pdMS_TO_TICKS(relay_ms) < wait) {
                wait = pdMS_TO_TICKS(relay_ms);
            }
        }

        QueueSetMemberHandle_t ready = xQueueSelectFromSet(set, wait);

        if (s_rx_enabled && ready == s_rx_sem) {
            xSemaphoreTake(s_rx_sem, 0);
            receive_frame(rx_errors);
        } else if (s_tx_enabled && ready == g_drone_event_queue) {
            DroneEvent_t ev = {};
            xQueueReceive(g_drone_event_queue, &ev, 0);
            send_event(ev, consecutive_fails);
            hb_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(heartbeat_next_ms());
        }
    }
}
