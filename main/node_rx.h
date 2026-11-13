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

typedef enum {
    NODE_RX_BAD,     /* wrong length / version, or failed authentication */
    NODE_RX_STALE,   /* authentic but not newer than the last accepted     */
    NODE_RX_OK,      /* accepted and delivered                             */
} node_rx_result_t;

/* Handle one frame received over the air. On NODE_RX_OK, *event_type is the
 * decrypted event type (LORA_EVENT_*). */
node_rx_result_t node_rx_packet(const uint8_t *buf, size_t len, float rssi, float snr,
                                uint8_t *event_type);

/* Handle an event this node originated itself (it can't hear its own TX). */
void node_rx_local(const lora_header_t *hdr, const lora_plaintext_t *pt);

/* Show a fatal message (e.g. radio init failure) on the OLED. */
void node_rx_fatal(const char *msg);

/* Count a radio-level receive error on the status line. */
void node_rx_error(void);

/* Light the OLED for its on-time (button press). No-op if the display
 * was never initialised (rx off). */
void node_rx_wake_display(void);

#ifdef __cplusplus
}
#endif
