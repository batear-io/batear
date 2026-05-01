/*
 * tf_recorder.c — microSD audio capture pipeline (Wired Detector)
 *
 * Architecture:
 *
 *   AudioTask (Core 1, 100 ms hop)
 *      │
 *      │ tf_recorder_push_pcm(int16, 1024)   ← non-blocking, drops on overrun
 *      ▼
 *   xRingbuffer (16 KB internal SRAM, no-split)
 *      │
 *      ▼
 *   TfWriterTask (Core 0, low priority)
 *      ├─ Always copies new samples into the PSRAM pre-roll ring (~5 s).
 *      ├─ On CMD_ALARM_START / CMD_MANUAL_START:
 *      │     open new WAV, flush pre-roll, enter STATE_RECORDING.
 *      ├─ On CMD_ALARM_CLEAR: schedule stop after BATEAR_TF_POSTROLL_SEC.
 *      ├─ On CMD_MANUAL_STOP: stop immediately.
 *      ├─ On always-on debug: rotate every 60 s.
 *      └─ On disk full: FIFO-delete oldest WAV before opening a new one.
 *
 * SD writes can stall >100 ms during FAT cluster allocation; the audio
 * task's ringbuffer absorbs that without ever back-pressuring i2s_channel_read.
 *
 * Filesystem layout:
 *   /sdcard/rec/<wired_id>/<timestamp>_<seq>_<suffix>.wav
 *
 * WAV format: RIFF/WAVE, PCM, 16-bit signed, 16 kHz, mono. Header data_size
 * is patched on close (and refreshed periodically during recording so a
 * crash mid-recording still leaves a parseable file).
 */

#include "tf_recorder.h"

#if CONFIG_BATEAR_TF_RECORD_ENABLE

#include "ntp_time.h"
#include "pin_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "tf_rec";

#ifndef BOARD_HAS_SDMMC
#define BOARD_HAS_SDMMC 0
#endif

#define SAMPLE_RATE_HZ      16000
#define BYTES_PER_SAMPLE    2
#define MOUNT_POINT         "/sdcard"
#define REC_ROOT            MOUNT_POINT "/rec"

#define RING_FRAMES         8
#define FRAME_SAMPLES_MAX   1024
#define RING_SIZE_BYTES     (RING_FRAMES * FRAME_SAMPLES_MAX * BYTES_PER_SAMPLE)

#define WRITER_TASK_STACK   (6 * 1024)
#define WRITER_TASK_PRIO    2  /* below EthMqttTask (configMAX_PRIORITIES - 3) */

#define ALWAYS_SEGMENT_SEC  60
#define HEADER_REFRESH_SEC  10

#define WAV_HEADER_SIZE     44

#define DEVID_MAX           32
#define PATH_MAX_LEN        128
#define NAME_MAX_LEN        64

typedef enum {
    STATE_IDLE = 0,
    STATE_RECORDING_ALARM,
    STATE_RECORDING_MANUAL,
    STATE_RECORDING_ALWAYS,
} TfState;

typedef struct {
    TfRecorderCmd type;
} TfCmd_t;

/* ---- module state (single-instance, all access from TfWriterTask except
 *      where explicitly mutex-protected) ---- */
static bool             s_inited;
static bool             s_mounted;
static char             s_dev_id[DEVID_MAX];
static char             s_dir[PATH_MAX_LEN];
static sdmmc_card_t    *s_card;

static RingbufHandle_t  s_pcm_ring;
static QueueHandle_t    s_cmd_queue;
static SemaphoreHandle_t s_stats_mtx;

static int16_t         *s_preroll;            /* PSRAM, capacity samples */
static size_t           s_preroll_cap;        /* total samples           */
static size_t           s_preroll_head;       /* write index (oldest at head when full) */
static size_t           s_preroll_count;      /* valid samples (≤ cap)   */

/* Stats, mutex-protected so REST/MQTT can read concurrently. */
static TfRecorderStats  s_stats;
static volatile uint32_t s_drops_atomic;      /* incremented from AudioTask, snapshot under mutex */

/* ---- helpers ---- */

static void stats_lock(void)   { xSemaphoreTake(s_stats_mtx, portMAX_DELAY); }
static void stats_unlock(void) { xSemaphoreGive(s_stats_mtx); }

bool tf_recorder_is_ready(void) { return s_mounted; }

const char *tf_recorder_dir(void)
{
    return s_mounted ? s_dir : NULL;
}

/* Strict whitelist: [A-Za-z0-9_.-]+, max NAME_MAX_LEN-1, no leading dot,
 * no embedded "..", no leading/trailing whitespace. */
static bool name_is_safe(const char *name)
{
    if (!name || !*name) return false;
    if (name[0] == '.') return false;
    size_t len = strnlen(name, NAME_MAX_LEN);
    if (len >= NAME_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    if (strstr(name, "..") != NULL) return false;
    return true;
}

bool tf_recorder_resolve_path(const char *name, char *out, size_t out_sz)
{
    if (!s_mounted || !out || out_sz == 0) return false;
    if (!name_is_safe(name)) return false;
    int n = snprintf(out, out_sz, "%s/%s", s_dir, name);
    return (n > 0 && (size_t)n < out_sz);
}

/* ---- WAV header ---- */

static void wav_build_header(uint8_t hdr[WAV_HEADER_SIZE], uint32_t data_bytes)
{
    const uint32_t byte_rate    = SAMPLE_RATE_HZ * BYTES_PER_SAMPLE;
    const uint16_t block_align  = BYTES_PER_SAMPLE;
    const uint16_t bits_per_sam = 16;
    const uint32_t riff_size    = data_bytes + 36;

    memcpy(hdr + 0, "RIFF", 4);
    hdr[4]  = (uint8_t)(riff_size & 0xFF);
    hdr[5]  = (uint8_t)((riff_size >> 8) & 0xFF);
    hdr[6]  = (uint8_t)((riff_size >> 16) & 0xFF);
    hdr[7]  = (uint8_t)((riff_size >> 24) & 0xFF);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;   /* fmt chunk size */
    hdr[20] = 1;  hdr[21] = 0;                              /* PCM */
    hdr[22] = 1;  hdr[23] = 0;                              /* mono */
    hdr[24] = (uint8_t)(SAMPLE_RATE_HZ & 0xFF);
    hdr[25] = (uint8_t)((SAMPLE_RATE_HZ >> 8) & 0xFF);
    hdr[26] = (uint8_t)((SAMPLE_RATE_HZ >> 16) & 0xFF);
    hdr[27] = (uint8_t)((SAMPLE_RATE_HZ >> 24) & 0xFF);
    hdr[28] = (uint8_t)(byte_rate & 0xFF);
    hdr[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
    hdr[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
    hdr[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
    hdr[32] = (uint8_t)block_align;
    hdr[33] = (uint8_t)(block_align >> 8);
    hdr[34] = (uint8_t)bits_per_sam;
    hdr[35] = (uint8_t)(bits_per_sam >> 8);
    memcpy(hdr + 36, "data", 4);
    hdr[40] = (uint8_t)(data_bytes & 0xFF);
    hdr[41] = (uint8_t)((data_bytes >> 8) & 0xFF);
    hdr[42] = (uint8_t)((data_bytes >> 16) & 0xFF);
    hdr[43] = (uint8_t)((data_bytes >> 24) & 0xFF);
}

/* ---- pre-roll ring (PSRAM) ----
 * Writer-only ring; flushed in-order from oldest to newest into the WAV
 * file when a recording starts. */
static void preroll_push(const int16_t *src, size_t n)
{
    if (s_preroll_cap == 0 || n == 0) return;
    while (n > 0) {
        size_t room = s_preroll_cap - s_preroll_head;
        size_t take = n < room ? n : room;
        memcpy(&s_preroll[s_preroll_head], src, take * BYTES_PER_SAMPLE);
        s_preroll_head = (s_preroll_head + take) % s_preroll_cap;
        s_preroll_count += take;
        if (s_preroll_count > s_preroll_cap) s_preroll_count = s_preroll_cap;
        src += take;
        n   -= take;
    }
}

/* Iterate pre-roll oldest → newest. invokes cb with up to two contiguous
 * spans (PSRAM ring may be split). */
static void preroll_flush(FILE *fp)
{
    if (!fp || s_preroll_count == 0) return;
    size_t start = (s_preroll_head + s_preroll_cap - s_preroll_count) % s_preroll_cap;
    size_t left  = s_preroll_count;
    if (start + left <= s_preroll_cap) {
        fwrite(&s_preroll[start], BYTES_PER_SAMPLE, left, fp);
    } else {
        size_t first = s_preroll_cap - start;
        fwrite(&s_preroll[start], BYTES_PER_SAMPLE, first, fp);
        fwrite(&s_preroll[0],     BYTES_PER_SAMPLE, left - first, fp);
    }
}

/* ---- FIFO rotation ---- */

typedef struct {
    char     name[NAME_MAX_LEN];
    time_t   mtime;
    uint64_t size;
} FileEntry;

static int file_entry_cmp_mtime(const void *a, const void *b)
{
    const FileEntry *fa = a, *fb = b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return 0;
}

/* Scan recording dir, sum sizes, optionally collect entries (caller frees).
 * Returns number of files. */
static uint32_t scan_recordings(uint64_t *total_bytes_out, FileEntry **entries_out)
{
    uint64_t total = 0;
    uint32_t count = 0;
    FileEntry *list = NULL;
    size_t cap = 0;

    DIR *d = opendir(s_dir);
    if (!d) {
        if (total_bytes_out) *total_bytes_out = 0;
        if (entries_out) *entries_out = NULL;
        return 0;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_type != DT_REG) continue;
        size_t nlen = strnlen(de->d_name, NAME_MAX_LEN);
        if (nlen < 5 || nlen >= NAME_MAX_LEN) continue;
        if (strcmp(&de->d_name[nlen - 4], ".wav") != 0) continue;

        char full[PATH_MAX_LEN];
        if (snprintf(full, sizeof(full), "%s/%s", s_dir, de->d_name) >= (int)sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;

        total += (uint64_t)st.st_size;
        count++;

        if (entries_out) {
            if (count > cap) {
                size_t new_cap = cap == 0 ? 16 : cap * 2;
                FileEntry *nl = (FileEntry *)realloc(list, new_cap * sizeof(FileEntry));
                if (!nl) { free(list); list = NULL; cap = 0; entries_out = NULL; continue; }
                list = nl;
                cap  = new_cap;
            }
            strncpy(list[count - 1].name, de->d_name, NAME_MAX_LEN - 1);
            list[count - 1].name[NAME_MAX_LEN - 1] = '\0';
            list[count - 1].mtime = st.st_mtime;
            list[count - 1].size  = (uint64_t)st.st_size;
        }
    }
    closedir(d);

    if (total_bytes_out) *total_bytes_out = total;
    if (entries_out) *entries_out = list;
    return count;
}

/* If used > MAX_MB, delete oldest files until under the cap. */
static void enforce_fifo_cap(void)
{
    const uint64_t cap_bytes = (uint64_t)CONFIG_BATEAR_TF_MAX_MB * 1024ULL * 1024ULL;
    uint64_t total = 0;
    FileEntry *entries = NULL;
    uint32_t n = scan_recordings(&total, &entries);

    if (total <= cap_bytes || n == 0 || !entries) {
        free(entries);
        return;
    }

    qsort(entries, n, sizeof(FileEntry), file_entry_cmp_mtime);

    for (uint32_t i = 0; i < n && total > cap_bytes; i++) {
        char full[PATH_MAX_LEN];
        if (snprintf(full, sizeof(full), "%s/%s", s_dir, entries[i].name) >= (int)sizeof(full)) continue;
        if (unlink(full) == 0) {
            ESP_LOGI(TAG, "FIFO: deleted %s (%llu B)", entries[i].name, (unsigned long long)entries[i].size);
            total -= entries[i].size;
        } else {
            ESP_LOGW(TAG, "FIFO: unlink %s failed: %s", entries[i].name, strerror(errno));
        }
    }
    free(entries);
}

static void refresh_storage_stats(void)
{
    uint64_t used = 0;
    uint32_t files = scan_recordings(&used, NULL);

    uint32_t free_mb = 0, total_mb = 0;
    struct statvfs vfs;
    if (statvfs(MOUNT_POINT, &vfs) == 0) {
        uint64_t free_bytes  = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        uint64_t total_bytes = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
        free_mb  = (uint32_t)(free_bytes  / (1024ULL * 1024ULL));
        total_mb = (uint32_t)(total_bytes / (1024ULL * 1024ULL));
    }

    stats_lock();
    s_stats.used_mb  = (uint32_t)(used / (1024ULL * 1024ULL));
    s_stats.free_mb  = free_mb;
    s_stats.total_mb = total_mb;
    s_stats.files    = files;
    stats_unlock();
}

/* ---- WAV file handle ---- */

typedef struct {
    FILE    *fp;
    char     name[NAME_MAX_LEN];
    uint32_t data_bytes;
    int64_t  start_us;
    int64_t  last_header_refresh_us;
} WavFile;

static bool wav_open(WavFile *w, const char *suffix)
{
    char ts[24];
    ntp_time_format(ts, sizeof(ts));
    static uint16_t s_seq;
    int n = snprintf(w->name, sizeof(w->name), "%s_%05u_%s.wav",
                     ts, (unsigned)(s_seq++ & 0xFFFF), suffix);
    if (n <= 0 || (size_t)n >= sizeof(w->name)) return false;

    char full[PATH_MAX_LEN];
    if (snprintf(full, sizeof(full), "%s/%s", s_dir, w->name) >= (int)sizeof(full)) return false;

    enforce_fifo_cap();

    w->fp = fopen(full, "wb");
    if (!w->fp) {
        ESP_LOGW(TAG, "fopen %s failed: %s", full, strerror(errno));
        return false;
    }
    /* setvbuf to a 4 KB block buffer reduces the number of fwrite syscalls
     * across small frames; FATFS handles cluster allocation in 32 KB chunks. */
    static char io_buf[4096];
    setvbuf(w->fp, io_buf, _IOFBF, sizeof(io_buf));

    uint8_t hdr[WAV_HEADER_SIZE];
    wav_build_header(hdr, 0);
    fwrite(hdr, 1, WAV_HEADER_SIZE, w->fp);

    w->data_bytes = 0;
    w->start_us   = esp_timer_get_time();
    w->last_header_refresh_us = w->start_us;

    ESP_LOGI(TAG, "REC start: %s", w->name);

    stats_lock();
    s_stats.recording = true;
    strncpy(s_stats.last_file, w->name, sizeof(s_stats.last_file) - 1);
    s_stats.last_file[sizeof(s_stats.last_file) - 1] = '\0';
    stats_unlock();

    return true;
}

static void wav_write_samples(WavFile *w, const int16_t *pcm, size_t n)
{
    if (!w || !w->fp || n == 0) return;
    size_t wrote = fwrite(pcm, BYTES_PER_SAMPLE, n, w->fp);
    if (wrote != n) {
        ESP_LOGW(TAG, "fwrite short: %zu/%zu", wrote, n);
    }
    w->data_bytes += (uint32_t)(wrote * BYTES_PER_SAMPLE);

    /* Periodically refresh the data_size field so a power loss leaves a
     * playable file (most players honour the in-place size). */
    int64_t now = esp_timer_get_time();
    if (now - w->last_header_refresh_us > (int64_t)HEADER_REFRESH_SEC * 1000000LL) {
        long pos = ftell(w->fp);
        if (pos > 0) {
            uint8_t hdr[WAV_HEADER_SIZE];
            wav_build_header(hdr, w->data_bytes);
            fflush(w->fp);
            if (fseek(w->fp, 0, SEEK_SET) == 0) {
                fwrite(hdr, 1, WAV_HEADER_SIZE, w->fp);
                fseek(w->fp, pos, SEEK_SET);
            }
            fflush(w->fp);
        }
        w->last_header_refresh_us = now;
    }
}

static void wav_close(WavFile *w)
{
    if (!w || !w->fp) return;
    uint8_t hdr[WAV_HEADER_SIZE];
    wav_build_header(hdr, w->data_bytes);
    fflush(w->fp);
    if (fseek(w->fp, 0, SEEK_SET) == 0) {
        fwrite(hdr, 1, WAV_HEADER_SIZE, w->fp);
    }
    fclose(w->fp);
    w->fp = NULL;

    int64_t dur_ms = (esp_timer_get_time() - w->start_us) / 1000;
    ESP_LOGI(TAG, "REC stop: %s dur=%lld.%03llds bytes=%u",
             w->name, (long long)(dur_ms / 1000), (long long)(dur_ms % 1000),
             (unsigned)(w->data_bytes + WAV_HEADER_SIZE));

    stats_lock();
    s_stats.recording = false;
    stats_unlock();

    refresh_storage_stats();
}

/* ---- writer task ----------------------------------------------------- */

static void writer_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TfWriterTask start (core %d)", xPortGetCoreID());

    TfState  state = STATE_IDLE;
    WavFile  wav   = {0};

    int64_t  postroll_deadline_us = 0;
    int64_t  manual_deadline_us   = 0;
    int64_t  always_segment_us    = 0;

#if CONFIG_BATEAR_TF_RECORD_ALWAYS
    /* Kick the always-on debug pipeline once. */
    state = STATE_RECORDING_ALWAYS;
    if (!wav_open(&wav, "always")) {
        state = STATE_IDLE;
    } else {
        always_segment_us = esp_timer_get_time();
    }
#endif

    for (;;) {
        /* 1) drain commands (non-blocking) */
        TfCmd_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case TF_CMD_ALARM_START:
                if (state == STATE_RECORDING_ALARM) {
                    /* Already recording an alarm; reset post-roll. */
                    postroll_deadline_us = 0;
                } else {
                    if (state != STATE_IDLE && wav.fp) wav_close(&wav);
                    if (wav_open(&wav, "alarm")) {
                        preroll_flush(wav.fp);
                        wav.data_bytes += (uint32_t)(s_preroll_count * BYTES_PER_SAMPLE);
                        state = STATE_RECORDING_ALARM;
                        postroll_deadline_us = 0;
                    }
                }
                break;
            case TF_CMD_ALARM_CLEAR:
                if (state == STATE_RECORDING_ALARM) {
                    postroll_deadline_us = esp_timer_get_time() +
                        (int64_t)CONFIG_BATEAR_TF_POSTROLL_SEC * 1000000LL;
                }
                break;
            case TF_CMD_MANUAL_START:
#if CONFIG_BATEAR_TF_MANUAL_ENABLE
                if (state != STATE_IDLE && wav.fp) wav_close(&wav);
                if (wav_open(&wav, "manual")) {
                    preroll_flush(wav.fp);
                    wav.data_bytes += (uint32_t)(s_preroll_count * BYTES_PER_SAMPLE);
                    state = STATE_RECORDING_MANUAL;
                    manual_deadline_us = esp_timer_get_time() +
                        (int64_t)CONFIG_BATEAR_TF_MANUAL_SEC * 1000000LL;
                    ESP_LOGI(TAG, "MANUAL REC start (max %ds)",
                             CONFIG_BATEAR_TF_MANUAL_SEC);
                }
#endif
                break;
            case TF_CMD_MANUAL_STOP:
                if (state == STATE_RECORDING_MANUAL && wav.fp) {
                    int64_t dur = (esp_timer_get_time() - wav.start_us) / 1000000LL;
                    ESP_LOGI(TAG, "MANUAL REC stop dur=%llds file=%s",
                             (long long)dur, wav.name);
                    wav_close(&wav);
                    state = STATE_IDLE;
                }
                break;
            }
        }

        /* 2) drain audio ringbuffer (block briefly so we don't busy-spin) */
        size_t  item_size = 0;
        int16_t *item = (int16_t *)xRingbufferReceive(s_pcm_ring, &item_size,
                                                       pdMS_TO_TICKS(100));
        if (item != NULL && item_size > 0) {
            size_t n_samples = item_size / BYTES_PER_SAMPLE;
            preroll_push(item, n_samples);
            if (state != STATE_IDLE && wav.fp) {
                wav_write_samples(&wav, item, n_samples);
            }
            vRingbufferReturnItem(s_pcm_ring, item);
        }

        /* 3) deadlines */
        int64_t now = esp_timer_get_time();
        if (state == STATE_RECORDING_ALARM &&
            postroll_deadline_us != 0 && now >= postroll_deadline_us) {
            wav_close(&wav);
            state = STATE_IDLE;
            postroll_deadline_us = 0;
        }
        if (state == STATE_RECORDING_MANUAL && now >= manual_deadline_us) {
            ESP_LOGI(TAG, "MANUAL REC auto-stop (timeout)");
            wav_close(&wav);
            state = STATE_IDLE;
        }
#if CONFIG_BATEAR_TF_RECORD_ALWAYS
        if (state == STATE_RECORDING_ALWAYS &&
            now - always_segment_us >= (int64_t)ALWAYS_SEGMENT_SEC * 1000000LL) {
            wav_close(&wav);
            if (wav_open(&wav, "always")) {
                always_segment_us = now;
            } else {
                state = STATE_IDLE;
            }
        }
#endif

        /* 4) periodic stats refresh (every ~5 s) */
        static int64_t s_last_stats_us = 0;
        if (now - s_last_stats_us > 5LL * 1000000LL) {
            stats_lock();
            s_stats.drops = s_drops_atomic;
            stats_unlock();
            refresh_storage_stats();
            s_last_stats_us = now;
        }
    }
}

/* ---- public API ----------------------------------------------------- */

void tf_recorder_push_pcm(const int16_t *pcm, size_t n_samples)
{
    if (!s_inited || !s_pcm_ring || n_samples == 0) return;
    BaseType_t ok = xRingbufferSend(s_pcm_ring, pcm,
                                     n_samples * BYTES_PER_SAMPLE, 0);
    if (ok != pdTRUE) {
        s_drops_atomic++;
    }
}

void tf_recorder_send_cmd(TfRecorderCmd cmd)
{
    if (!s_inited || !s_cmd_queue) return;
    TfCmd_t c = { .type = cmd };
    xQueueSend(s_cmd_queue, &c, 0);
}

void tf_recorder_get_stats(TfRecorderStats *out)
{
    if (!out) return;
    if (!s_inited) {
        memset(out, 0, sizeof(*out));
        return;
    }
    stats_lock();
    *out = s_stats;
    out->mounted = s_mounted;
    out->drops   = s_drops_atomic;
    stats_unlock();
}

#if BOARD_HAS_SDMMC
static esp_err_t mount_sd(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 32 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* 1-bit so we only need CLK + CMD + D0 (the LilyGo T-ETH-Lite-S3
     * routes those to GPIO 6/5/7; D3/CS on GPIO 42 is left floating). */
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.clk   = (gpio_num_t)PIN_SD_CLK;
    slot_cfg.cmd   = (gpio_num_t)PIN_SD_CMD;
    slot_cfg.d0    = (gpio_num_t)PIN_SD_D0;
    slot_cfg.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_cfg,
                                             &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s — recording disabled this boot",
                 esp_err_to_name(err));
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}
#endif /* BOARD_HAS_SDMMC */

esp_err_t tf_recorder_init(const char *wired_id)
{
    if (s_inited) return ESP_OK;

#if !BOARD_HAS_SDMMC
    ESP_LOGW(TAG, "board has no on-board SD slot — recording disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    s_inited = true;     /* mark up-front so push_pcm is safe even if mount fails */

    s_stats_mtx = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(8, sizeof(TfCmd_t));
    s_pcm_ring  = xRingbufferCreate(RING_SIZE_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_stats_mtx || !s_cmd_queue || !s_pcm_ring) {
        ESP_LOGE(TAG, "primitive alloc failed");
        return ESP_ERR_NO_MEM;
    }

    strncpy(s_dev_id, (wired_id && wired_id[0]) ? wired_id : "wd",
            sizeof(s_dev_id) - 1);
    s_dev_id[sizeof(s_dev_id) - 1] = '\0';
    snprintf(s_dir, sizeof(s_dir), "%s/%s", REC_ROOT, s_dev_id);

    /* Allocate pre-roll in PSRAM. Falls back to internal SRAM with a warning. */
    const size_t preroll_samples =
        (size_t)CONFIG_BATEAR_TF_PREROLL_SEC * (size_t)SAMPLE_RATE_HZ;
    s_preroll = (int16_t *)heap_caps_malloc(preroll_samples * BYTES_PER_SAMPLE,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_preroll) {
        ESP_LOGW(TAG, "PSRAM alloc failed — falling back to internal SRAM");
        s_preroll = (int16_t *)malloc(preroll_samples * BYTES_PER_SAMPLE);
    }
    if (!s_preroll) {
        ESP_LOGE(TAG, "pre-roll alloc failed (%u B) — recording disabled",
                 (unsigned)(preroll_samples * BYTES_PER_SAMPLE));
        return ESP_ERR_NO_MEM;
    }
    s_preroll_cap = preroll_samples;

    if (mount_sd() == ESP_OK) {
        s_mounted = true;
        mkdir(REC_ROOT, 0775);
        mkdir(s_dir, 0775);
        refresh_storage_stats();
        enforce_fifo_cap();
    }

    BaseType_t ok = xTaskCreatePinnedToCore(writer_task, "TfWriter",
                                             WRITER_TASK_STACK / sizeof(StackType_t),
                                             NULL, WRITER_TASK_PRIO, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "writer task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "init OK: dev=%s dir=%s preroll=%us mounted=%d",
             s_dev_id, s_dir, CONFIG_BATEAR_TF_PREROLL_SEC, (int)s_mounted);
    return s_mounted ? ESP_OK : ESP_ERR_INVALID_STATE;
#endif
}

#endif /* CONFIG_BATEAR_TF_RECORD_ENABLE */
