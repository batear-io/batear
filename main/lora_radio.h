/*
 * lora_radio.h — LoRaTask: the task that owns the SX1262 (pinned to Core 0)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Entry point for the LoRa communication task.
 * Launched via xTaskCreatePinnedToCore() in main.cpp.
 * pvParameters is unused (pass NULL).
 */
void LoRaTask(void *pvParameters);

#ifdef __cplusplus
}
#endif
