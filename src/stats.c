#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <arpa/inet.h>

#include "stats.h"
#include "flow.h"
#include "log.h"

void stats_write_header(FILE *f)
{
    fprintf(f, "timestamp,src_ip,dst_ip,src_port,dst_port,"
               "proto,rx_bytes,tx_bytes,rx_packets,tx_packets\n");
}

void stats_write_row(FILE *f, const struct flow_entry *e, const char *ts)
{
    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &e->key.src_ip, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &e->key.dst_ip, dst_ip, sizeof(dst_ip));

    fprintf(f, "%s,%s,%s,%u,%u,%u,%" PRIu64 ",%" PRIu64
               ",%" PRIu64 ",%" PRIu64 "\n",
            ts,
            src_ip, dst_ip,
            ntohs(e->key.src_port), ntohs(e->key.dst_port),
            e->key.proto,
            e->rx_bytes, e->tx_bytes,
            e->rx_packets, e->tx_packets);
}

#ifndef UNIT_TEST

#include <rte_hash.h>
#include <rte_cycles.h>
#include <rte_rcu_qsbr.h>
#include "worker.h"

/*
 * Called by the main lcore on each stats interval.
 *
 * Pass 1: iterate the hash table — write a CSV row for every active flow and
 *         collect the keys of flows that have timed out.
 * Synchronize: wait for all worker lcores to pass a quiescent point so no
 *              worker holds a stale hash-table position from a lookup that
 *              finished before we called synchronize.
 * Pass 2: delete the collected expired keys.
 *
 * This never pauses the worker lcores — they keep forwarding throughout.
 */
void stats_export_and_expire(struct worker_ctx *ctx, uint64_t now_tsc)
{
    struct flow_table *ft = &ctx->ftable;
    FILE *f = ctx->csv_file;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char tbuf[32];
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);

    uint32_t iter = 0;
    const void *key;
    void *data;
    int32_t pos;
    uint32_t nb_expired = 0;

    while ((pos = rte_hash_iterate(ft->ht, &key, &data, &iter)) >= 0) {
        struct flow_entry *e = &ft->entries[pos];
        stats_write_row(f, e, tbuf);

        if (now_tsc - e->last_seen_tsc > ctx->timeout_tsc)
            ctx->expired_keys[nb_expired++] = e->key;
    }

    if (fflush(f) != 0)
        LOG_WARN("fflush failed on %s", ctx->csv_path);

    if (nb_expired > 0) {
        /*
         * Wait for all worker lcores to complete their current burst before
         * deleting.  Workers call rte_rcu_qsbr_quiescent() after each burst,
         * so this returns within one burst latency (~10 µs at 1.8 Mpps).
         */
        rte_rcu_qsbr_synchronize(ctx->qsv, RTE_QSBR_THRID_INVALID);

        for (uint32_t i = 0; i < nb_expired; i++) {
            int32_t p = rte_hash_del_key(ft->ht, &ctx->expired_keys[i]);
            if (p >= 0) {
                memset(&ft->entries[p], 0, sizeof(ft->entries[p]));
                ft->count--;
            }
        }
    }
}

#endif /* !UNIT_TEST */
