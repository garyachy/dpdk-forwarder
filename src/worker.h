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
};

int  worker_init(struct worker_ctx *ctx, unsigned lcore_id,
                 uint16_t queue_idx, const struct fwd_config *cfg,
                 struct rte_mempool *mbuf_pool);
int  worker_run(void *arg);
