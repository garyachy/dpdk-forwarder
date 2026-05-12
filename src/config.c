#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <rte_ethdev.h>

#include "config.h"

static const struct option long_opts[] = {
    { "rx-port",        required_argument, NULL, 'r' },
    { "tx-port",        required_argument, NULL, 't' },
    { "workers",        required_argument, NULL, 'w' },
    { "burst",          required_argument, NULL, 'b' },
    { "pool-size",      required_argument, NULL, 'p' },
    { "max-flows",      required_argument, NULL, 'f' },
    { "stats-interval", required_argument, NULL, 's' },
    { "flow-timeout",   required_argument, NULL, 'o' },
    { "output-dir",     required_argument, NULL, 'd' },
    { "dst-mac",        required_argument, NULL, 'm' },
    { "log-level",      required_argument, NULL, 'l' },
    { NULL, 0, NULL, 0 }
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [EAL opts] -- [app opts]\n"
        "  --rx-port N          Ingress DPDK port index (default 0)\n"
        "  --tx-port N          Egress  DPDK port index (default 1)\n"
        "  --workers N          Worker lcore count (default: EAL lcores - 1)\n"
        "  --burst N            RX/TX burst size (default %u)\n"
        "  --pool-size N        Mbuf pool entries (default %u)\n"
        "  --max-flows N        Flow table capacity per core (default %u)\n"
        "  --stats-interval N   Export interval in seconds (default %u)\n"
        "  --flow-timeout N     Inactivity timeout in seconds (default %u)\n"
        "  --output-dir PATH    CSV output directory (default '%s')\n"
        "  --dst-mac MAC        Rewrite destination MAC (XX:XX:XX:XX:XX:XX)\n"
        "  --log-level N        0=ERR 1=WARN 2=INFO 3=DEBUG (default 2)\n",
        prog,
        FWD_DEFAULT_BURST,
        FWD_DEFAULT_POOL_SIZE,
        FWD_DEFAULT_MAX_FLOWS,
        FWD_DEFAULT_STATS_INTERVAL,
        FWD_DEFAULT_FLOW_TIMEOUT,
        FWD_DEFAULT_OUTPUT_DIR);
}

int config_parse_args(int argc, char **argv, struct fwd_config *cfg)
{
    /* Defaults */
    cfg->rx_port         = 0;
    cfg->tx_port         = 1;
    cfg->nb_workers      = 0;   /* 0 = auto */
    cfg->burst_size      = FWD_DEFAULT_BURST;
    cfg->mbuf_pool_size  = FWD_DEFAULT_POOL_SIZE;
    cfg->max_flows       = FWD_DEFAULT_MAX_FLOWS;
    cfg->stats_interval_s = FWD_DEFAULT_STATS_INTERVAL;
    cfg->flow_timeout_s  = FWD_DEFAULT_FLOW_TIMEOUT;
    cfg->rewrite_dst_mac = false;
    cfg->log_level       = 2;
    strncpy(cfg->output_dir, FWD_DEFAULT_OUTPUT_DIR, sizeof(cfg->output_dir) - 1);

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r': cfg->rx_port          = (uint16_t)atoi(optarg); break;
        case 't': cfg->tx_port          = (uint16_t)atoi(optarg); break;
        case 'w': cfg->nb_workers       = (uint16_t)atoi(optarg); break;
        case 'b': cfg->burst_size       = (uint32_t)atoi(optarg); break;
        case 'p': cfg->mbuf_pool_size   = (uint32_t)atoi(optarg); break;
        case 'f': cfg->max_flows        = (uint32_t)atoi(optarg); break;
        case 's': cfg->stats_interval_s = (uint32_t)atoi(optarg); break;
        case 'o': cfg->flow_timeout_s   = (uint32_t)atoi(optarg); break;
        case 'd': strncpy(cfg->output_dir, optarg, sizeof(cfg->output_dir) - 1); break;
        case 'l': cfg->log_level        = atoi(optarg); break;
        case 'm':
            if (rte_ether_unformat_addr(optarg, &cfg->dst_mac) != 0) {
                fprintf(stderr, "Invalid MAC address: %s\n", optarg);
                return -1;
            }
            cfg->rewrite_dst_mac = true;
            break;
        default:
            usage(argv[0]);
            return -1;
        }
    }

    /* Validation */
    if (cfg->rx_port == cfg->tx_port) {
        fprintf(stderr, "rx-port and tx-port must differ\n");
        return -1;
    }
    if (cfg->burst_size == 0 || cfg->burst_size > 512) {
        fprintf(stderr, "burst must be in [1, 512]\n");
        return -1;
    }
    if (cfg->stats_interval_s == 0) {
        fprintf(stderr, "stats-interval must be > 0\n");
        return -1;
    }
    if (cfg->flow_timeout_s == 0) {
        fprintf(stderr, "flow-timeout must be > 0\n");
        return -1;
    }

    return 0;
}
