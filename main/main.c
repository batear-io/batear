/*
 * ESP32-S3 + ICS-43434 (I2S MEMS) ambient sound monitor
 *
 * Uses multi-frequency Goertzel + tonal/broadband energy ratio
 * for heuristic drone rotor sound detection.
 * Thresholds must be calibrated per deployment environment.
 */

#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/i2s_std.h"

static const char *TAG = "batear";

/* ========= I2S microphone (ICS-43434) =========
 * Wiring: VDD=3.3V, GND, SD->DIN, WS->LRCLK, SCK->BCLK, L/R->GND (left channel)
 */
#define I2S_MIC_BCLK_GPIO   GPIO_NUM_43
#define I2S_MIC_WS_GPIO     GPIO_NUM_44
#define I2S_MIC_DIN_GPIO    GPIO_NUM_1

#define SAMPLE_RATE_HZ      16000
#define FRAME_SAMPLES       512
#define HOP_MS              100

/* Rotor harmonics typically fall in hundreds of Hz to a few kHz — calibrate per drone type */
#define GOERTZEL_FREQS      6
static const float k_target_hz[GOERTZEL_FREQS] = { 200.f, 400.f, 800.f, 1200.f, 2400.f, 4000.f };

/* EMA-smoothed per-frequency threshold — calibrate per environment */
#define FREQ_RATIO_ON       0.008f
#define FREQ_RATIO_OFF      0.004f
/* EMA smoothing: 0.3 = fast response, lower = smoother/slower */
#define EMA_ALPHA           0.25f
/* Minimum number of EMA-smoothed frequencies exceeding threshold */
#define FREQS_NEEDED        1
#define SUSTAIN_FRAMES_ON   2
#define SUSTAIN_FRAMES_OFF  8
/* Minimum RMS to process a frame — below this ratio is unreliable (noise floor artifact) */
#define RMS_MIN             0.0003f

static float s_window[FRAME_SAMPLES];
static float s_audio[FRAME_SAMPLES];
static i2s_chan_handle_t s_rx = NULL;

typedef struct {
    float coeff;
    float cos_omega;
    float sin_omega;
} goertzel_coeff_t;

static goertzel_coeff_t s_goertzel[GOERTZEL_FREQS];
static float s_freq_ema[GOERTZEL_FREQS];

static void init_hanning(void)
{
    const int n = FRAME_SAMPLES;
    for (int i = 0; i < n; i++) {
        s_window[i] = 0.5f * (1.f - cosf(2.f * (float)M_PI * i / (float)(n - 1)));
    }
}

static void init_goertzel_coeffs(void)
{
    for (int f = 0; f < GOERTZEL_FREQS; f++) {
        float k = roundf(k_target_hz[f] * FRAME_SAMPLES / (float)SAMPLE_RATE_HZ);
        if (k < 1.f) k = 1.f;
        const float omega = 2.f * (float)M_PI * k / (float)FRAME_SAMPLES;
        s_goertzel[f].coeff     = 2.f * cosf(omega);
        s_goertzel[f].cos_omega = cosf(omega);
        s_goertzel[f].sin_omega = sinf(omega);
    }
}

static float goertzel_power(const float *x, int n, const goertzel_coeff_t *gc)
{
    float s = 0.f, s_prev = 0.f, s_prev2 = 0.f;
    for (int i = 0; i < n; i++) {
        s = x[i] + gc->coeff * s_prev - s_prev2;
        s_prev2 = s_prev;
        s_prev = s;
    }
    const float real = s_prev - s_prev2 * gc->cos_omega;
    const float imag = s_prev2 * gc->sin_omega;
    return real * real + imag * imag;
}

static esp_err_t i2s_microphone_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_BCLK_GPIO,
            .ws = I2S_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* Philips mode defaults to BOTH slots on S3; ICS-43434 with L/R=GND uses left channel */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    /* 24-bit effective width in 32-bit slot; MCLK x384 recommended for 24-bit accuracy */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    return i2s_channel_enable(s_rx);
}

static void read_pcm_frame(float *out, int n)
{
    static int32_t raw[FRAME_SAMPLES];
    size_t bytes_read = 0;
    const size_t need = (size_t)n * sizeof(int32_t);

    /* Drain stale DMA data, keep only the latest frame to minimize latency */
    while (i2s_channel_read(s_rx, raw, need, &bytes_read, pdMS_TO_TICKS(1000)) == ESP_OK) {
        if (bytes_read >= need) {
            break;
        }
    }

    for (int i = 0; i < n; i++) {
        /* 32-bit I2S left-aligned 24-bit: normalize by full-scale value */
        float v = (float)raw[i] / 2147483648.0f;
        out[i] = v * s_window[i];
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Drone sound monitor (ICS-43434 @ %d Hz, frame=%d)", SAMPLE_RATE_HZ, FRAME_SAMPLES);

    init_hanning();
    init_goertzel_coeffs();

    esp_err_t err = i2s_microphone_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return;
    }

    int sustain_on = 0;
    int sustain_off = 0;
    bool alarm = false;
    int detection_count = 0;

    for (;;) {
        read_pcm_frame(s_audio, FRAME_SAMPLES);

        float sum_sq = 0.f;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            sum_sq += s_audio[i] * s_audio[i];
        }
        float rms = sqrtf(sum_sq / (float)FRAME_SAMPLES);
        if (rms < RMS_MIN) {
            /* Frame too quiet — ratio unreliable, decay EMA toward zero */
            for (int f = 0; f < GOERTZEL_FREQS; f++) {
                s_freq_ema[f] *= (1.f - EMA_ALPHA);
            }
            if (alarm) {
                sustain_off++;
                if (sustain_off >= SUSTAIN_FRAMES_OFF) {
                    alarm = false;
                    ESP_LOGI(TAG, "Environment quiet (silent)");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(HOP_MS));
            continue;
        }

        const float denom = sum_sq * (float)FRAME_SAMPLES + 1e-12f;
        int active_freqs = 0;
        float freq_ratios[GOERTZEL_FREQS];
        for (int f = 0; f < GOERTZEL_FREQS; f++) {
            float r = goertzel_power(s_audio, FRAME_SAMPLES, &s_goertzel[f]) / denom;
            s_freq_ema[f] = EMA_ALPHA * r + (1.f - EMA_ALPHA) * s_freq_ema[f];
            freq_ratios[f] = s_freq_ema[f];
            if (s_freq_ema[f] > FREQ_RATIO_ON) active_freqs++;
        }

        if (!alarm) {
            if (active_freqs >= FREQS_NEEDED) {
                sustain_on++;
                sustain_off = 0;
                if (sustain_on >= SUSTAIN_FRAMES_ON) {
                    alarm = true;
                    detection_count++;
                    ESP_LOGW(TAG, ">>> Drone detected #%d: %d/%d freqs active, rms=%.5f", detection_count, active_freqs, GOERTZEL_FREQS, rms);
                }
            } else {
                sustain_on = 0;
            }
        } else {
            int active_off = 0;
            for (int f = 0; f < GOERTZEL_FREQS; f++) {
                if (freq_ratios[f] > FREQ_RATIO_OFF) active_off++;
            }
            if (active_off < FREQS_NEEDED) {
                sustain_off++;
                sustain_on = 0;
                if (sustain_off >= SUSTAIN_FRAMES_OFF) {
                    alarm = false;
                    ESP_LOGI(TAG, "Environment quiet (active_freqs=%d)", active_off);
                }
            } else {
                sustain_off = 0;
            }
        }

        static int64_t last_cal_log_us;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_cal_log_us > 1000000) {
            ESP_LOGI(TAG, "cal: active=%d/%d [%.3f %.3f %.3f %.3f %.3f %.3f] rms=%.5f alarm=%d",
                     active_freqs, GOERTZEL_FREQS,
                     freq_ratios[0], freq_ratios[1], freq_ratios[2],
                     freq_ratios[3], freq_ratios[4], freq_ratios[5],
                     rms, alarm);
            last_cal_log_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(HOP_MS));
    }
}
