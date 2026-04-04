/*
 * lorawan_provision.h — Zero-Config LoRaWAN provisioning from ESP32 MAC
 *
 * Derives unique DevEUI and AppKey from the factory-burned MAC address so
 * that every Batear node has globally-unique LoRaWAN identifiers without
 * manual configuration.
 *
 * DevEUI:  6-byte MAC → 8-byte EUI-64 by inserting 0xFFFE (IEEE standard).
 * AppKey:  16-byte key derived via salted XOR of MAC bytes.
 */
#pragma once

#include <stdint.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORAWAN_DEV_EUI_LEN  8
#define LORAWAN_APP_KEY_LEN  16

typedef struct {
    uint8_t dev_eui[LORAWAN_DEV_EUI_LEN];
    uint8_t app_key[LORAWAN_APP_KEY_LEN];
} lorawan_keys_t;

/*
 * Expand a 6-byte MAC to an 8-byte EUI-64 by inserting 0xFF 0xFE
 * between the OUI (first 3 bytes) and NIC (last 3 bytes).
 *
 *   MAC  AA:BB:CC:DD:EE:FF
 *   EUI  AA:BB:CC:FF:FE:DD:EE:FF
 */
static inline void lorawan_mac_to_eui64(const uint8_t mac[6],
                                        uint8_t eui64[LORAWAN_DEV_EUI_LEN])
{
    eui64[0] = mac[0];
    eui64[1] = mac[1];
    eui64[2] = mac[2];
    eui64[3] = 0xFF;
    eui64[4] = 0xFE;
    eui64[5] = mac[3];
    eui64[6] = mac[4];
    eui64[7] = mac[5];
}

/*
 * Derive a 16-byte AppKey by XOR-ing the MAC bytes with a fixed
 * project-specific salt.  This is NOT cryptographically strong but
 * guarantees per-device uniqueness on networks like TTN without
 * requiring manual key entry.
 */
static inline void lorawan_derive_app_key(const uint8_t mac[6],
                                          uint8_t app_key[LORAWAN_APP_KEY_LEN])
{
    static const uint8_t salt[LORAWAN_APP_KEY_LEN] = {
        'B','A','T','E','A','R','_','2',
        '0','2','6','_','D','R','O','N'
    };

    for (int i = 0; i < LORAWAN_APP_KEY_LEN; i++) {
        app_key[i] = mac[i % 6] ^ salt[i];
    }
}

/*
 * One-call provisioning: read the factory MAC and fill both DevEUI
 * and AppKey.  Returns ESP_OK on success.
 */
static inline esp_err_t lorawan_provision_keys(lorawan_keys_t *keys)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        return err;
    }

    lorawan_mac_to_eui64(mac, keys->dev_eui);
    lorawan_derive_app_key(mac, keys->app_key);
    return ESP_OK;
}

/*
 * Log the provisioned keys at INFO level.
 * Format matches TTN console expectations (MSB hex).
 */
static inline void lorawan_log_keys(const char *tag, const lorawan_keys_t *keys)
{
    ESP_LOGI(tag, "DevEUI: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             keys->dev_eui[0], keys->dev_eui[1], keys->dev_eui[2],
             keys->dev_eui[3], keys->dev_eui[4], keys->dev_eui[5],
             keys->dev_eui[6], keys->dev_eui[7]);

    ESP_LOGI(tag, "AppKey: %02X%02X%02X%02X%02X%02X%02X%02X"
                  "%02X%02X%02X%02X%02X%02X%02X%02X",
             keys->app_key[0],  keys->app_key[1],  keys->app_key[2],
             keys->app_key[3],  keys->app_key[4],  keys->app_key[5],
             keys->app_key[6],  keys->app_key[7],  keys->app_key[8],
             keys->app_key[9],  keys->app_key[10], keys->app_key[11],
             keys->app_key[12], keys->app_key[13], keys->app_key[14],
             keys->app_key[15]);
}

#ifdef __cplusplus
}
#endif
