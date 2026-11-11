/*
 * node_rx.h — handling of received LoRa packets (decrypt, replay check,
 * OLED + LED, MQTT hand-off). Used by nodes with the rx capability.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "lora_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power the display, init OLED + LED, show the idle screen. */
void node_rx_init(void);

/* Handle one frame received over the air. */
void node_rx_packet(const uint8_t *buf, size_t len, float rssi, float snr);

/* Handle an event this node originated itself (it can't hear its own TX). */
void node_rx_local(const lora_header_t *hdr, const lora_plaintext_t *pt);

/* Show a fatal message (e.g. radio init failure) on the OLED. */
void node_rx_fatal(const char *msg);

/* Count a radio-level receive error on the status line. */
void node_rx_error(void);

#ifdef __cplusplus
}
#endif
