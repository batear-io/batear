/*
 * tf_recorder.h — microSD audio capture for the Wired Detector role
 *
 * Captures raw 16-bit mono PCM at 16 kHz to /sdcard/rec/<wired_id>/ as .wav.
 * Decoupled from AudioTask via an internal ringbuffer so SD writes
 * (which can stall >100 ms on FAT cluster allocation) never block the
 * 100 ms I2S hop.
 *
 * Three trigger sources, all going through the same WAV pipeline:
 *   - DRONE_EVENT_ALARM  → CMD_ALARM_START   (auto)
 *   - BOOT long-press    → CMD_MANUAL_START  (operator)
 *   - 60 s rotation tick → CMD_ALWAYS_TICK   (debug, opt-in)
 *
 * Compiled out entirely when CONFIG_BATEAR_TF_RECORD_ENABLE is unset.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TF_CMD_ALARM_START   = 1,  /* DRONE_EVENT_ALARM transition */
    TF_CMD_ALARM_CLEAR   = 2,  /* DRONE_EVENT_CLEAR — start post-roll countdown */
    TF_CMD_MANUAL_START  = 3,  /* BOOT long-press */
    TF_CMD_MANUAL_STOP   = 4,  /* BOOT short-press during manual recording */
} TfRecorderCmd;

typedef struct {
    bool      mounted;          /* SD mounted successfully                */
    bool      recording;        /* currently writing a WAV file           */
    uint32_t  used_mb;          /* total bytes in /sdcard/rec/<id> / MiB  */
    uint32_t  free_mb;          /* card free space (MiB)                  */
    uint32_t  total_mb;         /* card total capacity (MiB)              */
    uint32_t  files;            /* number of WAV files currently on disk  */
    uint32_t  drops;            /* AudioTask ringbuffer overruns          */
    char      last_file[64];    /* basename of the most recent recording  */
} TfRecorderStats;

#if CONFIG_BATEAR_TF_RECORD_ENABLE

/**
 * Initialise SDMMC + FATFS, allocate PSRAM pre-roll ring, start writer task.
 * `wired_id` is used as the per-device subdirectory under /sdcard/rec.
 * Returns ESP_OK on success. On failure (no card, alloc failure, …) the
 * recorder logs a single warning and all subsequent calls are no-ops, but
 * AudioTask / EthMqttTask continue running normally.
 */
esp_err_t tf_recorder_init(const char *wired_id);

/**
 * Push one frame of int16 mono PCM @ 16 kHz from AudioTask.
 * Non-blocking: drops + increments stats.drops if the ring is full.
 * Safe to call before init (no-op).
 */
void tf_recorder_push_pcm(const int16_t *pcm, size_t n_samples);

/** Send a state-machine command from EthMqttTask / manual capture task. */
void tf_recorder_send_cmd(TfRecorderCmd cmd);

/** Snapshot stats (mutex-protected, safe from any task). */
void tf_recorder_get_stats(TfRecorderStats *out);

/**
 * Resolve `name` (a recording basename) to an absolute path under
 * /sdcard/rec/<wired_id>/. Validates `name` against a strict whitelist
 * (alphanumerics + `_.-`, no slashes, no leading dot, no `..`).
 * Returns true on success; false on whitelist violation OR if the recorder
 * isn't mounted.
 */
bool tf_recorder_resolve_path(const char *name, char *out, size_t out_sz);

/** Recording directory for opendir() in REST handlers. NULL if not mounted. */
const char *tf_recorder_dir(void);

/** True after a successful mount; false otherwise (REST returns 503). */
bool tf_recorder_is_ready(void);

#else /* !CONFIG_BATEAR_TF_RECORD_ENABLE — feature compiled out */

static inline esp_err_t tf_recorder_init(const char *wired_id)
{
    (void)wired_id;
    return ESP_OK;
}
static inline void tf_recorder_push_pcm(const int16_t *pcm, size_t n)
{
    (void)pcm;
    (void)n;
}
static inline void tf_recorder_send_cmd(TfRecorderCmd cmd) { (void)cmd; }
static inline void tf_recorder_get_stats(TfRecorderStats *out)
{
    if (out) {
        *out = (TfRecorderStats){0};
    }
}
static inline bool tf_recorder_resolve_path(const char *name, char *out, size_t sz)
{
    (void)name;
    (void)out;
    (void)sz;
    return false;
}
static inline const char *tf_recorder_dir(void) { return NULL; }
static inline bool tf_recorder_is_ready(void) { return false; }

#endif /* CONFIG_BATEAR_TF_RECORD_ENABLE */

#ifdef __cplusplus
}
#endif
