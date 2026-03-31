/*
 * drone_detector.h — shared types and inter-task queue handle
 *
 * Included by both audio_task.c (C) and lora_task.cpp (C++), so all
 * declarations are wrapped in extern "C".
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Event types sent from AudioTask → LoRaTask via the FreeRTOS queue.
 * Values are also the literal LoRa payload byte, so keep them ≤ 0xFF.
 * ---------------------------------------------------------------------- */
typedef enum {
    DRONE_EVENT_ALARM = 0x01,   /* state transition SAFE  → ALARM */
    DRONE_EVENT_CLEAR = 0x00,   /* state transition ALARM → SAFE  */
} DroneEventType;

typedef struct {
    DroneEventType type;
    float          peak_ratio;    /* harmonic confidence at the moment of transition */
    int            f0_bin;        /* fundamental bin index (or harm_ok flag)         */
    float          rms;           /* frame RMS at transition                  */
    uint32_t       timestamp_ms;  /* xTaskGetTickCount() * portTICK_PERIOD_MS */
} DroneEvent_t;

/* Defined once in main.cpp; extern'd here for both task translation units. */
extern QueueHandle_t g_drone_event_queue;

#ifdef __cplusplus
}
#endif
