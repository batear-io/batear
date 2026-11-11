/*
 * mesh.c — dedup ring, relay queue with slotted backoff, suppression and an
 * hourly relay budget. See mesh.h.
 */

#include "mesh.h"
#include <string.h>

#define MESH_RING_LEN   64
#define MESH_QUEUE_LEN   4
#define MESH_JITTER_MS  50
#define MS_PER_HOUR     3600000ULL

typedef struct {
    bool       used;
    mesh_key_t key;
    uint8_t    frame[MESH_FRAME_MAX];
    size_t     len;
    uint32_t   due_ms;
    uint8_t    ttl;
    uint8_t    dups;
} pending_t;

static mesh_config_t s_cfg;
static mesh_key_t    s_ring[MESH_RING_LEN];
static uint8_t       s_ring_n;
static uint8_t       s_ring_head;
static pending_t     s_queue[MESH_QUEUE_LEN];
static mesh_stats_t  s_stats;

/* Token bucket in units of 1/3600000 relay, so refill is exact integer math:
 * per_hour tokens per hour = per_hour units per ms. */
static uint64_t s_bucket;
static uint32_t s_bucket_ms;

static bool key_eq(const mesh_key_t *a, const mesh_key_t *b)
{
    return a->orig_id == b->orig_id && a->boot_id == b->boot_id && a->seq == b->seq;
}

/* Wrap-safe "a is at or after b" for millisecond timestamps. */
static bool time_reached(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

void mesh_init(const mesh_config_t *cfg, uint32_t now_ms)
{
    s_cfg = *cfg;
    if (s_cfg.event_slots == 0) s_cfg.event_slots = 1;
    if (s_cfg.heartbeat_slots == 0) s_cfg.heartbeat_slots = 1;
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_n = 0;
    s_ring_head = 0;
    memset(s_queue, 0, sizeof(s_queue));
    memset(&s_stats, 0, sizeof(s_stats));
    s_bucket = (uint64_t)s_cfg.burst * MS_PER_HOUR;
    s_bucket_ms = now_ms;
}

bool mesh_is_dup(const mesh_key_t *k, uint8_t ttl)
{
    bool dup = false;
    for (uint8_t i = 0; i < s_ring_n; i++) {
        if (key_eq(&s_ring[i], k)) {
            dup = true;
            break;
        }
    }
    if (!dup) {
        return false;
    }
    s_stats.dup_dropped++;
    for (int i = 0; i < MESH_QUEUE_LEN; i++) {
        if (s_queue[i].used && key_eq(&s_queue[i].key, k) &&
            ttl <= s_queue[i].ttl && s_queue[i].dups < 255) {
            s_queue[i].dups++;
        }
    }
    return true;
}

void mesh_record(const mesh_key_t *k)
{
    s_ring[s_ring_head] = *k;
    s_ring_head = (uint8_t)((s_ring_head + 1) % MESH_RING_LEN);
    if (s_ring_n < MESH_RING_LEN) {
        s_ring_n++;
    }
}

bool mesh_schedule(const mesh_key_t *k, const uint8_t *frame, size_t len, uint8_t ttl,
                   bool heartbeat, uint32_t now_ms, uint32_t rnd)
{
    if (len == 0 || len > MESH_FRAME_MAX) {
        return false;
    }
    for (int i = 0; i < MESH_QUEUE_LEN; i++) {
        if (s_queue[i].used) {
            continue;
        }
        uint8_t  slots = heartbeat ? s_cfg.heartbeat_slots : s_cfg.event_slots;
        uint32_t slot  = rnd % slots;
        uint32_t jit   = (rnd / slots) % (MESH_JITTER_MS + 1);
        pending_t *p = &s_queue[i];
        p->used   = true;
        p->key    = *k;
        memcpy(p->frame, frame, len);
        p->len    = len;
        p->due_ms = now_ms + slot * s_cfg.slot_ms + jit;
        p->ttl    = ttl;
        p->dups   = 0;
        return true;
    }
    s_stats.queue_full++;
    return false;
}

int32_t mesh_next_due_ms(uint32_t now_ms)
{
    int32_t best = -1;
    for (int i = 0; i < MESH_QUEUE_LEN; i++) {
        if (!s_queue[i].used) {
            continue;
        }
        int32_t d = (int32_t)(s_queue[i].due_ms - now_ms);
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

static bool take_token(uint32_t now_ms)
{
    uint64_t cap = (uint64_t)s_cfg.burst * MS_PER_HOUR;
    uint32_t dt  = now_ms - s_bucket_ms;
    s_bucket_ms  = now_ms;
    s_bucket    += (uint64_t)dt * s_cfg.per_hour;
    if (s_bucket > cap) s_bucket = cap;
    if (s_bucket < MS_PER_HOUR) {
        return false;
    }
    s_bucket -= MS_PER_HOUR;
    return true;
}

size_t mesh_pop_due(uint32_t now_ms, uint8_t out[MESH_FRAME_MAX])
{
    for (int i = 0; i < MESH_QUEUE_LEN; i++) {
        pending_t *p = &s_queue[i];
        if (!p->used || !time_reached(now_ms, p->due_ms)) {
            continue;
        }
        p->used = false;
        if (s_cfg.suppress_dups > 0 && p->dups >= s_cfg.suppress_dups) {
            s_stats.suppressed++;
            continue;
        }
        if (!take_token(now_ms)) {
            s_stats.rate_limited++;
            continue;
        }
        memcpy(out, p->frame, p->len);
        s_stats.relayed++;
        return p->len;
    }
    return 0;
}

const mesh_stats_t *mesh_stats(void)
{
    return &s_stats;
}
