/*
 * ntp_time.h — SNTP wall-clock for TF recording filenames (Wired Detector)
 *
 * Started after Ethernet is up; falls back to a boot-relative timestamp
 * (`bootNNNNNNNNN`) until the first sync completes so recording filenames
 * are still unique even before NTP responds.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start SNTP. Safe to call once after Ethernet has an IP.
 *  server: NTP host name (NULL = use CONFIG_BATEAR_TF_NTP_SERVER). */
void ntp_time_start(const char *server);

/** True after the first successful SNTP sync. */
bool ntp_time_is_synced(void);

/**
 * Format a timestamp suitable for use as a filename component.
 *   synced  → "YYYYMMDD-HHMMSS"   (15 chars + NUL)
 *   pre-sync → "bootNNNNNNNNN"    (uptime ms, 13 chars + NUL)
 * `out` must hold at least 16 bytes. Returns out.
 */
char *ntp_time_format(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
