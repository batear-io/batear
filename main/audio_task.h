/*
 * audio_task.h — AudioTask declaration (pinned to Core 1)
 */
#pragma once

#include <stdbool.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Entry point for the audio engine task.
 * Launched via xTaskCreatePinnedToCore() in main.cpp.
 * pvParameters is unused (pass NULL).
 */
void AudioTask(void *pvParameters);

#if CONFIG_BATEAR_LORA_NODE
/*
 * Detect whether an I2S mic is fitted: DIN is pulled down, so with no mic
 * every sample reads 0. Takes ~400 ms. Leaves the I2S peripheral released.
 */
bool audio_mic_probe(void);
#endif

#ifdef __cplusplus
}
#endif
