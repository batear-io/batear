/* Host unit test for main/mesh.c — run: gcc -std=c11 -Wall -Wextra -Werror -Imain test/host/test_mesh.c main/mesh.c -o /tmp/test_mesh && /tmp/test_mesh */
#include <stdio.h>
#include "mesh.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } else printf("ok: %s\n", m); } while (0)
static mesh_config_t cfg = { .slot_ms = 600, .event_slots = 4, .heartbeat_slots = 8,
                             .suppress_dups = 1, .per_hour = 60, .burst = 3 };
int main(void) {
    uint8_t f[38] = {0x23, 5}, out[MESH_FRAME_MAX];
    mesh_key_t a = {5, 100, 7}, b = {5, 100, 8}, c = {6, 1, 7};

    mesh_init(&cfg, 0);
    CHECK(!mesh_is_dup(&a, 3), "unseen key is not a dup");
    mesh_record(&a);
    CHECK(mesh_is_dup(&a, 3) && mesh_stats()->dup_dropped == 1, "recorded key is a dup");
    CHECK(!mesh_is_dup(&b, 3) && !mesh_is_dup(&c, 3), "different seq / orig not dups");

    /* ring eviction after 64 newer keys */
    for (int i = 0; i < 64; i++) { mesh_key_t k = {9, 0, (uint16_t)i}; mesh_record(&k); }
    CHECK(!mesh_is_dup(&a, 3), "oldest key evicted after 64 records");

    /* scheduling: slot = rnd % slots, jitter = (rnd / slots) % 51 */
    mesh_init(&cfg, 1000);
    CHECK(mesh_next_due_ms(1000) == -1, "empty queue -> -1");
    CHECK(mesh_schedule(&a, f, sizeof f, 2, false, 1000, 2 + 4 * 10), "schedule event");
    CHECK(mesh_next_due_ms(1000) == 2 * 600 + 10, "event due at slot 2 + 10 ms jitter");
    CHECK(mesh_pop_due(1000 + 1209, out) == 0, "not popped before due");
    CHECK(mesh_pop_due(1000 + 1210, out) == sizeof f && out[0] == 0x23, "popped at due time with frame");
    CHECK(mesh_stats()->relayed == 1, "relayed counted");

    /* heartbeat uses heartbeat_slots */
    CHECK(mesh_schedule(&b, f, sizeof f, 2, true, 0, 7), "schedule heartbeat");
    CHECK(mesh_next_due_ms(0) == 7 * 600, "heartbeat slot 7 (beyond event slots)");
    mesh_pop_due(10000, out);

    /* suppression: hearing a relayed copy of a pending packet cancels it */
    mesh_init(&cfg, 0);
    mesh_record(&a);
    mesh_schedule(&a, f, sizeof f, 2, false, 0, 1);
    mesh_is_dup(&a, 2);
    CHECK(mesh_pop_due(5000, out) == 0 && mesh_stats()->suppressed == 1, "suppressed after another relay's copy");

    /* originator's retry (higher TTL than our pending copy) must not suppress */
    mesh_init(&cfg, 0);
    mesh_record(&a);
    mesh_schedule(&a, f, sizeof f, 2, false, 0, 1);
    mesh_is_dup(&a, 3);
    CHECK(mesh_pop_due(5000, out) == sizeof f, "originator retry (ttl 3 > pending 2) does not suppress");

    /* suppression disabled */
    mesh_config_t c2 = cfg; c2.suppress_dups = 0;
    mesh_init(&c2, 0);
    mesh_record(&a); mesh_schedule(&a, f, sizeof f, 2, false, 0, 1); mesh_is_dup(&a, 2);
    CHECK(mesh_pop_due(5000, out) == sizeof f, "suppress_dups=0 still relays");

    /* queue full */
    mesh_init(&cfg, 0);
    for (int i = 0; i < 4; i++) { mesh_key_t k = {1, 0, (uint16_t)i}; mesh_schedule(&k, f, sizeof f, 2, false, 0, 0); }
    CHECK(!mesh_schedule(&c, f, sizeof f, 2, false, 0, 0) && mesh_stats()->queue_full == 1, "5th relay rejected when queue full");

    /* rate limit: burst 3, then 60/h = one per minute */
    mesh_init(&cfg, 0);
    int sent = 0;
    for (int i = 0; i < 5; i++) { mesh_key_t k = {2, 0, (uint16_t)i}; mesh_schedule(&k, f, sizeof f, 2, false, 0, 0); sent += mesh_pop_due(0, out) ? 1 : 0; }
    CHECK(sent == 3 && mesh_stats()->rate_limited == 2, "burst of 3 then rate limited");
    mesh_key_t k1 = {3, 0, 1}; mesh_schedule(&k1, f, sizeof f, 2, false, 59999, 0);
    CHECK(mesh_pop_due(59999, out) == 0, "no token before 60 s");
    mesh_key_t k2 = {3, 0, 2}; mesh_schedule(&k2, f, sizeof f, 2, false, 60000, 0);
    CHECK(mesh_pop_due(60000, out) == sizeof f, "one token refilled after 60 s");

    /* millisecond clock wrap */
    mesh_init(&cfg, 0xFFFFFF00u);
    mesh_schedule(&a, f, sizeof f, 2, false, 0xFFFFFF00u, 1 + 4 * 0);
    CHECK(mesh_pop_due(0xFFFFFF00u + 599, out) == 0, "not due just before wrap-crossing deadline");
    CHECK(mesh_pop_due(0xFFFFFF00u + 600, out) == sizeof f, "due across uint32 ms wrap");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
