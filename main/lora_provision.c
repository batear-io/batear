/*
 * lora_provision.c — NVS-backed LoRa key provisioning
 */

#include "lora_provision.h"
#include "pin_config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "provision";

#define NVS_NAMESPACE   "lora_cfg"
#define NVS_KEY_DEV_EUI   "dev_eui"
#define NVS_KEY_NET_KEY   "app_key"   /* historical NVS key name, kept for compatibility */
#define NVS_KEY_DEVICE_ID "device_id"
#define NVS_KEY_LORA_FREQ "lora_freq"
#define NVS_KEY_SYNC_WORD "sync_word"
#define NVS_KEY_BOOT_ID   "boot_id"

static lora_keys_t s_keys;
static bool s_initialised;

/* Expand 6-byte MAC → 8-byte EUI-64 (insert 0xFFFE after OUI). */
static void mac_to_eui64(const uint8_t mac[6], uint8_t eui[8])
{
    eui[0] = mac[0]; eui[1] = mac[1]; eui[2] = mac[2];
    eui[3] = 0xFF;   eui[4] = 0xFE;
    eui[5] = mac[3]; eui[6] = mac[4]; eui[7] = mac[5];
}

esp_err_t lora_provision_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }

    bool got_eui = false, got_key = false, got_devid = false;
    bool got_freq = false, got_sw = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = LORA_DEV_EUI_LEN;
        if (nvs_get_blob(h, NVS_KEY_DEV_EUI, s_keys.dev_eui, &len) == ESP_OK
            && len == LORA_DEV_EUI_LEN) {
            got_eui = true;
        }

        len = LORA_NET_KEY_LEN;
        if (nvs_get_blob(h, NVS_KEY_NET_KEY, s_keys.net_key, &len) == ESP_OK
            && len == LORA_NET_KEY_LEN) {
            got_key = true;
        }

        uint8_t devid;
        if (nvs_get_u8(h, NVS_KEY_DEVICE_ID, &devid) == ESP_OK) {
            s_keys.device_id = devid;
            got_devid = true;
        }

        uint32_t freq;
        if (nvs_get_u32(h, NVS_KEY_LORA_FREQ, &freq) == ESP_OK) {
            s_keys.lora_freq_khz = freq;
            got_freq = true;
        }

        uint8_t sw;
        if (nvs_get_u8(h, NVS_KEY_SYNC_WORD, &sw) == ESP_OK) {
            s_keys.lora_sync_word = sw;
            got_sw = true;
        }

        nvs_close(h);
    }

    /* DevEUI fallback: derive from factory MAC */
    if (!got_eui) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        mac_to_eui64(mac, s_keys.dev_eui);
        ESP_LOGI(TAG, "DevEUI derived from MAC (not in NVS)");
    }

    /* Network key fallback: compile-time BATEAR_NET_KEY (LoRa roles only).
     * Wired Detector never reads s_keys.net_key, so we leave it zeroed
     * to make accidental misuse obvious in logs / packet dumps. */
    if (!got_key) {
#ifdef CONFIG_BATEAR_NET_KEY
        static const uint8_t fallback[16] = BATEAR_NET_KEY;
        memcpy(s_keys.net_key, fallback, 16);
        ESP_LOGW(TAG,
                 "No LoRa keys found in NVS. "
                 "Using default key — provision via Batear Web Flasher "
                 "for a unique network key.");
#else
        memset(s_keys.net_key, 0, sizeof(s_keys.net_key));
#endif
    }

    /* device_id fallback: compile-time CONFIG_BATEAR_DEVICE_ID */
    if (!got_devid) {
#ifdef CONFIG_BATEAR_DEVICE_ID
        s_keys.device_id = (uint8_t)CONFIG_BATEAR_DEVICE_ID;
#else
        s_keys.device_id = 0;
#endif
    }

    if (!got_freq) {
#ifdef CONFIG_BATEAR_LORA_FREQ
        s_keys.lora_freq_khz = (uint32_t)CONFIG_BATEAR_LORA_FREQ;
#else
        s_keys.lora_freq_khz = 915000;
#endif
    }
    if (!got_sw) {
#ifdef CONFIG_BATEAR_LORA_SYNC_WORD
        s_keys.lora_sync_word = (uint8_t)CONFIG_BATEAR_LORA_SYNC_WORD;
#else
        s_keys.lora_sync_word = 0x12;
#endif
    }

    s_keys.from_nvs = got_eui && got_key;
    s_keys.device_id_from_nvs = got_devid;
    s_keys.lora_freq_from_nvs = got_freq;
    s_keys.sync_word_from_nvs = got_sw;
    s_initialised = true;
    return ESP_OK;
}

const lora_keys_t *lora_get_keys(void)
{
    return &s_keys;
}

void lora_log_keys(const char *tag)
{
    const lora_keys_t *k = &s_keys;

    ESP_LOGI(tag, "DevEUI: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X%s",
             k->dev_eui[0], k->dev_eui[1], k->dev_eui[2], k->dev_eui[3],
             k->dev_eui[4], k->dev_eui[5], k->dev_eui[6], k->dev_eui[7],
             k->from_nvs ? " [NVS]" : " [MAC]");

#ifdef CONFIG_BATEAR_NET_KEY
    ESP_LOGI(tag, "NetKey: %02X%02X%02X%02X%02X%02X%02X%02X"
                  "%02X%02X%02X%02X%02X%02X%02X%02X%s",
             k->net_key[0],  k->net_key[1],  k->net_key[2],  k->net_key[3],
             k->net_key[4],  k->net_key[5],  k->net_key[6],  k->net_key[7],
             k->net_key[8],  k->net_key[9],  k->net_key[10], k->net_key[11],
             k->net_key[12], k->net_key[13], k->net_key[14], k->net_key[15],
             k->from_nvs ? " [NVS]" : " [DEFAULT]");
#else
    ESP_LOGI(tag, "NetKey: (not used in this role)");
#endif

    ESP_LOGI(tag, "DeviceID: %u (0x%02X)%s",
             k->device_id, k->device_id,
             k->device_id_from_nvs ? " [NVS]" : " [DEFAULT]");

#if defined(CONFIG_BATEAR_LORA_FREQ) || defined(CONFIG_BATEAR_LORA_SYNC_WORD)
    ESP_LOGI(tag, "LoRa freq: %lu kHz  sync_word: 0x%02X%s%s",
             (unsigned long)k->lora_freq_khz, k->lora_sync_word,
             k->lora_freq_from_nvs ? " freq[NVS]" : " freq[DEFAULT]",
             k->sync_word_from_nvs ? " sw[NVS]" : " sw[DEFAULT]");
#endif
}

uint16_t lora_next_boot_id(void)
{
    nvs_handle_t h;
    uint16_t id;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        id = (uint16_t)esp_random();
        ESP_LOGE(TAG, "boot_id: nvs_open failed (%s) — using random %u",
                 esp_err_to_name(err), id);
        return id;
    }

    /* Missing (first boot or erased NVS): start at a random point rather than
     * 0 so an NVS wipe is unlikely to replay IVs from earlier boots. */
    if (nvs_get_u16(h, NVS_KEY_BOOT_ID, &id) == ESP_OK) {
        id++;
    } else {
        id = (uint16_t)esp_random();
    }

    err = nvs_set_u16(h, NVS_KEY_BOOT_ID, id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        id = (uint16_t)esp_random();
        ESP_LOGE(TAG, "boot_id: persist failed (%s) — using random %u",
                 esp_err_to_name(err), id);
    }
    return id;
}
