/*
 * manual_capture.h — BOOT (GPIO 0) long-press / short-press handler.
 *
 * Long-press ≥ CONFIG_BATEAR_TF_MANUAL_HOLD_MS while idle  → TF_CMD_MANUAL_START.
 * Short-press (release within 400 ms) while a manual recording is in progress
 *                                                           → TF_CMD_MANUAL_STOP.
 *
 * GPIO 0 is the ESP32-S3 strap pin; this module configures it as INPUT only
 * and only AFTER Ethernet is up, so the ROM bootloader handoff during
 * `idf.py flash` is unaffected.
 *
 * Compiled out when BATEAR_TF_MANUAL_ENABLE=n. Safe no-op when the parent
 * BATEAR_TF_RECORD_ENABLE is also off.
 */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BATEAR_TF_RECORD_ENABLE && CONFIG_BATEAR_TF_MANUAL_ENABLE

/** Configure GPIO 0 as input and start the polling task. Idempotent. */
void manual_capture_init(void);

#else

static inline void manual_capture_init(void) { }

#endif

#ifdef __cplusplus
}
#endif
