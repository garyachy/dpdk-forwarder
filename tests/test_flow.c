#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <time.h>

#include <rte_eal.h>
#include <rte_hash.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>

#include "flow.h"

static void init_eal(void)
{
    char *argv[] = {
        "test_flow", "-c", "0x1",
        "--no-huge", "--no-pci",
        "-m", "512",
        "--file-prefix=test_flow",
        "--iova-mode=va", "--no-telemetry",
        "--log-level", "1",
    };
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    if (rte_eal_init(argc, argv) < 0) {
        fprintf(stderr, "rte_eal_init failed\n");
        exit(1);
    }
}

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
    assert(flow_table_init(&ft, 64, 1, 0) == 0);

    for (uint16_t i = 0; i < 64; i++) {
        struct flow_key k = make_key(i, i, i, i, 6);
        assert(flow_lookup_or_create(&ft, &k, 1) != NULL);
    }
    assert(ft.count == 64);

    /* Table full → NULL */
    struct flow_key k = make_key(999, 999, 999, 999, 6);
    assert(flow_lookup_or_create(&ft, &k, 1) == NULL);

    flow_table_free(&ft);
    printf("PASS: test_table_full\n");
}

static void test_expire(void)
{
    struct flow_table ft;
    assert(flow_table_init(&ft, 16, 2, 0) == 0);

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

    /* k1 gone — fresh insert should succeed */
    struct flow_entry *e1 = flow_lookup_or_create(&ft, &k1, 1000);
    assert(e1 != NULL);

    flow_table_free(&ft);
    printf("PASS: test_expire\n");
}

static void test_stat_counters(void)
{
    struct flow_table ft;
    assert(flow_table_init(&ft, 16, 3, 0) == 0);

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
    assert(flow_table_init(&ft, cap, 4, 0) == 0);

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

    /* Walk via rte_hash_iterate — mirrors stats_export_and_expire */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t walked = 0;
    uint32_t iter = 0;
    const void *key;
    void *data;
    int32_t pos;
    while ((pos = rte_hash_iterate(ft.ht, &key, &data, &iter)) >= 0) {
        volatile uint64_t x = ft.entries[pos].rx_bytes;
        (void)x;
        walked++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double walk_ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
                     (t1.tv_nsec - t0.tv_nsec) * 1e-6;
    assert(walked == n_flows);

    /* Expire all (timeout=0 forces every entry out) */
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

static void test_entry_prefetch(void)
{
    const uint32_t n_flows  = 1000000;
    const uint32_t cap      = 1u << 21;   /* 2 097 152, ~47% load */
    const uint16_t burst_sz = 32;
    const uint32_t n_bursts = n_flows / burst_sz;   /* 31 250 */

    struct flow_table ft;
    assert(flow_table_init(&ft, cap, 5, 0) == 0);

    int32_t *pos = malloc(n_flows * sizeof *pos);
    assert(pos);

    for (uint32_t i = 0; i < n_flows; i++) {
        struct flow_key k = make_key(i, 0x0a000001u, (uint16_t)i, 80, 6);
        struct flow_entry *e = flow_lookup_or_create(&ft, &k, i);
        assert(e != NULL);
        e->rx_bytes = (uint64_t)i * 64;
        pos[i] = rte_hash_lookup(ft.ht, &k);
        assert(pos[i] >= 0);
    }
    assert(ft.count == n_flows);

    volatile uint64_t sink = 0;
    uint64_t t0, t1;

    /* Warm-up: one sweep to bring entries into LLC */
    for (uint32_t b = 0; b < n_bursts; b++) {
        uint32_t base = b * burst_sz;
        for (uint16_t j = 0; j < burst_sz; j++)
            sink ^= ft.entries[pos[base + j]].rx_bytes;
    }

    /* Pass A — no software prefetch (steady-state, entries in LLC) */
    t0 = rte_rdtsc();
    for (uint32_t b = 0; b < n_bursts; b++) {
        uint32_t base = b * burst_sz;
        for (uint16_t j = 0; j < burst_sz; j++)
            sink ^= ft.entries[pos[base + j]].rx_bytes;
    }
    t1 = rte_rdtsc();
    uint64_t no_pf = (t1 - t0) / ((uint64_t)n_bursts * burst_sz);

    /* Pass B — full-burst software prefetch before access */
    t0 = rte_rdtsc();
    for (uint32_t b = 0; b < n_bursts; b++) {
        uint32_t base = b * burst_sz;
        for (uint16_t j = 0; j < burst_sz; j++)
            rte_prefetch0(&ft.entries[pos[base + j]]);
        for (uint16_t j = 0; j < burst_sz; j++)
            sink ^= ft.entries[pos[base + j]].rx_bytes;
    }
    t1 = rte_rdtsc();
    uint64_t with_pf = (t1 - t0) / ((uint64_t)n_bursts * burst_sz);

    int64_t delta = (int64_t)no_pf - (int64_t)with_pf;

    printf("PASS: test_entry_prefetch — 1M flows, burst=%u, entries=%.0f MB\n",
           burst_sz, (double)cap * sizeof(struct flow_entry) / (1u << 20));
    printf("      steady-state (LLC-warm) no prefetch: %" PRIu64 " cycles/access\n", no_pf);
    printf("      steady-state (LLC-warm) sw prefetch: %" PRIu64 " cycles/access\n", with_pf);
    if (delta > 0)
        printf("      improvement: %" PRId64 " cycles/access (sw prefetch hides LLC latency)\n", delta);
    else
        printf("      overhead:    %" PRId64 " cycles/access"
               " (entries fit in host LLC; prefetch loop cost dominates)\n", -delta);

    (void)sink;
    free(pos);
    flow_table_free(&ft);
}

int main(void)
{
    init_eal();
    test_insert_lookup();
    test_table_full();
    test_expire();
    test_stat_counters();
    test_scale_export();
    test_entry_prefetch();
    printf("All flow table tests passed.\n");
    rte_eal_cleanup();
    return 0;
}
