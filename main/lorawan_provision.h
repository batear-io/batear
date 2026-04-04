/*
 * lorawan_provision.h — LoRa key provisioning from NVS
 *
 * Reads dev_eui (8B) and app_key (16B) from NVS namespace "lora_cfg".
 * If keys are missing, falls back to compile-time BATEAR_NET_KEY and
 * a MAC-derived DevEUI.
 *
 * The app_key from NVS serves as the AES-128 network key for the
 * detector↔gateway encrypted LoRa link, replacing the hardcoded
 * BATEAR_NET_KEY when present.
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
    uint8_t dev_eui[LORAWAN_DEV_EUI_LEN];
    uint8_t app_key[LORAWAN_APP_KEY_LEN];
    bool    from_nvs;
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
