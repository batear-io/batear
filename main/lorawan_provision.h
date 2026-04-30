/*
 * lorawan_provision.h — LoRa key provisioning from NVS
 *
 * Reads dev_eui (8B) and app_key (16B) from NVS namespace "lora_cfg".
 * If keys are missing, falls back to a MAC-derived DevEUI and (for the
 * Detector / Gateway roles only) the compile-time BATEAR_NET_KEY.
 *
 * The app_key from NVS serves as the AES-128 network key for the
 * detector↔gateway encrypted LoRa link, replacing the hardcoded
 * BATEAR_NET_KEY when present.
 *
 * Wired Detector role does not transmit over LoRa, so app_key is
 * unused in that build — the field stays in the struct (kept zero
 * unless previously provisioned) purely for layout compatibility.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORAWAN_DEV_EUI_LEN  8
#define LORAWAN_APP_KEY_LEN  16

typedef struct {
    uint8_t  dev_eui[LORAWAN_DEV_EUI_LEN];
    uint8_t  app_key[LORAWAN_APP_KEY_LEN];
    uint8_t  device_id;
    uint32_t lora_freq_khz;
    uint8_t  lora_sync_word;
    bool     from_nvs;
    bool     device_id_from_nvs;
    bool     lora_freq_from_nvs;
    bool     sync_word_from_nvs;
} lorawan_keys_t;

/**
 * Read keys from NVS; fall back to compile-time defaults if absent.
 * Call once from app_main() after nvs_flash_init().
 */
esp_err_t lorawan_provision_init(void);

/** Return pointer to the active key set (valid after _init). */
const lorawan_keys_t *lorawan_get_keys(void);

/** Log DevEUI and AppKey at INFO level. */
void lorawan_log_keys(const char *tag);

#ifdef __cplusplus
}
#endif
