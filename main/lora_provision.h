/*
 * lora_provision.h — LoRa key provisioning from NVS
 *
 * Reads dev_eui (8B) and the network key (16B, NVS key "app_key" for
 * historical reasons) from NVS namespace "lora_cfg".
 * If keys are missing, falls back to a MAC-derived DevEUI and (for the
 * Detector / Gateway roles only) the compile-time BATEAR_NET_KEY.
 *
 * The network key from NVS is the AES-128 key for the
 * detector↔gateway encrypted LoRa link, replacing the hardcoded
 * BATEAR_NET_KEY when present.
 *
 * Wired Detector role does not transmit over LoRa, so net_key is
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

#define LORA_DEV_EUI_LEN  8
#define LORA_NET_KEY_LEN  16

typedef struct {
    uint8_t  dev_eui[LORA_DEV_EUI_LEN];
    uint8_t  net_key[LORA_NET_KEY_LEN];
    uint8_t  device_id;
    uint32_t lora_freq_khz;
    uint8_t  lora_sync_word;
    bool     from_nvs;
    bool     device_id_from_nvs;
    bool     lora_freq_from_nvs;
    bool     sync_word_from_nvs;
} lora_keys_t;

/**
 * Read keys from NVS; fall back to compile-time defaults if absent.
 * Call once from app_main() after nvs_flash_init().
 */
esp_err_t lora_provision_init(void);

/** Return pointer to the active key set (valid after _init). */
const lora_keys_t *lora_get_keys(void);

/** Log DevEUI and network key at INFO level. */
void lora_log_keys(const char *tag);

/**
 * Increment and persist the per-node boot counter used in the LoRa frame
 * header (and thus the GCM IV). Call once per boot, and again whenever the
 * per-boot seq counter would wrap.
 */
uint16_t lora_next_boot_id(void);

#ifdef __cplusplus
}
#endif
