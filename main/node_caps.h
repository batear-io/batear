/*
 * node_caps.h — runtime capabilities of a LoRa node
 *
 * One firmware image serves every LoRa deployment shape. What a node does is
 * decided at boot from NVS namespace "node_cfg" (keys cap_mic, cap_rx,
 * cap_bridge; 0 = off, 1 = on, 2 = auto), falling back to the Kconfig
 * defaults of the build's role preset.
 *
 *   mic    — run AudioTask and transmit alarm/clear/heartbeat over LoRa.
 *            auto: probe the I2S mic at boot.
 *   rx     — keep the radio listening; show received events on OLED + LED.
 *            auto: on when the node has no mic (mains-powered gateway/relay).
 *   bridge — publish received events to MQTT over Wi-Fi. Implies rx.
 *            auto: on when a Wi-Fi SSID is configured.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_CAP_OFF   0
#define NODE_CAP_ON    1
#define NODE_CAP_AUTO  2

typedef struct {
    bool mic;
    bool rx;
    bool bridge;
} node_caps_t;

/* Resolve capabilities once at boot. Call after nvs_flash_init(). */
void node_caps_init(void);

const node_caps_t *node_caps(void);

#ifdef __cplusplus
}
#endif
