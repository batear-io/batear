/*
 * ml_classifier.cpp — TFLite Micro drone classifier wrapper.
 *
 * Wires the embedded drone_v0_*.tflite model to a MicroInterpreter with a
 * minimal MicroMutableOpResolver (only the ops the dummy + planned real
 * models need). Tensor arena is heap-allocated:
 *   - PSRAM if available (LILYGO wired_detector),
 *   - else internal SRAM (Heltec V3 / V4).
 *
 * Public API is C-callable so audio_task.c (a C TU) can use it without
 * having to compile against TFLite Micro headers.
 */

#include "ml_classifier.h"
#include "audio_features.h"

#include <cmath>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "ml_clf";

/* Embedded model symbols from main/CMakeLists.txt EMBED_FILES.
 * IDF builds these symbols from the source filename (no path, dots → underscores)
 * and prepends "_binary_". For models/drone_v0_dummy.tflite the basename is
 * drone_v0_dummy.tflite → _binary_drone_v0_dummy_tflite_{start,end}. */
extern "C" const uint8_t  drone_v0_dummy_tflite_start[] asm("_binary_drone_v0_dummy_tflite_start");
extern "C" const uint8_t  drone_v0_dummy_tflite_end[]   asm("_binary_drone_v0_dummy_tflite_end");

namespace {

constexpr int kNumOps = 8;

/* Persistent state. The MicroInterpreter is held in a raw byte buffer and
 * placement-new'd in init(); this avoids needing a default constructor and
 * lets us alloc the tensor arena on the heap (PSRAM-aware) before construction. */
const tflite::Model           *g_model         = nullptr;
tflite::MicroInterpreter      *g_interpreter   = nullptr;
TfLiteTensor                  *g_input         = nullptr;
TfLiteTensor                  *g_output        = nullptr;
uint8_t                       *g_arena         = nullptr;
size_t                         g_arena_size    = 0;
size_t                         g_arena_used    = 0;
bool                           g_initialised   = false;

alignas(tflite::MicroInterpreter)
uint8_t g_interp_storage[sizeof(tflite::MicroInterpreter)];

uint8_t *alloc_arena(size_t bytes)
{
    uint8_t *p = nullptr;
#if CONFIG_SPIRAM
    p = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p != nullptr) {
        ESP_LOGI(TAG, "tensor arena: %u KB in PSRAM", (unsigned)(bytes / 1024));
        return p;
    }
    ESP_LOGW(TAG, "PSRAM arena alloc failed — falling back to internal SRAM");
#endif
    p = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (p != nullptr) {
        ESP_LOGI(TAG, "tensor arena: %u KB in internal SRAM", (unsigned)(bytes / 1024));
    }
    return p;
}

}  // namespace

extern "C" esp_err_t ml_classifier_init(void)
{
    if (g_initialised) {
        return ESP_OK;
    }

    g_model = tflite::GetModel(drone_v0_dummy_tflite_start);
    if (g_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema version %lu != TFLITE_SCHEMA_VERSION %d",
                 (unsigned long)g_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    /* MicroMutableOpResolver: register only the ops we need, to keep code size
     * down. Add more here as the real model evolves. */
    static tflite::MicroMutableOpResolver<kNumOps> s_resolver;
    s_resolver.AddConv2D();
    s_resolver.AddDepthwiseConv2D();
    s_resolver.AddMaxPool2D();
    s_resolver.AddAveragePool2D();
    s_resolver.AddMean();
    s_resolver.AddFullyConnected();
    s_resolver.AddReshape();
    s_resolver.AddSoftmax();

    g_arena_size = static_cast<size_t>(CONFIG_BATEAR_ML_TENSOR_ARENA_KB) * 1024U;
    g_arena      = alloc_arena(g_arena_size);
    if (g_arena == nullptr) {
        ESP_LOGE(TAG, "failed to allocate %u-byte tensor arena", (unsigned)g_arena_size);
        return ESP_ERR_NO_MEM;
    }

    g_interpreter = new (g_interp_storage)
        tflite::MicroInterpreter(g_model, s_resolver, g_arena, g_arena_size);

    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena too small? %u KB)",
                 (unsigned)(g_arena_size / 1024));
        return ESP_FAIL;
    }

    g_arena_used = g_interpreter->arena_used_bytes();
    g_input      = g_interpreter->input(0);
    g_output     = g_interpreter->output(0);

    /* Validate shape against what audio_features will produce. */
    if (g_input->dims->size != 4 ||
        g_input->dims->data[0] != 1 ||
        g_input->dims->data[1] != AUDIO_FEATURES_MEL_FRAMES ||
        g_input->dims->data[2] != AUDIO_FEATURES_MEL_BANDS ||
        g_input->dims->data[3] != 1 ||
        g_input->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "model input mismatch: dims=%d [%d,%d,%d,%d] type=%d "
                      "(expected [1,%d,%d,1] int8)",
                 g_input->dims->size,
                 g_input->dims->size > 0 ? g_input->dims->data[0] : -1,
                 g_input->dims->size > 1 ? g_input->dims->data[1] : -1,
                 g_input->dims->size > 2 ? g_input->dims->data[2] : -1,
                 g_input->dims->size > 3 ? g_input->dims->data[3] : -1,
                 (int)g_input->type,
                 AUDIO_FEATURES_MEL_FRAMES, AUDIO_FEATURES_MEL_BANDS);
        return ESP_FAIL;
    }

    if (g_output->type != kTfLiteInt8 ||
        g_output->dims->size < 2 ||
        g_output->dims->data[0] != 1) {
        ESP_LOGE(TAG, "model output mismatch: dims=%d type=%d",
                 g_output->dims->size, (int)g_output->type);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "init OK: model=%u B arena=%u/%u B used input=[1,%d,%d,1] output_dims=%d",
             (unsigned)(drone_v0_dummy_tflite_end - drone_v0_dummy_tflite_start),
             (unsigned)g_arena_used, (unsigned)g_arena_size,
             AUDIO_FEATURES_MEL_FRAMES, AUDIO_FEATURES_MEL_BANDS,
             g_output->dims->size);

    g_initialised = true;
    return ESP_OK;
}

extern "C" esp_err_t ml_classifier_input_quant(int8_t *zero_point, float *scale)
{
    if (!g_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (zero_point == nullptr || scale == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *zero_point = static_cast<int8_t>(g_input->params.zero_point);
    *scale      = g_input->params.scale;
    return ESP_OK;
}

extern "C" esp_err_t ml_classifier_classify(const int8_t *mel_int8,
                                            float        *out_drone_prob,
                                            int64_t      *out_invoke_us)
{
    if (!g_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mel_int8 == nullptr || out_drone_prob == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::memcpy(g_input->data.int8, mel_int8, AUDIO_FEATURES_MEL_TOTAL);

    const int64_t t0 = esp_timer_get_time();
    if (g_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "Invoke failed");
        return ESP_FAIL;
    }
    const int64_t dt_us = esp_timer_get_time() - t0;
    if (out_invoke_us != nullptr) {
        *out_invoke_us = dt_us;
    }

    /* Two output conventions are supported:
     *   - 2-class softmax : output[1] is the drone class (matches Keras Dense(2))
     *   - 1-logit sigmoid : output[0] interpreted as P(drone)
     * Both are dequantised int8 -> float in [0, 1]. */
    const int   n_out = (g_output->dims->size >= 2) ? g_output->dims->data[1] : 1;
    const float scale = g_output->params.scale;
    const int   zp    = g_output->params.zero_point;
    auto dequant = [&](int idx) {
        return scale * static_cast<float>(static_cast<int>(g_output->data.int8[idx]) - zp);
    };

    if (n_out >= 2) {
        /* Manual softmax over int8 logits (only 2 classes — fast). */
        const float l0 = dequant(0);
        const float l1 = dequant(1);
        const float m  = (l0 > l1) ? l0 : l1;
        const float e0 = expf(l0 - m);
        const float e1 = expf(l1 - m);
        *out_drone_prob = e1 / (e0 + e1);
    } else {
        const float l = dequant(0);
        *out_drone_prob = 1.0f / (1.0f + expf(-l));
    }
    return ESP_OK;
}

extern "C" size_t ml_classifier_arena_used(void)
{
    return g_arena_used;
}
