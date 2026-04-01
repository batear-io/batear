/*
 * lora_crypto.h — AES-128-GCM encrypted LoRa packet protocol
 *
 * Uses PSA Crypto API (mbedtls 4.x / ESP-IDF 6.x).
 *
 * Packet wire format (28 bytes total):
 *   [4B nonce] [24B AEAD output = 8B ciphertext + 16B GCM tag]
 *
 * Plaintext (8 bytes):
 *   [2B seq_counter] [1B device_id] [1B event_type]
 *   [1B f0_bin] [1B rms_db] [2B reserved]
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "psa/crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_PLAINTEXT_LEN   8
#define LORA_NONCE_LEN       4
#define LORA_GCM_TAG_LEN    16
#define LORA_AEAD_OUT_LEN   (LORA_PLAINTEXT_LEN + LORA_GCM_TAG_LEN)
#define LORA_PACKET_LEN     (LORA_NONCE_LEN + LORA_AEAD_OUT_LEN)
#define LORA_GCM_IV_LEN     12

typedef struct __attribute__((packed)) {
    uint16_t seq;
    uint8_t  device_id;
    uint8_t  event_type;
    uint8_t  f0_bin;
    uint8_t  rms_db;
    uint8_t  reserved[2];
} lora_plaintext_t;

_Static_assert(sizeof(lora_plaintext_t) == LORA_PLAINTEXT_LEN, "plaintext size mismatch");

typedef struct __attribute__((packed)) {
    uint8_t nonce[LORA_NONCE_LEN];
    uint8_t aead[LORA_AEAD_OUT_LEN];
} lora_packet_t;

_Static_assert(sizeof(lora_packet_t) == LORA_PACKET_LEN, "packet size mismatch");

static inline void lora_build_iv(const uint8_t key[16],
                                 const uint8_t nonce[LORA_NONCE_LEN],
                                 uint8_t iv_out[LORA_GCM_IV_LEN])
{
    memcpy(iv_out, key, 8);
    memcpy(iv_out + 8, nonce, LORA_NONCE_LEN);
}

static inline bool lora_encrypt(const uint8_t key[16],
                                uint32_t counter,
                                const lora_plaintext_t *pt,
                                lora_packet_t *pkt)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) return false;

    memcpy(pkt->nonce, &counter, LORA_NONCE_LEN);

    uint8_t iv[LORA_GCM_IV_LEN];
    lora_build_iv(key, pkt->nonce, iv);

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);

    psa_key_id_t kid;
    status = psa_import_key(&attr, key, 16, &kid);
    if (status != PSA_SUCCESS) return false;

    size_t out_len = 0;
    // cppcheck-suppress cstyleCast
    const uint8_t *pt_bytes = (const uint8_t *)pt;
    status = psa_aead_encrypt(kid, PSA_ALG_GCM,
                               iv, LORA_GCM_IV_LEN,
                               NULL, 0,
                               pt_bytes, LORA_PLAINTEXT_LEN,
                               pkt->aead, LORA_AEAD_OUT_LEN,
                               &out_len);
    psa_destroy_key(kid);
    return status == PSA_SUCCESS && out_len == LORA_AEAD_OUT_LEN;
}

static inline bool lora_decrypt(const uint8_t key[16],
                                const lora_packet_t *pkt,
                                lora_plaintext_t *pt_out)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) return false;

    uint8_t iv[LORA_GCM_IV_LEN];
    lora_build_iv(key, pkt->nonce, iv);

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);

    psa_key_id_t kid;
    status = psa_import_key(&attr, key, 16, &kid);
    if (status != PSA_SUCCESS) return false;

    size_t out_len = 0;
    // cppcheck-suppress cstyleCast
    uint8_t *pt_out_bytes = (uint8_t *)pt_out;
    status = psa_aead_decrypt(kid, PSA_ALG_GCM,
                               iv, LORA_GCM_IV_LEN,
                               NULL, 0,
                               pkt->aead, LORA_AEAD_OUT_LEN,
                               pt_out_bytes, LORA_PLAINTEXT_LEN,
                               &out_len);
    psa_destroy_key(kid);
    return status == PSA_SUCCESS;
}

static inline uint8_t lora_rms_to_db(float rms)
{
    if (rms <= 0.0f) return 0;
    float db = 20.0f * log10f(rms);
    int val = (int)(db + 96.0f);
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (uint8_t)val;
}

#ifdef __cplusplus
}
#endif
