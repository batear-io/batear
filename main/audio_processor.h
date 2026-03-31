/*
 * audio_processor.h — ESP-DSP FFT pipeline + harmonic drone signature scan
 *
 * 16 kHz, 1024-point complex FFT (interleaved fc32), Hanning window,
 * 15.625 Hz/bin. Buffers in .c are 16-byte aligned for SIMD (ESP32-S3).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PROC_SAMPLE_RATE_HZ   16000
#define AUDIO_PROC_FFT_SIZE         1024
#define AUDIO_PROC_BIN_HZ           ((float)AUDIO_PROC_SAMPLE_RATE_HZ / (float)AUDIO_PROC_FFT_SIZE)

/** One-sided PSD length: bins 0 .. Nyquist (inclusive). */
#define AUDIO_PROC_PSD_BINS         ((AUDIO_PROC_FFT_SIZE / 2) + 1)

typedef struct {
    int   fundamental_bin;
    float fundamental_hz;
    float fundamental_pwr;
    float harmonic2_pwr;
    float harmonic3_pwr;
    float h2_ratio;       /* P(h2) / P(f0) */
    float h3_ratio;       /* P(h3) / P(f0) */
    float noise_floor;    /* linear power from estimate hook */
    float snr;            /* P(f0) / noise_floor */
    float confidence;     /* 0..1 heuristic */
} HarmonicAnalysisResult;

/**
 * Dynamic noise floor (linear power). Weak symbol — override in another TU for
 * custom masking / percentile logic. Default: mean PSD excluding DC (bin 0).
 */
__attribute__((weak)) float audio_processor_noise_floor_estimate(const float *psd, int n_bins_half);

esp_err_t audio_processor_init(void);
void      audio_processor_deinit(void);

/** Hann window: w[n] = 0.5 * (1 - cos(2*pi*n/(N-1))). Built at init. */
const float *audio_processor_window(void);

/**
 * Fill FFT input: for i in [0, N), fft[2*i]=pcm[i]*scale*window[i], fft[2*i+1]=0.
 * pcm is 32-bit I2S mono (typical Q31-style); scale matches legacy /2147483648.f.
 * Returns RMS of windowed samples (same domain as before FFT).
 */
float audio_processor_prepare_fft_input(float *fft_io /* [2*AUDIO_PROC_FFT_SIZE], aligned */,
                                        const int32_t *pcm, int n_samples);

/** Forward FFT + bit-reverse; fft_io is interleaved complex length AUDIO_PROC_FFT_SIZE. */
esp_err_t audio_processor_fft_forward(float *fft_io);

/**
 * Magnitude-squared spectrum for bins 0 .. N/2 (writes AUDIO_PROC_PSD_BINS floats).
 * Call after audio_processor_fft_forward().
 */
void audio_processor_psd_from_fft(const float *fft_complex, float *psd_out);

/**
 * Full chain: prepare (if pcm non-NULL) + FFT + PSD into internal buffer.
 * If pcm is NULL, expects fft_io already filled (e.g. unit test).
 */
esp_err_t audio_processor_compute_psd(float *fft_work, const int32_t *pcm, int n_samples);

const float *audio_processor_last_psd(void);
float        audio_processor_last_rms(void);

/**
 * Scan for drone-like tone with 2nd/3rd harmonics above the noise floor.
 * psd: length n_bins_half (≥ AUDIO_PROC_PSD_BINS). fundamental search in [f0_min_hz, f0_max_hz].
 */
bool analyze_harmonics(const float *psd, int n_bins_half, float f0_min_hz, float f0_max_hz,
                       HarmonicAnalysisResult *out);

#ifdef __cplusplus
}
#endif
