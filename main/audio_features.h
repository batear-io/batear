/*
 * audio_features.h — log-mel spectrogram feature extractor for the TFLite
 * drone classifier.
 *
 * Sample rate          : 16 kHz (AUDIO_PROC_SAMPLE_RATE_HZ)
 * Per-frame audio      : 1024 samples (AUDIO_PROC_FFT_SIZE) — same window as
 *                        the legacy FFT harmonic detector, so audio_task.c
 *                        does not need to change its I2S read cadence.
 * Mel bands            : AUDIO_FEATURES_MEL_BANDS  (40)
 * Time frames in mel   : AUDIO_FEATURES_MEL_FRAMES (32)
 * Effective context    : 32 * 100 ms hop = 3.2 s rolling window — plenty for
 *                        the propeller fundamentals + sidebands a drone
 *                        signature occupies.
 *
 * The mel grid is delivered as INT8 with the same quantisation parameters as
 * the model's input tensor. Call audio_features_init() with the values from
 * ml_classifier_input_quant() before pushing any frames.
 *
 * Internally, the feature extractor reuses the FFT + Hann window machinery
 * from audio_processor.c (one FFT per push, no extra computation), then
 * projects the magnitude-squared PSD onto a sparse mel filter bank.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_FEATURES_MEL_BANDS   40
#define AUDIO_FEATURES_MEL_FRAMES  32
#define AUDIO_FEATURES_MEL_TOTAL   (AUDIO_FEATURES_MEL_BANDS * AUDIO_FEATURES_MEL_FRAMES)

/**
 * Build the mel filter bank table and reset the ring buffer.
 * @param input_zero_point  int8 zero point of the model's input tensor.
 * @param input_scale       float scale of the model's input tensor.
 *                          Both come from ml_classifier_input_quant().
 *                          Must be called before audio_features_push_frame().
 */
esp_err_t audio_features_init(int8_t input_zero_point, float input_scale);

/**
 * Push one 1024-sample int32 mono PCM frame.
 * Computes window + FFT (via audio_processor) + PSD + mel projection + log10
 * + int8 quantisation, then advances the ring buffer head by one frame.
 *
 * @param pcm        AUDIO_PROC_FFT_SIZE samples of int32 audio (Q31-style).
 * @param n_samples  Must be AUDIO_PROC_FFT_SIZE; checked at runtime.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE on misuse.
 */
esp_err_t audio_features_push_frame(const int32_t *pcm, int n_samples);

/**
 * Linearise the ring buffer into [AUDIO_FEATURES_MEL_FRAMES, AUDIO_FEATURES_MEL_BANDS]
 * row-major (oldest frame = row 0, newest frame = row 31). This is the layout
 * the dummy tflite model expects ([1, 32, 40, 1]).
 *
 * @param out_mel  caller-allocated buffer of size AUDIO_FEATURES_MEL_TOTAL bytes.
 */
esp_err_t audio_features_get_melgram(int8_t *out_mel);

/**
 * @return true once at least AUDIO_FEATURES_MEL_FRAMES frames have been pushed
 *         (i.e. the ring is fully populated and a meaningful inference can
 *         be performed). Before warm-up, audio_task should skip ML invoke.
 */
bool audio_features_is_warm(void);

/** Reset to cold state. Used on I2S restart / silence-recovery. */
void audio_features_reset(void);

#ifdef __cplusplus
}
#endif
