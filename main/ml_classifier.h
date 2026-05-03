/*
 * ml_classifier.h — TFLite Micro drone classifier wrapper.
 *
 * Loads the embedded drone_v0_*.tflite model, allocates a tensor arena
 * (internal SRAM on Heltec V3 / SPIRAM on LILYGO), registers the minimum
 * MicroMutableOpResolver needed for the model, and exposes a single
 * classify() call that returns the drone probability in [0, 1].
 *
 * Memory budget (Heltec V3 — no PSRAM):
 *   - Model weights live in flash rodata (~5 KB for the dummy, ~80 KB target)
 *   - Tensor arena is malloc()'d in MALLOC_CAP_INTERNAL up to
 *     CONFIG_BATEAR_ML_TENSOR_ARENA_KB (default 100 KB).
 *
 * Stage 1 (skeleton): the embedded model is the int8 dummy from
 * tools/gen_dummy_model.py. Inference is wired end-to-end so we can verify
 * the espressif/esp-tflite-micro integration, the EMBED_FILES plumbing, and
 * the actual RAM/flash usage on real hardware.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the TFLite Micro interpreter, allocate the tensor arena, and
 * verify the model's input shape matches AUDIO_FEATURES_MEL_FRAMES x
 * AUDIO_FEATURES_MEL_BANDS x 1, dtype int8.
 *
 * Idempotent — repeated calls return ESP_OK without re-allocating.
 */
esp_err_t ml_classifier_init(void);

/**
 * Quantisation parameters of the model's input tensor. Used by audio_features
 * to int8-quantise the log-mel spectrogram in the same domain as the model
 * was trained on.
 *
 * Caller must invoke ml_classifier_init() first.
 */
esp_err_t ml_classifier_input_quant(int8_t *zero_point, float *scale);

/**
 * Run inference on a quantised mel-spectrogram and return the drone probability.
 *
 * @param mel_int8        AUDIO_FEATURES_MEL_TOTAL bytes — int8 mel grid
 *                        produced by audio_features_get_melgram(). Layout:
 *                        [frames=32, bands=40], row-major.
 * @param out_drone_prob  drone probability in [0, 1]. Computed from the int8
 *                        output logits with softmax (or sigmoid if the model
 *                        outputs a single logit).
 * @param out_invoke_us   optional — set to interpreter->Invoke() wall time
 *                        in microseconds. NULL to skip.
 *
 * @return ESP_OK on success.
 */
esp_err_t ml_classifier_classify(const int8_t *mel_int8,
                                 float        *out_drone_prob,
                                 int64_t      *out_invoke_us);

/**
 * @return tensor arena bytes actually used by the latest AllocateTensors().
 *         0 before init. Useful to right-size BATEAR_ML_TENSOR_ARENA_KB.
 */
size_t ml_classifier_arena_used(void);

#ifdef __cplusplus
}
#endif
