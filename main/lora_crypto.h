/*
 * lora_crypto.h — AES-128-GCM encrypted LoRa packet protocol
 *
 * Uses PSA Crypto API (mbedtls 4.x / ESP-IDF 6.x).
 *
 * Packet wire format v2 (38 bytes total):
 *   [6B cleartext header] [32B AEAD output = 16B ciphertext + 16B GCM tag]
 *
 * Header:
 *   [1B ver_ttl]   // high nibble = frame version (2), low nibble = mesh TTL
 *   [1B orig_id]   // originating node's device_id
 *   [2B boot_id]   // per-node boot counter, persisted in NVS, LE
 *   [2B seq]       // per-boot packet counter, LE
 *
 * (orig_id, boot_id, seq) is unique per packet. It forms the 12-byte GCM IV,
 * the receiver's replay key and the mesh dedup key. The header is
 * authenticated as AAD with the TTL nibble masked out, so relays can
 * decrement TTL without invalidating the tag.
 *
 * Plaintext (16 bytes):
 *   [2B seq] [1B device_id] [1B event_type] [1B f0_bin] [1B rms_db]
 *   [1B vbat_cv]          // vbat_V = 2.50 + vbat_cv * 0.01
 *   [1B fw_major] [1B fw_minor] [1B fw_patch]
 *   [2B uptime_min]       // clamped to 65535
 *   [2B free_heap_kb]     // esp_get_free_heap_size() / 1024
 *   [1B tx_fail_count]    // monotonic, clamped to 255
 *   [1B flags]            // bit0=low_batt, bit1=usb_present (reserved)
 *
 * event_type values:
 *   0x00 CLEAR      — state transition ALARM → SAFE
 *   0x01 ALARM      — state transition SAFE  → ALARM
 *   0x02 TELEMETRY  — periodic heartbeat (f0_bin / rms_db are unused, zero)
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

#define LORA_FRAME_VERSION   2
#define LORA_HEADER_LEN      6
#define LORA_PLAINTEXT_LEN   16
#define LORA_GCM_TAG_LEN    16
#define LORA_AEAD_OUT_LEN   (LORA_PLAINTEXT_LEN + LORA_GCM_TAG_LEN)
#define LORA_PACKET_LEN     (LORA_HEADER_LEN + LORA_AEAD_OUT_LEN)
#define LORA_PACKET_LEN_V1  36   /* pre-v3.0 frame, rejected with a hint */
#define LORA_GCM_IV_LEN     12

#define LORA_TTL_DEFAULT     3
#define LORA_TTL_MASK        0x0F

#define LORA_EVENT_CLEAR      0x00
#define LORA_EVENT_ALARM      0x01
#define LORA_EVENT_TELEMETRY  0x02

#define LORA_FLAG_LOW_BATT    0x01
#define LORA_FLAG_USB_PRESENT 0x02

/* Battery voltage encoding: V = 2.50 + vbat_cv * 0.01  (range 2.50..5.05 V) */
#define LORA_VBAT_OFFSET_MV   2500
#define LORA_VBAT_STEP_MV       10

typedef struct __attribute__((packed)) {
    uint16_t seq;
    uint8_t  device_id;
    uint8_t  event_type;
    uint8_t  f0_bin;
    uint8_t  rms_db;
    uint8_t  vbat_cv;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint16_t uptime_min;
    uint16_t free_heap_kb;
    uint8_t  tx_fail_count;
    uint8_t  flags;
} lora_plaintext_t;

_Static_assert(sizeof(lora_plaintext_t) == LORA_PLAINTEXT_LEN, "plaintext size mismatch");

typedef struct __attribute__((packed)) {
    uint8_t  ver_ttl;
    uint8_t  orig_id;
    uint16_t boot_id;
    uint16_t seq;
} lora_header_t;

_Static_assert(sizeof(lora_header_t) == LORA_HEADER_LEN, "header size mismatch");

typedef struct __attribute__((packed)) {
    lora_header_t hdr;
    uint8_t aead[LORA_AEAD_OUT_LEN];
} lora_packet_t;

_Static_assert(sizeof(lora_packet_t) == LORA_PACKET_LEN, "packet size mismatch");

static inline uint8_t lora_hdr_version(const lora_header_t *h) { return h->ver_ttl >> 4; }
static inline uint8_t lora_hdr_ttl(const lora_header_t *h)     { return h->ver_ttl & LORA_TTL_MASK; }

static inline void lora_hdr_init(lora_header_t *h, uint8_t orig_id,
                                 uint16_t boot_id, uint16_t seq, uint8_t ttl)
{
    h->ver_ttl = (uint8_t)((LORA_FRAME_VERSION << 4) | (ttl & LORA_TTL_MASK));
    h->orig_id = orig_id;
    h->boot_id = boot_id;
    h->seq     = seq;
}

/* IV = [ver][orig_id][boot_id LE][seq LE][6B zero]; unique per packet as long
 * as boot_id never repeats for a given orig_id under the same key. */
static inline void lora_build_iv(const lora_header_t *h, uint8_t iv_out[LORA_GCM_IV_LEN])
{
    memset(iv_out, 0, LORA_GCM_IV_LEN);
    iv_out[0] = LORA_FRAME_VERSION;
    iv_out[1] = h->orig_id;
    memcpy(iv_out + 2, &h->boot_id, 2);
    memcpy(iv_out + 4, &h->seq, 2);
}

/* AAD = header with the TTL nibble cleared (relays rewrite TTL in flight). */
static inline void lora_build_aad(const lora_header_t *h, uint8_t aad_out[LORA_HEADER_LEN])
{
    lora_header_t a = *h;
    a.ver_ttl &= (uint8_t)~LORA_TTL_MASK;
    memcpy(aad_out, &a, LORA_HEADER_LEN);
}

static inline bool lora_encrypt(const uint8_t key[16],
                                const lora_header_t *hdr,
                                const lora_plaintext_t *pt,
                                lora_packet_t *pkt)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) return false;

    pkt->hdr = *hdr;

    uint8_t iv[LORA_GCM_IV_LEN];
    uint8_t aad[LORA_HEADER_LEN];
    lora_build_iv(hdr, iv);
    lora_build_aad(hdr, aad);

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
                               aad, LORA_HEADER_LEN,
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

    if (lora_hdr_version(&pkt->hdr) != LORA_FRAME_VERSION) return false;

    uint8_t iv[LORA_GCM_IV_LEN];
    uint8_t aad[LORA_HEADER_LEN];
    lora_build_iv(&pkt->hdr, iv);
    lora_build_aad(&pkt->hdr, aad);

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
                               aad, LORA_HEADER_LEN,
                               pkt->aead, LORA_AEAD_OUT_LEN,
                               pt_out_bytes, LORA_PLAINTEXT_LEN,
                               &out_len);
    psa_destroy_key(kid);
    if (status != PSA_SUCCESS) return false;

    /* The plaintext repeats orig_id/seq; they must match the authenticated header. */
    return pt_out->device_id == pkt->hdr.orig_id && pt_out->seq == pkt->hdr.seq;
}

static inline uint8_t lora_vbat_encode(uint32_t mv)
{
    if (mv < LORA_VBAT_OFFSET_MV) return 0;
    uint32_t v = (mv - LORA_VBAT_OFFSET_MV + (LORA_VBAT_STEP_MV / 2)) / LORA_VBAT_STEP_MV;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static inline uint32_t lora_vbat_decode_mv(uint8_t vbat_cv)
{
    return LORA_VBAT_OFFSET_MV + (uint32_t)vbat_cv * LORA_VBAT_STEP_MV;
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
