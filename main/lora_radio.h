/*
 * lora_radio.h — LoRaTask: the task that owns the SX1262 (pinned to Core 0)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Entry point for the LoRa communication task.
 * Launched via xTaskCreatePinnedToCore() in main.cpp.
 * pvParameters is unused (pass NULL).
 */
void LoRaTask(void *pvParameters);

/*
 * Ask LoRaTask to put the SX1262 to sleep and stop using it (for power-off).
 * Returns true once it confirms, false on timeout (e.g. the task never
 * started the radio).
 */
bool lora_radio_sleep(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
