#include <signal.h>
#include <stdlib.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include "config.h"
#include "log.h"
#include "port.h"
#include "worker.h"

volatile bool force_quit;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        LOG_INFO("signal %d received, shutting down", signum);
        force_quit = true;
    }
}

int main(int argc, char **argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");

    argc -= ret;
    argv += ret;

    struct fwd_config cfg = {0};
    if (config_parse_args(argc, argv, &cfg) != 0)
        rte_exit(EXIT_FAILURE, "invalid arguments\n");

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 2 || cfg.rx_port >= nb_ports || cfg.tx_port >= nb_ports)
        rte_exit(EXIT_FAILURE,
                 "need at least 2 ports, found %u\n", nb_ports);

    /* Count available worker lcores */
    uint16_t nb_workers = 0;
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id)
        nb_workers++;

    if (nb_workers == 0)
        rte_exit(EXIT_FAILURE, "no worker lcores available (use EAL -l)\n");

    if (cfg.nb_workers == 0 || cfg.nb_workers > nb_workers)
        cfg.nb_workers = nb_workers;

    /* Pool-size sanity warning */
    uint32_t min_pool = cfg.nb_workers * cfg.burst_size * FWD_POOL_BURST_MULT;
    if (cfg.mbuf_pool_size < min_pool)
        LOG_WARN("pool-size %u may be too small (recommended >= %u)",
                 cfg.mbuf_pool_size, min_pool);

    /* Allocate mbuf pool on the socket of the RX port */
    int rx_socket = rte_eth_dev_socket_id(cfg.rx_port);
    struct rte_mempool *mbuf_pool =
        rte_pktmbuf_pool_create("mbuf_pool",
                                cfg.mbuf_pool_size,
                                FWD_MBUF_CACHE_SIZE,
                                0,
                                RTE_MBUF_DEFAULT_BUF_SIZE,
                                rx_socket);
    if (!mbuf_pool)
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create failed\n");

    int rx_queues = port_init(cfg.rx_port, cfg.nb_workers, mbuf_pool);
    if (rx_queues < 0)
        rte_exit(EXIT_FAILURE, "port_init failed for rx port %u\n", cfg.rx_port);

    int tx_queues = port_init(cfg.tx_port, cfg.nb_workers, mbuf_pool);
    if (tx_queues < 0)
        rte_exit(EXIT_FAILURE, "port_init failed for tx port %u\n", cfg.tx_port);

    /* Clamp workers to the actual queue count both ports support */
    uint16_t actual_queues = (uint16_t)RTE_MIN(rx_queues, tx_queues);
    if (cfg.nb_workers > actual_queues) {
        LOG_WARN("clamping workers %u → %u (device queue limit)",
                 cfg.nb_workers, actual_queues);
        cfg.nb_workers = actual_queues;
    }

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    LOG_INFO("starting %u worker(s), rx_port=%u tx_port=%u",
             cfg.nb_workers, cfg.rx_port, cfg.tx_port);

    /* Launch worker lcores */
    uint16_t worker_idx = 0;
    static struct worker_ctx worker_ctxs[RTE_MAX_LCORE];

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (worker_idx >= cfg.nb_workers)
            break;
        struct worker_ctx *ctx = &worker_ctxs[worker_idx];
        if (worker_init(ctx, lcore_id, worker_idx, &cfg, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "worker_init failed for lcore %u\n", lcore_id);
        rte_eal_remote_launch(worker_run, ctx, lcore_id);
        worker_idx++;
    }

    /* Main lcore: wait for shutdown signal */
    while (!force_quit)
        rte_delay_ms(FWD_MAIN_POLL_MS);

    /* Wait for all workers to finish */
    RTE_LCORE_FOREACH_WORKER(lcore_id)
        rte_eal_wait_lcore(lcore_id);

    /* Tear down ports */
    rte_eth_dev_stop(cfg.rx_port);
    rte_eth_dev_stop(cfg.tx_port);
    port_stats_print(cfg.rx_port);
    port_stats_print(cfg.tx_port);
    rte_eth_dev_close(cfg.rx_port);
    rte_eth_dev_close(cfg.tx_port);

    rte_eal_cleanup();
    return 0;
}
