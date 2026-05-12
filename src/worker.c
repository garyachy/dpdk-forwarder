#include <string.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_ether.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_prefetch.h>

#include "worker.h"
#include "flow.h"
#include "stats.h"
#include "log.h"

static void parse_key(struct rte_mbuf *pkt, struct flow_key *key,
                      bool *is_ip)
{
    memset(key, 0, sizeof(*key));
    *is_ip = false;

    struct rte_ether_hdr *eth =
        rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return;

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    key->src_ip = ip->src_addr;
    key->dst_ip = ip->dst_addr;
    key->proto  = ip->next_proto_id;

    if (ip->next_proto_id == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcp =
            (struct rte_tcp_hdr *)((uint8_t *)ip + rte_ipv4_hdr_len(ip));
        key->src_port = tcp->src_port;
        key->dst_port = tcp->dst_port;
    } else if (ip->next_proto_id == IPPROTO_UDP) {
        struct rte_udp_hdr *udp =
            (struct rte_udp_hdr *)((uint8_t *)ip + rte_ipv4_hdr_len(ip));
        key->src_port = udp->src_port;
        key->dst_port = udp->dst_port;
    }

    *is_ip = true;
}

int worker_init(struct worker_ctx *ctx, unsigned lcore_id,
                uint16_t queue_idx, const struct fwd_config *cfg,
                struct rte_mempool *mbuf_pool)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->lcore_id    = lcore_id;
    ctx->rx_queue_id = queue_idx;
    ctx->tx_queue_id = queue_idx;
    ctx->rx_port     = cfg->rx_port;
    ctx->tx_port     = cfg->tx_port;
    ctx->mbuf_pool   = mbuf_pool;
    ctx->cfg         = cfg;

    uint64_t hz = rte_get_tsc_hz();
    ctx->export_tsc_interval = cfg->stats_interval_s * hz;
    ctx->timeout_tsc         = cfg->flow_timeout_s   * hz;
    ctx->last_export_tsc     = rte_rdtsc();
    ctx->perf.interval_tsc   = ctx->last_export_tsc;

    int socket = rte_lcore_to_socket_id(lcore_id);
    if (flow_table_init(&ctx->ftable, cfg->max_flows, lcore_id, socket) != 0)
        return -1;

    snprintf(ctx->csv_path, sizeof(ctx->csv_path) - 1,
             "%s/flow_stats_core_%u.csv", cfg->output_dir, lcore_id);

    ctx->csv_file = fopen(ctx->csv_path, "a");
    if (!ctx->csv_file) {
        LOG_ERR("cannot open %s", ctx->csv_path);
        return -1;
    }

    /* Write header if the file is new */
    if (ftell(ctx->csv_file) == 0)
        fprintf(ctx->csv_file,
                "timestamp,src_ip,dst_ip,src_port,dst_port,"
                "proto,rx_bytes,tx_bytes,rx_packets,tx_packets\n");

    LOG_INFO("worker lcore %u: queue %u, csv=%s", lcore_id, queue_idx, ctx->csv_path);
    return 0;
}

int worker_run(void *arg)
{
    struct worker_ctx *ctx = arg;
    const struct fwd_config *cfg = ctx->cfg;
    const uint16_t rx_port  = ctx->rx_port;
    const uint16_t tx_port  = ctx->tx_port;
    const uint16_t rx_queue = ctx->rx_queue_id;
    const uint16_t tx_queue = ctx->tx_queue_id;
    const uint32_t burst    = cfg->burst_size;

    struct rte_mbuf    *rx_pkts[512];
    struct rte_mbuf    *tx_pkts[512];
    struct flow_entry  *tx_flows[512]; /* flow entry per tx_pkt — no re-lookup */

    LOG_INFO("worker lcore %u started", ctx->lcore_id);

    while (!force_quit) {
        uint64_t now_tsc = rte_rdtsc();

        /* Export and expire even when idle so stats are flushed after traffic bursts */
        if (now_tsc - ctx->last_export_tsc > ctx->export_tsc_interval) {
            stats_export_and_expire(ctx, now_tsc);
            ctx->last_export_tsc   = now_tsc;
            ctx->table_full_warned = false;
        }

        uint16_t nb_rx = rte_eth_rx_burst(rx_port, rx_queue, rx_pkts, burst);
        if (nb_rx == 0) {
            ctx->perf.idle_polls++;
            continue;
        }
        ctx->perf.active_polls++;

        /* ── per-burst processing: parse → lookup → update → build tx_buf ── */
        uint64_t proc_start = rte_rdtsc();
        uint16_t nb_tx = 0;

        for (uint16_t i = 0; i < nb_rx; i++) {
            /* Prefetch next packet's headers one iteration ahead */
            if (i + 1 < nb_rx)
                rte_prefetch0(rte_pktmbuf_mtod(rx_pkts[i + 1], void *));

            struct rte_mbuf *pkt = rx_pkts[i];
            struct flow_key key;
            bool is_ip;

            parse_key(pkt, &key, &is_ip);

            if (!is_ip) {
                rte_pktmbuf_free(pkt);
                continue;
            }

            struct flow_entry *e =
                flow_lookup_or_create(&ctx->ftable, &key, now_tsc);

            if (!e) {
                if (!ctx->table_full_warned) {
                    LOG_WARN("flow table full on core %u, dropping packets",
                             ctx->lcore_id);
                    ctx->table_full_warned = true;
                }
                rte_pktmbuf_free(pkt);
                continue;
            }

            e->rx_packets++;
            e->rx_bytes += rte_pktmbuf_pkt_len(pkt);

            if (cfg->rewrite_dst_mac) {
                struct rte_ether_hdr *eth =
                    rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
                rte_ether_addr_copy(&cfg->dst_mac, &eth->dst_addr);
            }

            /* Store entry pointer — reused for TX stats, avoids re-lookup */
            tx_flows[nb_tx] = e;
            tx_pkts[nb_tx++] = pkt;
        }

        /* Measure only application processing cycles, not PMD I/O */
        ctx->perf.proc_cycles += rte_rdtsc() - proc_start;
        ctx->perf.rx_packets  += nb_rx;

        if (nb_tx == 0)
            continue;

        uint16_t nb_sent = rte_eth_tx_burst(tx_port, tx_queue, tx_pkts, nb_tx);

        /* Update TX stats using stored pointers — no re-parse, no re-lookup */
        for (uint16_t i = 0; i < nb_sent; i++) {
            tx_flows[i]->tx_packets++;
            tx_flows[i]->tx_bytes += rte_pktmbuf_pkt_len(tx_pkts[i]);
        }

        ctx->perf.tx_packets += nb_sent;

        /* Free unsent packets */
        for (uint16_t i = nb_sent; i < nb_tx; i++)
            rte_pktmbuf_free(tx_pkts[i]);
    }

    /* Final export on shutdown */
    stats_export_and_expire(ctx, rte_rdtsc());
    fclose(ctx->csv_file);
    flow_table_free(&ctx->ftable);

    LOG_INFO("worker lcore %u exiting", ctx->lcore_id);
    return 0;
}
