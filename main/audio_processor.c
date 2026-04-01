/*
 * audio_processor.c — FFT + Hanning + PSD + harmonic analysis (ESP-DSP)
 */

#include "audio_processor.h"

#include <math.h>
#include <string.h>

#include "sdkconfig.h"
#include "dsps_fft2r.h"
#include "esp_log.h"

static const char *TAG = "audio_proc";

static float __attribute__((aligned(16))) s_window[AUDIO_PROC_FFT_SIZE];
static float __attribute__((aligned(16))) s_psd[AUDIO_PROC_PSD_BINS];
static float s_last_rms;

/* --- Default weak noise floor: mean linear power, skip DC --- */
__attribute__((weak)) float audio_processor_noise_floor_estimate(const float *psd, int n_bins_half)
{
    if (n_bins_half < 2 || psd == NULL) {
        return 1e-18f;
    }
    double acc = 0.0;
    for (int i = 1; i < n_bins_half; i++) {
        acc += (double)psd[i];
    }
    float nf = (float)(acc / (double)(n_bins_half - 1));
    return (nf < 1e-18f) ? 1e-18f : nf;
}

static void build_hanning(void)
{
    const int n = AUDIO_PROC_FFT_SIZE;
    if (n == 1) {
        s_window[0] = 1.f;
        return;
    }
    const float scale = 2.f * (float)M_PI / (float)(n - 1);
    for (int i = 0; i < n; i++) {
        s_window[i] = 0.5f * (1.f - cosf(scale * (float)i));
    }
}

esp_err_t audio_processor_init(void)
{
    build_hanning();
    esp_err_t err = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "FFT init OK (max=%d, N=%d)", CONFIG_DSP_MAX_FFT_SIZE, AUDIO_PROC_FFT_SIZE);
    return ESP_OK;
}

void audio_processor_deinit(void)
{
    dsps_fft2r_deinit_fc32();
}

// cppcheck-suppress unusedFunction
const float *audio_processor_window(void)
{
    return s_window;
}

float audio_processor_prepare_fft_input(float *fft_io, const int32_t *pcm, int n_samples)
{
    if (fft_io == NULL || pcm == NULL || n_samples != AUDIO_PROC_FFT_SIZE) {
        return 0.f;
    }

    const float inv_full_scale = 1.f / 2147483648.f;
    double sum_sq = 0.0;

    for (int i = 0; i < AUDIO_PROC_FFT_SIZE; i++) {
        const float x  = (float)pcm[i] * inv_full_scale;
        const float wx = x * s_window[i];
        fft_io[2 * i]     = wx;
        fft_io[2 * i + 1] = 0.f;
        sum_sq += (double)wx * (double)wx;
    }

    s_last_rms = (float)sqrt(sum_sq / (double)AUDIO_PROC_FFT_SIZE);
    return s_last_rms;
}

esp_err_t audio_processor_fft_forward(float *fft_io)
{
    if (fft_io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = dsps_fft2r_fc32(fft_io, AUDIO_PROC_FFT_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    return dsps_bit_rev_fc32(fft_io, AUDIO_PROC_FFT_SIZE);
}

void audio_processor_psd_from_fft(const float *fft_complex, float *psd_out)
{
    if (fft_complex == NULL || psd_out == NULL) {
        return;
    }
    for (int k = 0; k < AUDIO_PROC_PSD_BINS; k++) {
        const float re = fft_complex[2 * k];
        const float im = fft_complex[2 * k + 1];
        psd_out[k]     = re * re + im * im;
    }
}

esp_err_t audio_processor_compute_psd(float *fft_work, const int32_t *pcm, int n_samples)
{
    if (fft_work == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pcm != NULL) {
        audio_processor_prepare_fft_input(fft_work, pcm, n_samples);
    }
    esp_err_t err = audio_processor_fft_forward(fft_work);
    if (err != ESP_OK) {
        return err;
    }
    audio_processor_psd_from_fft(fft_work, s_psd);
    return ESP_OK;
}

const float *audio_processor_last_psd(void)
{
    return s_psd;
}

float audio_processor_last_rms(void)
{
    return s_last_rms;
}

/* --- Harmonic scan (tunable) --- */
#ifndef AUDIO_PROC_HARM_PEAK_MIN_SNR
#define AUDIO_PROC_HARM_PEAK_MIN_SNR 4.f
#endif
#ifndef AUDIO_PROC_HARM_MIN_H2
#define AUDIO_PROC_HARM_MIN_H2 0.07f
#endif
#ifndef AUDIO_PROC_HARM_MIN_H3
#define AUDIO_PROC_HARM_MIN_H3 0.035f
#endif
#ifndef AUDIO_PROC_HARM_BIN_TOL
#define AUDIO_PROC_HARM_BIN_TOL 2
#endif

static int clamp_bin(int b, int n_bins)
{
    if (b < 0) {
        return 0;
    }
    if (b >= n_bins) {
        return n_bins - 1;
    }
    return b;
}

static float local_peak_max(const float *psd, int n_bins, int center, int half_width)
{
    int lo = clamp_bin(center - half_width, n_bins);
    int hi = clamp_bin(center + half_width, n_bins);
    float m = 0.f;
    for (int i = lo; i <= hi; i++) {
        if (psd[i] > m) {
            m = psd[i];
        }
    }
    return m;
}

bool analyze_harmonics(const float *psd, int n_bins_half, float f0_min_hz, float f0_max_hz,
                       HarmonicAnalysisResult *out)
{
    if (psd == NULL || out == NULL || n_bins_half < AUDIO_PROC_PSD_BINS) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const float nf = audio_processor_noise_floor_estimate(psd, n_bins_half);
    out->noise_floor = nf;

    int k0 = (int)ceilf(f0_min_hz / AUDIO_PROC_BIN_HZ);
    int k1 = (int)floorf(f0_max_hz / AUDIO_PROC_BIN_HZ);
    k0     = clamp_bin(k0, n_bins_half);
    k1     = clamp_bin(k1, n_bins_half);
    if (k1 <= k0) {
        return false;
    }

    int peak_bin = k0;
    float peak   = psd[k0];
    for (int k = k0 + 1; k <= k1; k++) {
        if (psd[k] > peak) {
            peak     = psd[k];
            peak_bin = k;
        }
    }

    const float snr = peak / nf;
    out->fundamental_bin  = peak_bin;
    out->fundamental_hz   = (float)peak_bin * AUDIO_PROC_BIN_HZ;
    out->fundamental_pwr  = peak;
    out->snr              = snr;

    if (snr < AUDIO_PROC_HARM_PEAK_MIN_SNR) {
        out->confidence = 0.f;
        return false;
    }

    const int h2c = peak_bin * 2;
    const int h3c = peak_bin * 3;
    if (h2c >= n_bins_half || h3c >= n_bins_half) {
        out->confidence = 0.f;
        return false;
    }

    out->harmonic2_pwr = local_peak_max(psd, n_bins_half, h2c, AUDIO_PROC_HARM_BIN_TOL);
    out->harmonic3_pwr = local_peak_max(psd, n_bins_half, h3c, AUDIO_PROC_HARM_BIN_TOL);

    const float denom = peak + 1e-18f;
    out->h2_ratio     = out->harmonic2_pwr / denom;
    out->h3_ratio     = out->harmonic3_pwr / denom;

    const bool h2_ok = out->h2_ratio >= AUDIO_PROC_HARM_MIN_H2;
    const bool h3_ok = out->h3_ratio >= AUDIO_PROC_HARM_MIN_H3;

    if (!h2_ok || !h3_ok) {
        out->confidence = fminf(1.f, (snr / 40.f)) * fmaxf(out->h2_ratio / AUDIO_PROC_HARM_MIN_H2,
                                                            out->h3_ratio / AUDIO_PROC_HARM_MIN_H3);
        out->confidence = fminf(out->confidence, 0.99f);
        return false;
    }

    out->confidence = fminf(1.f, (snr / 25.f) * sqrtf(out->h2_ratio * out->h3_ratio));
    return true;
}
