/*
 * eth_mqtt_task.h — W5500 Ethernet + MQTT for Wired Detector
 *
 * EthMqttTask runs on Core 0, initialises the W5500 Ethernet PHY,
 * connects to an MQTT broker, and publishes drone detection events
 * received from AudioTask via g_drone_event_queue.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void EthMqttTask(void *pvParameters);

#ifdef __cplusplus
}
#endif
