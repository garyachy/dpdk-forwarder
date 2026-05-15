#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define UNIT_TEST
#include "../src/flow.h"
#include "../src/flow.c"   /* include implementation for unit build */

static struct flow_key make_key(uint32_t sip, uint32_t dip,
                                uint16_t sp,  uint16_t dp, uint8_t proto)
{
    struct flow_key k = {0};
    k.src_ip   = sip;
    k.dst_ip   = dip;
    k.src_port = sp;
    k.dst_port = dp;
    k.proto    = proto;
    return k;
}

static void test_insert_lookup(void)
{
    struct flow_table ft;
    assert(flow_table_init(&ft, 64, 0, 0) == 0);

    struct flow_key k = make_key(0x01020304, 0x05060708, 1234, 80, 6);
    struct flow_entry *e1 = flow_lookup_or_create(&ft, &k, 100);
    assert(e1 != NULL);
    assert(ft.count == 1);

    /* Second lookup returns the same entry */
    struct flow_entry *e2 = flow_lookup_or_create(&ft, &k, 200);
    assert(e2 == e1);
    assert(e2->last_seen_tsc == 200);
    assert(ft.count == 1);

    /* Different key → new entry */
    struct flow_key k2 = make_key(0x01020304, 0x05060708, 1234, 443, 6);
    struct flow_entry *e3 = flow_lookup_or_create(&ft, &k2, 300);
    assert(e3 != NULL);
    assert(e3 != e1);
    assert(ft.count == 2);

    flow_table_free(&ft);
    printf("PASS: test_insert_lookup\n");
}

static void test_table_full(void)
{
    struct flow_table ft;
    assert(flow_table_init(&ft, 4, 0, 0) == 0);

    for (uint16_t i = 0; i < 4; i++) {
        struct flow_key k = make_key(i, i, i, i, 6);
        assert(flow_lookup_or_create(&ft, &k, 1) != NULL);
    }
    assert(ft.count == 4);

    /* Table full → NULL */
    struct flow_key k = make_key(99, 99, 99, 99, 6);
    assert(flow_lookup_or_create(&ft, &k, 1) == NULL);

    flow_table_free(&ft);
    printf("PASS: test_table_full\n");
}

static void test_expire(void)
{
    struct flow_table ft;
    assert(flow_table_init(&ft, 16, 0, 0) == 0);

    struct flow_key k1 = make_key(1, 1, 1, 1, 6);
    struct flow_key k2 = make_key(2, 2, 2, 2, 6);

    flow_lookup_or_create(&ft, &k1, 100);   /* last_seen = 100 */
    flow_lookup_or_create(&ft, &k2, 900);   /* last_seen = 900 */
    assert(ft.count == 2);

    /* Expire with timeout 500: k1 (idle 900) expires, k2 (idle 100) survives */
    flow_expire(&ft, 1000, 500);
    assert(ft.count == 1);

    /* k2 still present */
    struct flow_entry *e = flow_lookup_or_create(&ft, &k2, 1000);
    assert(e != NULL);

    /* k1 gone */
    /* After expiry the slot is cleared; a fresh insert should succeed */
    struct flow_entry *e1 = flow_lookup_or_create(&ft, &k1, 1000);
    assert(e1 != NULL);

    flow_table_free(&ft);
    printf("PASS: test_expire\n");
}

static void test_stat_counters(void)
{
    struct flow_table ft;
    assert(flow_table_init(&ft, 16, 0, 0) == 0);

    struct flow_key k = make_key(1, 2, 10, 20, 17);
    struct flow_entry *e = flow_lookup_or_create(&ft, &k, 1);
    e->rx_packets = 5;
    e->rx_bytes   = 1500;
    e->tx_packets = 5;
    e->tx_bytes   = 1500;

    struct flow_entry *e2 = flow_lookup_or_create(&ft, &k, 2);
    assert(e2->rx_packets == 5);
    assert(e2->rx_bytes   == 1500);

    flow_table_free(&ft);
    printf("PASS: test_stat_counters\n");
}

static void test_scale_export(void)
{
    const uint32_t n_flows = 1000000;
    const uint32_t cap     = 1u << 21;  /* 2 097 152 — ~47% load factor */

    struct flow_table ft;
    assert(flow_table_init(&ft, cap, 0, 0) == 0);

    struct timespec t0, t1;

    /* Insert n_flows unique flows */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t i = 0; i < n_flows; i++) {
        struct flow_key k = make_key(i, 0x0a000001u, (uint16_t)i, 80, 6);
        struct flow_entry *e = flow_lookup_or_create(&ft, &k, (uint64_t)i);
        assert(e != NULL);
        e->rx_packets = i;
        e->rx_bytes   = (uint64_t)i * 64;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double insert_ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
                       (t1.tv_nsec - t0.tv_nsec) * 1e-6;
    assert(ft.count == n_flows);

    /* Time the full linear walk of all entries (simulates export-path iteration).
     * Each occupied entry is touched to force a cache load — mirrors what
     * stats_write_row does when reading counters for CSV output. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t walked = 0;
    ut_ht_t *ht = (ut_ht_t *)ft.ht;
    for (uint32_t i = 0; i < ht->capacity; i++) {
        if (ht->occupied[i] == 1) { /* live entries only, skip tombstones */
            volatile uint64_t x = ht->slots[i].rx_bytes;
            (void)x;
            walked++;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double walk_ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
                     (t1.tv_nsec - t0.tv_nsec) * 1e-6;
    assert(walked == n_flows);

    /* Time expire-all (timeout=0 forces every entry to be deleted).
     * This is the deletion pass that follows the CSV write in the current design. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    flow_expire(&ft, (uint64_t)n_flows + 1, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double expire_ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
                       (t1.tv_nsec - t0.tv_nsec) * 1e-6;
    assert(ft.count == 0);

    printf("PASS: test_scale_export — %u flows (cap=%u)\n", n_flows, cap);
    printf("      insert=%.1f ms | walk=%.2f ms | expire=%.2f ms\n",
           insert_ms, walk_ms, expire_ms);
    printf("      worker stop-the-world estimate: %.2f ms\n",
           walk_ms + expire_ms);

    flow_table_free(&ft);
}

int main(void)
{
    test_insert_lookup();
    test_table_full();
    test_expire();
    test_stat_counters();
    test_scale_export();
    printf("All flow table tests passed.\n");
    return 0;
}
