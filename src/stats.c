#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#include "stats.h"
#include "flow.h"
#include "log.h"

void stats_write_header(FILE *f)
{
    fprintf(f, "timestamp,src_ip,dst_ip,src_port,dst_port,"
               "proto,rx_bytes,tx_bytes,rx_packets,tx_packets\n");
}

void stats_write_row(FILE *f, const struct flow_entry *e)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    char tbuf[32];
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);

    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &e->key.src_ip, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &e->key.dst_ip, dst_ip, sizeof(dst_ip));

    fprintf(f, "%s,%s,%s,%u,%u,%u,%" PRIu64 ",%" PRIu64
               ",%" PRIu64 ",%" PRIu64 "\n",
            tbuf,
            src_ip, dst_ip,
            ntohs(e->key.src_port), ntohs(e->key.dst_port),
            e->key.proto,
            e->rx_bytes, e->tx_bytes,
            e->rx_packets, e->tx_packets);
}

#ifndef UNIT_TEST

#include <rte_hash.h>
#include "worker.h"

void stats_export_and_expire(struct worker_ctx *ctx, uint64_t now_tsc)
{
    struct flow_table *ft = &ctx->ftable;
    FILE *f = ctx->csv_file;
    uint64_t timeout_tsc = ctx->timeout_tsc;

    uint32_t iter = 0;
    const void *key;
    void *data;
    int32_t pos;

    while ((pos = rte_hash_iterate(ft->ht, &key, &data, &iter)) >= 0) {
        struct flow_entry *e = &ft->entries[pos];

        if (fprintf(f, "") < 0) {
            /* fprintf itself handles the row; just detect errors */
        }
        stats_write_row(f, e);

        if (now_tsc - e->last_seen_tsc > timeout_tsc) {
            rte_hash_del_key(ft->ht, key);
            memset(e, 0, sizeof(*e));
            ft->count--;
        }
    }

    if (fflush(f) != 0)
        LOG_WARN("fflush failed on %s", ctx->csv_path);
}

#endif /* !UNIT_TEST */
