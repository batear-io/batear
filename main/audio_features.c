/*
 * audio_features.c — log-mel spectrogram for the TFLite drone classifier.
 *
 * The actual FFT, Hann windowing and PSD computation are reused from
 * audio_processor.c. This file only adds:
 *   1) a triangular mel filter bank (sparse representation),
 *   2) log10 + int8 quantisation matching the model's input tensor,
 *   3) a 32-frame circular buffer (oldest -> newest = row 0 -> row 31).
 *
 * No dynamic allocation: everything sits in static storage. The mel filter
 * bank holds at most ~30 weights per band, so the sparse table stays under
 * ~5 KB even for 40 bands.
 */

#include "audio_features.h"
#include "audio_processor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "audio_feat";

#define MEL_LOWER_HZ       0.0f
#define MEL_UPPER_HZ       8000.0f                       /* Nyquist for 16 kHz */
#define MEL_LOG_EPS        1e-10f                        /* avoid log10(0) */

/* Maximum bins a single triangular filter can span at our resolution.
 * BIN_HZ = 16000 / 1024 = 15.625 Hz. The widest mel filter near the top of
 * the band is well under 50 bins, so 64 leaves comfortable headroom. */
#define MEL_MAX_BAND_BINS  64

typedef struct {
    int   start_bin;                      /* first PSD bin contributing */
    int   n_bins;                         /* number of contributing bins */
    float weights[MEL_MAX_BAND_BINS];     /* triangular filter coefficients */
} MelBand;

static MelBand s_bands[AUDIO_FEATURES_MEL_BANDS];
static bool    s_initialised = false;

/* Aligned FFT scratch (interleaved complex). Owned by audio_features so
 * audio_task does not need to expose its private buffers across translation
 * units. ~8 KB internal SRAM. */
static float __attribute__((aligned(16))) s_fft_scratch[2 * AUDIO_PROC_FFT_SIZE];

/* Quantisation parameters captured from the model's input tensor. */
static float   s_input_scale = 1.0f;
static int8_t  s_input_zp    = 0;

/* Circular mel-spectrogram ring: [AUDIO_FEATURES_MEL_FRAMES][AUDIO_FEATURES_MEL_BANDS] int8.
 * s_head points at the oldest frame (= the slot the next push will overwrite). */
static int8_t  s_ring[AUDIO_FEATURES_MEL_FRAMES][AUDIO_FEATURES_MEL_BANDS];
static int     s_head        = 0;
static int     s_n_pushed    = 0;

static inline float hz_to_mel(float hz)
{
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static inline float mel_to_hz(float mel)
{
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

/* Build a 40-band triangular mel filter bank over PSD bins [0 .. AUDIO_PROC_PSD_BINS).
 * Standard Slaney-style: place AUDIO_FEATURES_MEL_BANDS+2 mel-spaced points,
 * filter k spans (point[k], point[k+1], point[k+2]) with peak 1 at point[k+1]. */
static void build_mel_filterbank(void)
{
    const float mel_lo = hz_to_mel(MEL_LOWER_HZ);
    const float mel_hi = hz_to_mel(MEL_UPPER_HZ);
    const int   n_pts  = AUDIO_FEATURES_MEL_BANDS + 2;

    int bin_centres[AUDIO_FEATURES_MEL_BANDS + 2];
    for (int i = 0; i < n_pts; i++) {
        const float frac = (float)i / (float)(n_pts - 1);
        const float mel  = mel_lo + frac * (mel_hi - mel_lo);
        const float hz   = mel_to_hz(mel);
        int bin = (int)roundf(hz / AUDIO_PROC_BIN_HZ);
        if (bin < 0) {
            bin = 0;
        }
        if (bin >= AUDIO_PROC_PSD_BINS) {
            bin = AUDIO_PROC_PSD_BINS - 1;
        }
        bin_centres[i] = bin;
    }

    for (int b = 0; b < AUDIO_FEATURES_MEL_BANDS; b++) {
        const int bin_l = bin_centres[b];
        const int bin_c = bin_centres[b + 1];
        const int bin_r = bin_centres[b + 2];

        /* Degenerate filter (low-frequency bins squashed by quantisation):
         * fall back to a single-bin pass-through so we never produce zeros. */
        if (bin_r <= bin_l) {
            s_bands[b].start_bin = bin_c;
            s_bands[b].n_bins    = 1;
            s_bands[b].weights[0] = 1.0f;
            continue;
        }

        const int span = bin_r - bin_l + 1;
        if (span > MEL_MAX_BAND_BINS) {
            ESP_LOGW(TAG, "mel band %d span %d exceeds MEL_MAX_BAND_BINS=%d, clamping",
                     b, span, MEL_MAX_BAND_BINS);
        }

        const int n_bins = (span > MEL_MAX_BAND_BINS) ? MEL_MAX_BAND_BINS : span;
        s_bands[b].start_bin = bin_l;
        s_bands[b].n_bins    = n_bins;

        for (int k = 0; k < n_bins; k++) {
            const int bin = bin_l + k;
            float w;
            if (bin <= bin_c) {
                const int denom = (bin_c - bin_l) > 0 ? (bin_c - bin_l) : 1;
                w = (float)(bin - bin_l) / (float)denom;
            } else {
                const int denom = (bin_r - bin_c) > 0 ? (bin_r - bin_c) : 1;
                w = (float)(bin_r - bin) / (float)denom;
            }
            if (w < 0.0f) {
                w = 0.0f;
            }
            s_bands[b].weights[k] = w;
        }
    }
}

esp_err_t audio_features_init(int8_t input_zero_point, float input_scale)
{
    if (input_scale <= 0.0f || !isfinite(input_scale)) {
        ESP_LOGE(TAG, "invalid input_scale=%f", (double)input_scale);
        return ESP_ERR_INVALID_ARG;
    }
    s_input_zp    = input_zero_point;
    s_input_scale = input_scale;

    build_mel_filterbank();
    audio_features_reset();
    s_initialised = true;

    ESP_LOGI(TAG, "init OK: %d bands x %d frames, input scale=%.5f zp=%d",
             AUDIO_FEATURES_MEL_BANDS, AUDIO_FEATURES_MEL_FRAMES,
             (double)s_input_scale, (int)s_input_zp);
    return ESP_OK;
}

void audio_features_reset(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_head     = 0;
    s_n_pushed = 0;
}

bool audio_features_is_warm(void)
{
    return s_n_pushed >= AUDIO_FEATURES_MEL_FRAMES;
}

esp_err_t audio_features_push_frame(const int32_t *pcm, int n_samples)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || n_samples != AUDIO_PROC_FFT_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = audio_processor_compute_psd(s_fft_scratch, pcm, n_samples);
    if (err != ESP_OK) {
        return err;
    }

    const float *psd = audio_processor_last_psd();
    if (psd == NULL) {
        return ESP_FAIL;
    }

    const int8_t int8_min = -128;
    const int8_t int8_max =  127;

    for (int b = 0; b < AUDIO_FEATURES_MEL_BANDS; b++) {
        const MelBand *band = &s_bands[b];
        float acc = 0.0f;
        for (int k = 0; k < band->n_bins; k++) {
            const int bin = band->start_bin + k;
            if (bin >= 0 && bin < AUDIO_PROC_PSD_BINS) {
                acc += band->weights[k] * psd[bin];
            }
        }
        const float log_mel = log10f(acc + MEL_LOG_EPS);

        int q = (int)lrintf(log_mel / s_input_scale) + (int)s_input_zp;
        if (q < int8_min) {
            q = int8_min;
        } else if (q > int8_max) {
            q = int8_max;
        }
        s_ring[s_head][b] = (int8_t)q;
    }

    s_head = (s_head + 1) % AUDIO_FEATURES_MEL_FRAMES;
    if (s_n_pushed < AUDIO_FEATURES_MEL_FRAMES) {
        s_n_pushed++;
    }
    return ESP_OK;
}

esp_err_t audio_features_get_melgram(int8_t *out_mel)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_mel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* s_head points at the oldest slot (= next-write target). To produce a
     * row-major [oldest..newest] layout, copy from s_head wrapping around. */
    int row = 0;
    int idx = s_head;
    for (int i = 0; i < AUDIO_FEATURES_MEL_FRAMES; i++) {
        memcpy(out_mel + row * AUDIO_FEATURES_MEL_BANDS,
               s_ring[idx],
               AUDIO_FEATURES_MEL_BANDS);
        idx = (idx + 1) % AUDIO_FEATURES_MEL_FRAMES;
        row++;
    }
    return ESP_OK;
}
