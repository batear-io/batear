/*
 * mesh.h — controlled-flooding relay for LoRa nodes
 *
 * Every rx node keeps a ring of recently *accepted* packet keys
 * (orig_id, boot_id, seq) so copies of a packet arriving via several relays
 * are dropped before decryption. Only keys of packets that authenticated and
 * passed the freshness check are recorded, so a forged header can't block a
 * genuine packet.
 *
 * Nodes with the relay capability re-transmit accepted packets (TTL > 0,
 * not their own) after a random backoff measured in slots of roughly one
 * packet airtime, so a relay that picked a later slot hears an earlier
 * relay's copy and cancels its own (suppression). A per-hour token bucket
 * caps relay airtime (EU 868 MHz has a 1 % duty-cycle limit).
 *
 * Pure C with no ESP-IDF dependencies; time and randomness are passed in.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_FRAME_MAX  64

typedef struct {
    uint8_t  orig_id;
    uint16_t boot_id;
    uint16_t seq;
} mesh_key_t;

typedef struct {
    uint32_t relayed;        /* frames re-transmitted                          */
    uint32_t dup_dropped;    /* copies of already-accepted packets             */
    uint32_t suppressed;     /* pending relays cancelled: someone else relayed */
    uint32_t rate_limited;   /* relays dropped by the hourly budget            */
    uint32_t queue_full;     /* relays dropped because the queue was full      */
} mesh_stats_t;

typedef struct {
    uint32_t slot_ms;        /* ≥ one packet airtime                          */
    uint8_t  event_slots;    /* backoff slots for alarm/clear                  */
    uint8_t  heartbeat_slots;/* backoff slots for telemetry heartbeats         */
    uint8_t  suppress_dups;  /* cancel after hearing this many other copies    */
    uint16_t per_hour;       /* relay budget refill per hour                   */
    uint16_t burst;          /* relay budget capacity                          */
} mesh_config_t;

void mesh_init(const mesh_config_t *cfg, uint32_t now_ms);

/* True if this exact packet was already accepted (or sent by us). A copy
 * whose TTL is at or below that of our pending relay of the same packet was
 * relayed by someone else and counts towards suppression; a copy with a
 * higher TTL (e.g. the originator's own retry) does not. */
bool mesh_is_dup(const mesh_key_t *k, uint8_t ttl);

/* Record a packet as accepted (after decrypt + freshness) or as our own TX. */
void mesh_record(const mesh_key_t *k);

/* Queue a frame for relay. `frame` must already carry the decremented TTL,
 * passed again as `ttl`. `rnd` is a fresh random number. Returns false if
 * the queue is full. */
bool mesh_schedule(const mesh_key_t *k, const uint8_t *frame, size_t len, uint8_t ttl,
                   bool heartbeat, uint32_t now_ms, uint32_t rnd);

/* Milliseconds until the next pending relay is due, or -1 if none. */
int32_t mesh_next_due_ms(uint32_t now_ms);

/* Pop the next due relay that survives suppression and the rate limit.
 * Returns its length (0 if nothing to send now). */
size_t mesh_pop_due(uint32_t now_ms, uint8_t out[MESH_FRAME_MAX]);

const mesh_stats_t *mesh_stats(void);

#ifdef __cplusplus
}
#endif
