/*
 * audio_task.c — I2S + ESP-DSP FFT harmonic drone detector (Core 1)
 */

#include "audio_task.h"
#include "audio_processor.h"
#include "drone_detector.h"
#include "pin_config.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "sdkconfig.h"

#if CONFIG_BATEAR_TF_RECORD_ENABLE
#include "tf_recorder.h"
#endif

static const char *TAG = "audio";

#define I2S_MIC_BCLK_GPIO   ((gpio_num_t)PIN_I2S_BCLK)
#define I2S_MIC_WS_GPIO     ((gpio_num_t)PIN_I2S_WS)
#define I2S_MIC_DIN_GPIO    ((gpio_num_t)PIN_I2S_DIN)

#define SAMPLE_RATE_HZ      AUDIO_PROC_SAMPLE_RATE_HZ
#define FRAME_SAMPLES       AUDIO_PROC_FFT_SIZE
#define HOP_MS              100

/* Fundamental search band (Hz); 3×f0 must stay below Nyquist (8 kHz). */
#define HARM_F0_MIN_HZ      180.f
#define HARM_F0_MAX_HZ      2400.f

#define EMA_ALPHA           0.25f
#define CONF_ON             0.30f
#define CONF_OFF            0.18f
#define SUSTAIN_FRAMES_ON   2
#define SUSTAIN_FRAMES_OFF  8
#define RMS_MIN             0.0004f

static float __attribute__((aligned(16))) s_fft_work[2 * FRAME_SAMPLES];
static int32_t __attribute__((aligned(16))) s_raw[FRAME_SAMPLES];
/* Stereo DMA: L,R pairs — ESP32-S3 Philips RX (see i2s_microphone_init). */
static int32_t __attribute__((aligned(16))) s_stereo[FRAME_SAMPLES * 2];

#if CONFIG_BATEAR_TF_RECORD_ENABLE
/* Down-sampled buffer handed off to the TF recorder — int16 mono @ 16 kHz.
 * The 32-bit I2S word from the ICS-43434 carries 24 valid bits in the upper
 * portion; right-shift by 16 keeps the most significant 16 bits. */
static int16_t __attribute__((aligned(16))) s_pcm16[FRAME_SAMPLES];
#endif

static i2s_chan_handle_t s_rx = NULL;
static float             s_conf_ema = 0.f;

static esp_err_t i2s_microphone_init(void)
{
    /*
     * ESP32-S3 Philips default slot_mask is BOTH (see i2s_std.h). Using MONO mode
     * (active_slot=1) with that mask can yield all-zero RX; use STEREO + software
     * deinterleave — same effective data as legacy ESP32 mono + LEFT.
     */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 256;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk  = I2S_GPIO_UNUSED,
            .bclk  = I2S_MIC_BCLK_GPIO,
            .ws    = I2S_MIC_WS_GPIO,
            .dout  = I2S_GPIO_UNUSED,
            .din   = I2S_MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    return i2s_channel_enable(s_rx);
}

/**
 * Block until one full FRAME_SAMPLES mono int32 frame is available.
 * Accumulates partial reads so FreeRTOS I2S DMA chunking never drops samples.
 */
/** First-frame + rare heartbeat so bring-up shows mic activity (not only on ALARM). */
static float raw_i32_rms_norm(const int32_t *s, int n)
{
    if (n <= 0 || s == NULL) {
        return 0.f;
    }
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        const double v = (double)s[i] / 2147483648.0;
        acc += v * v;
    }
    return (float)sqrt(acc / (double)n);
}

static void log_pcm_int32_span(const int32_t *s, int n, float raw_rms_norm, const char *note)
{
    if (n <= 0 || s == NULL) {
        return;
    }
    int32_t lo = s[0], hi = s[0];
    for (int i = 1; i < n; i++) {
        if (s[i] < lo) {
            lo = s[i];
        }
        if (s[i] > hi) {
            hi = s[i];
        }
    }
    ESP_LOGI(TAG, "mic %s: int32 min=%" PRId32 " max=%" PRId32 " span=%" PRId32 " raw_rms~%.5f",
             note, lo, hi, (int32_t)(hi - lo), raw_rms_norm);
}

static bool read_pcm_chunk_bytes(uint8_t *dst, size_t need_bytes)
{
    size_t got = 0;
    while (got < need_bytes) {
        size_t    chunk = 0;
        esp_err_t e     = i2s_channel_read(s_rx, dst + got, need_bytes - got, &chunk, pdMS_TO_TICKS(1000));
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "I2S read err %s", esp_err_to_name(e));
            return false;
        }
        if (chunk == 0) {
            vTaskDelay(1);
            continue;
        }
        got += chunk;
    }
    return true;
}

/** STEREO Philips: read 2 * n int32 (L,R,…), copy one channel to mono `out`. */
static bool read_pcm_frame_i32(int32_t *out, int n)
{
    if (n != FRAME_SAMPLES) {
        return false;
    }
    const size_t need_bytes = (size_t)n * 2u * sizeof(int32_t);
    if (!read_pcm_chunk_bytes((uint8_t *)s_stereo, need_bytes)) {
        return false;
    }
#if CONFIG_BATEAR_I2S_MIC_SLOT_RIGHT
    const unsigned ch = 1u;
#else
    const unsigned ch = 0u;
#endif
    for (int i = 0; i < n; i++) {
        out[i] = s_stereo[i * 2 + ch];
    }
    return true;
}

static void push_event(DroneEventType type, float ratio, int active, float rms)
{
    DroneEvent_t ev = {
        .type         = type,
        .peak_ratio   = ratio,
        .f0_bin       = active,
        .rms          = rms,
        .timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
    };
    if (xQueueSend(g_drone_event_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full — dropped 0x%02X", (unsigned)type);
    }
}

void AudioTask(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "AudioTask start (core %d) FFT N=%d", xPortGetCoreID(), FRAME_SAMPLES);

    esp_err_t err = audio_processor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_processor_init failed — suspending");
        vTaskSuspend(NULL);
        return;
    }

    err = i2s_microphone_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s — suspending", esp_err_to_name(err));
        audio_processor_deinit();
        vTaskSuspend(NULL);
        return;
    }
    ESP_LOGI(TAG, "I2S OK — port0 STEREO→mono BCLK=%d WS=%d DIN=%d",
             PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DIN);

    int  sustain_on      = 0;
    int  sustain_off     = 0;
    bool alarm           = false;
    int  detection_count = 0;
    bool mic_diag_logged = false;
    int  frames_total    = 0;

    for (;;) {
        if (!read_pcm_frame_i32(s_raw, FRAME_SAMPLES)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!mic_diag_logged) {
            log_pcm_int32_span(s_raw, FRAME_SAMPLES, raw_i32_rms_norm(s_raw, FRAME_SAMPLES), "first frame");
            mic_diag_logged = true;
        }

#if CONFIG_BATEAR_TF_RECORD_ENABLE
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            s_pcm16[i] = (int16_t)(s_raw[i] >> 16);
        }
        tf_recorder_push_pcm(s_pcm16, FRAME_SAMPLES);
#endif

#if CONFIG_BATEAR_AUDIO_PERF_LOG
        const int64_t t_psd_start = esp_timer_get_time();
#endif
        err = audio_processor_compute_psd(s_fft_work, s_raw, FRAME_SAMPLES);
#if CONFIG_BATEAR_AUDIO_PERF_LOG
        const int32_t dt_psd_us = (int32_t)(esp_timer_get_time() - t_psd_start);
#endif
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "compute_psd: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(HOP_MS));
            continue;
        }

        const float *psd = audio_processor_last_psd();
        const float rms  = audio_processor_last_rms();

        frames_total++;
        if ((frames_total % 300) == 0) {
            ESP_LOGI(TAG, "mic heartbeat (~30s): win_rms=%.5f (gate %.4f)", rms, RMS_MIN);
            if (rms < 1e-6f) {
                ESP_LOGW(TAG,
                         "I2S still silent — check ICS-43434 wiring (SCK/WS/SD), 3.3 V, GND, L/R; "
                         "try menuconfig BATEAR_I2S_MIC_SLOT_RIGHT if L/R is tied to VDD");
            }
        }

        HarmonicAnalysisResult hr;
#if CONFIG_BATEAR_AUDIO_PERF_LOG
        const int64_t t_harm_start = esp_timer_get_time();
#endif
        const bool harm_ok =
            analyze_harmonics(psd, AUDIO_PROC_PSD_BINS, HARM_F0_MIN_HZ, HARM_F0_MAX_HZ, &hr);
#if CONFIG_BATEAR_AUDIO_PERF_LOG
        const int32_t dt_harm_us = (int32_t)(esp_timer_get_time() - t_harm_start);
        static int      s_perf_n;
        static int32_t  s_psd_sum, s_harm_sum, s_psd_min = INT32_MAX, s_psd_max, s_harm_min = INT32_MAX,
            s_harm_max;
        s_psd_sum += dt_psd_us;
        s_harm_sum += dt_harm_us;
        if (dt_psd_us < s_psd_min) {
            s_psd_min = dt_psd_us;
        }
        if (dt_psd_us > s_psd_max) {
            s_psd_max = dt_psd_us;
        }
        if (dt_harm_us < s_harm_min) {
            s_harm_min = dt_harm_us;
        }
        if (dt_harm_us > s_harm_max) {
            s_harm_max = dt_harm_us;
        }
        s_perf_n++;
        if (s_perf_n >= 100) {
            ESP_LOGI(TAG,
                     "perf (100 frames): psd_fft min/avg/max=%" PRId32 "/%" PRId32 "/%" PRId32 " us  "
                     "harm min/avg/max=%" PRId32 "/%" PRId32 "/%" PRId32 " us  dsp_tot_avg=%" PRId32 " us",
                     s_psd_min, s_psd_sum / s_perf_n, s_psd_max, s_harm_min, s_harm_sum / s_perf_n, s_harm_max,
                     (s_psd_sum + s_harm_sum) / s_perf_n);
            s_perf_n     = 0;
            s_psd_sum    = 0;
            s_harm_sum   = 0;
            s_psd_min    = INT32_MAX;
            s_psd_max    = 0;
            s_harm_min   = INT32_MAX;
            s_harm_max   = 0;
        }
#endif

        float conf_display  = s_conf_ema;

        if (rms < RMS_MIN) {
            s_conf_ema *= (1.f - EMA_ALPHA);
            if (alarm) {
                sustain_off++;
                if (sustain_off >= SUSTAIN_FRAMES_OFF) {
                    alarm = false;
                    ESP_LOGI(TAG, "CLEAR (silent)");
                    push_event(DRONE_EVENT_CLEAR, 0.f, 0, rms);
                }
            }
        } else {
            if (harm_ok) {
                s_conf_ema = EMA_ALPHA * hr.confidence + (1.f - EMA_ALPHA) * s_conf_ema;
            } else {
                s_conf_ema = EMA_ALPHA * fminf(hr.confidence, 0.15f) + (1.f - EMA_ALPHA) * s_conf_ema;
            }

            const int active_metric = harm_ok ? 1 : 0;

            if (!alarm) {
                if (harm_ok && s_conf_ema >= CONF_ON) {
                    sustain_on++;
                    sustain_off = 0;
                    if (sustain_on >= SUSTAIN_FRAMES_ON) {
                        alarm = true;
                        detection_count++;
                        ESP_LOGW(TAG, "ALARM #%d: f0=%.1f Hz conf=%.2f rms=%.5f",
                                 detection_count, hr.fundamental_hz, (double)s_conf_ema, rms);
                        push_event(DRONE_EVENT_ALARM, hr.confidence,
                                   hr.fundamental_bin > 255 ? 255 : hr.fundamental_bin, rms);
                    }
                } else {
                    sustain_on = 0;
                }
            } else {
                if (!harm_ok || s_conf_ema < CONF_OFF) {
                    sustain_off++;
                    sustain_on = 0;
                    if (sustain_off >= SUSTAIN_FRAMES_OFF) {
                        alarm = false;
                        ESP_LOGI(TAG, "CLEAR (harm_ok=%d conf=%.2f)", harm_ok, (double)s_conf_ema);
                        push_event(DRONE_EVENT_CLEAR, hr.confidence, active_metric, rms);
                    }
                } else {
                    sustain_off = 0;
                }
            }
        }

        static int64_t last_cal_us = 0;
        const int64_t now_us       = esp_timer_get_time();
        if (alarm && now_us - last_cal_us > 1000000LL) {
            ESP_LOGI(TAG,
                     "cal: f0=%.1f Hz h2=%.2f h3=%.2f snr=%.1f nf=%.2e conf_ema=%.2f rms=%.5f",
                     hr.fundamental_hz, hr.h2_ratio, hr.h3_ratio, hr.snr, hr.noise_floor,
                     (double)conf_display, rms);
            last_cal_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(HOP_MS));
    }
}
