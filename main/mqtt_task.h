/*
 * mqtt_task.h — WiFi + MQTT + Home Assistant discovery (gateway only)
 *
 * MqttTask runs on Core 1, receives detection events from
 * node_rx (LoRaTask) via a FreeRTOS queue and publishes to MQTT.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  device_id;
    uint8_t  event_type;      /* 0=CLEAR 1=ALARM 2=TELEMETRY (heartbeat) */
    bool     alarm;           /* cached ALARM state; ignored on heartbeat */
    float    rssi;
    float    snr;
    uint8_t  rms_db;
    uint8_t  f0_bin;
    uint16_t seq;

    /* Telemetry piggybacked on every packet. */
    uint16_t vbat_mv;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint16_t uptime_min;
    uint16_t free_heap_kb;
    uint8_t  tx_fail_count;
    uint8_t  flags;
} MqttEvent_t;

/* Queue populated by node_rx, consumed by MqttTask. */
extern QueueHandle_t g_mqtt_event_queue;

void MqttTask(void *pvParameters);

#ifdef __cplusplus
}
#endif
