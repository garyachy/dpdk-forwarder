#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <rte_ether.h>

#define FWD_DEFAULT_BURST         32
#define FWD_DEFAULT_POOL_SIZE     8191   /* must be 2^n - 1 */
#define FWD_DEFAULT_MAX_FLOWS     65536
#define FWD_DEFAULT_STATS_INTERVAL 5
#define FWD_DEFAULT_FLOW_TIMEOUT  60
#define FWD_DEFAULT_OUTPUT_DIR    "."

struct fwd_config {
    uint16_t rx_port;
    uint16_t tx_port;
    uint16_t nb_workers;
    uint32_t burst_size;
    uint32_t mbuf_pool_size;
    uint32_t max_flows;
    uint32_t stats_interval_s;
    uint32_t flow_timeout_s;
    char     output_dir[256];
    struct rte_ether_addr dst_mac;
    bool     rewrite_dst_mac;
    int      log_level;
};

/* Parse application arguments (after --). Returns 0 on success. */
int config_parse_args(int argc, char **argv, struct fwd_config *cfg);
