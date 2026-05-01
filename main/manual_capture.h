/*
 * manual_capture.h — BOOT (GPIO 0) push-to-talk handler.
 *
 * Press   → TF_CMD_MANUAL_START.
 * Release → TF_CMD_MANUAL_STOP.
 *
 * GPIO 0 is the ESP32-S3 strap pin; this module configures it as INPUT only
 * and only AFTER Ethernet is up, so the ROM bootloader handoff during
 * `idf.py flash` is unaffected.
 *
 * Compiled out when BATEAR_TF_MANUAL_ENABLE=n, or when the role isn't
 * BATEAR_ROLE_WIRED_DETECTOR (the only board with an SD slot).
 */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BATEAR_ROLE_WIRED_DETECTOR && CONFIG_BATEAR_TF_MANUAL_ENABLE

/** Configure GPIO 0 as input and start the polling task. Idempotent. */
void manual_capture_init(void);

#else

/* Stub for builds without the recorder; the only call site is gated on
 * CONFIG_BATEAR_ROLE_WIRED_DETECTOR && CONFIG_BATEAR_TF_MANUAL_ENABLE. */
// cppcheck-suppress unusedFunction
static inline void manual_capture_init(void) { }

#endif

#ifdef __cplusplus
}
#endif
