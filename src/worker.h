#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <rte_mempool.h>

#include "config.h"
#include "flow.h"

extern volatile bool force_quit;

struct worker_ctx {
    unsigned            lcore_id;
    uint16_t            rx_queue_id;
    uint16_t            tx_queue_id;
    uint16_t            rx_port;
    uint16_t            tx_port;
    struct flow_table   ftable;
    struct rte_mempool *mbuf_pool;
    const struct fwd_config *cfg;
    uint64_t            last_export_tsc;
    uint64_t            export_tsc_interval; /* pre-computed */
    uint64_t            timeout_tsc;         /* pre-computed */
    FILE               *csv_file;
    char                csv_path[512];
    /* throttle "table full" warning: emit at most once per export interval */
    bool                table_full_warned;

    /* per-interval performance counters (reset each export cycle) */
    struct {
        uint64_t proc_cycles;   /* TSC cycles spent in active rx+process+tx */
        uint64_t rx_packets;    /* packets received this interval */
        uint64_t tx_packets;    /* packets forwarded this interval */
        uint64_t active_polls;  /* rx_burst calls that returned > 0 */
        uint64_t idle_polls;    /* rx_burst calls that returned 0 */
        uint64_t interval_tsc;  /* TSC at start of this interval */
    } perf;
};

int  worker_init(struct worker_ctx *ctx, unsigned lcore_id,
                 uint16_t queue_idx, const struct fwd_config *cfg,
                 struct rte_mempool *mbuf_pool);
int  worker_run(void *arg);
