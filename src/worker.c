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
#include <rte_hash.h>
#include <rte_malloc.h>
#include <rte_rcu_qsbr.h>

#include "config.h"
#include "worker.h"
#include "flow.h"
#include "stats.h"
#include "log.h"

/*
 * Idle-path TSC throttle: only call rte_rdtsc() every N idle rx_burst calls.
 * Eliminates the serialising TSC read from the empty-queue spin loop.
 */
#define IDLE_EXPORT_BATCH 1000000UL

/* ── Per-packet header parser ───────────────────────────────────────────── */

static __rte_always_inline void parse_key(struct rte_mbuf *pkt,
                                          struct flow_key *key, bool *is_ip)
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

/* ── Burst pipeline phases ──────────────────────────────────────────────── */

/*
 * Phase 1: parse each packet header into a flow_key.
 * Drops non-IPv4 packets immediately. Returns count of IPv4 packets.
 */
static __rte_always_inline uint16_t
burst_parse(struct rte_mbuf **rx_pkts, uint16_t nb_rx,
            struct flow_key *keys, const void **key_ptrs, uint16_t *ip_pkts)
{
    uint16_t nb_ip = 0;
    for (uint16_t i = 0; i < nb_rx; i++) {
        if (i + 1 < nb_rx)
            rte_prefetch0(rte_pktmbuf_mtod(rx_pkts[i + 1], void *));

        bool is_ip;
        parse_key(rx_pkts[i], &keys[nb_ip], &is_ip);
        if (!is_ip) {
            rte_pktmbuf_free(rx_pkts[i]);
            continue;
        }
        key_ptrs[nb_ip] = &keys[nb_ip];
        ip_pkts[nb_ip]  = i;
        nb_ip++;
    }
    return nb_ip;
}

/*
 * Phase 3: for each IPv4 packet, find-or-create its flow entry, update RX
 * counters, optionally rewrite the dst MAC, and build the TX burst array.
 * Returns the number of packets ready to transmit.
 */
static __rte_always_inline uint16_t
burst_flow_update(struct worker_ctx *ctx,
                  struct rte_mbuf **rx_pkts,
                  struct rte_mbuf **tx_pkts,
                  struct flow_entry **tx_flows,
                  const struct flow_key *keys,
                  const void **key_ptrs,
                  const uint16_t *ip_pkts,
                  const int32_t *positions,
                  uint16_t nb_ip,
                  uint64_t now_tsc)
{
    const struct fwd_config *cfg = ctx->cfg;
    uint16_t nb_tx = 0;

    for (uint16_t j = 0; j < nb_ip; j++) {
        struct rte_mbuf *pkt = rx_pkts[ip_pkts[j]];
        int32_t pos = positions[j];
        struct flow_entry *e;

        if (likely(pos >= 0)) {
            e = &ctx->ftable.entries[pos];
            e->last_seen_tsc = now_tsc;
        } else {
            if (ctx->ftable.count >= ctx->ftable.capacity) {
                if (!ctx->table_full_warned) {
                    LOG_WARN("flow table full on core %u, dropping packets",
                             ctx->lcore_id);
                    ctx->table_full_warned = true;
                }
                rte_pktmbuf_free(pkt);
                continue;
            }
            pos = rte_hash_add_key(ctx->ftable.ht, key_ptrs[j]);
            if (unlikely(pos < 0)) {
                rte_pktmbuf_free(pkt);
                continue;
            }
            e = &ctx->ftable.entries[pos];
            memset(e, 0, sizeof(*e));
            e->key           = keys[j];
            e->created_tsc   = now_tsc;
            e->last_seen_tsc = now_tsc;
            ctx->ftable.count++;
        }

        e->rx_packets++;
        e->rx_bytes += rte_pktmbuf_pkt_len(pkt);

        if (cfg->rewrite_dst_mac) {
            struct rte_ether_hdr *eth =
                rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
            rte_ether_addr_copy(&cfg->dst_mac, &eth->dst_addr);
        }

        tx_flows[nb_tx] = e;
        tx_pkts[nb_tx++] = pkt;
    }
    return nb_tx;
}

/*
 * Post-TX: update per-flow TX counters and free packets the PMD couldn't send.
 */
static __rte_always_inline void
burst_tx_stats(struct worker_ctx *ctx,
               struct rte_mbuf **tx_pkts,
               struct flow_entry **tx_flows,
               uint16_t nb_sent, uint16_t nb_tx)
{
    for (uint16_t i = 0; i < nb_sent; i++) {
        tx_flows[i]->tx_packets++;
        tx_flows[i]->tx_bytes += rte_pktmbuf_pkt_len(tx_pkts[i]);
    }
    for (uint16_t i = nb_sent; i < nb_tx; i++)
        rte_pktmbuf_free(tx_pkts[i]);
}

/*
 * Log the per-interval performance summary and reset accumulators.
 */
static __rte_always_inline void
worker_check_perf_log(struct worker_ctx *ctx, uint64_t now_tsc)
{
    if (likely(now_tsc - ctx->last_export_tsc <= ctx->export_tsc_interval))
        return;

    uint64_t hz = rte_get_tsc_hz();
    uint64_t elapsed_tsc = now_tsc - ctx->perf.interval_tsc;
    double elapsed_s  = hz ? (double)elapsed_tsc / hz : 1.0;
    uint64_t cpp      = ctx->perf.rx_packets ?
                        ctx->perf.proc_cycles / ctx->perf.rx_packets : 0;
    double mpps       = ctx->perf.rx_packets / elapsed_s / 1e6;
    uint64_t total    = ctx->perf.active_polls + ctx->perf.idle_polls;
    double poll_eff   = total ? 100.0 * ctx->perf.active_polls / total : 0.0;

    LOG_INFO("[perf] core %u | %" PRIu64 " cycles/pkt | %.3f Mpps"
             " | rx=%" PRIu64 " tx=%" PRIu64 " | poll_eff=%.1f%%",
             ctx->lcore_id, cpp, mpps,
             ctx->perf.rx_packets, ctx->perf.tx_packets, poll_eff);

    memset(&ctx->perf, 0, sizeof(ctx->perf));
    ctx->perf.interval_tsc = now_tsc;
    ctx->last_export_tsc   = now_tsc;
    ctx->table_full_warned = false;
}

/* ── Per-burst scratch buffers ──────────────────────────────────────────── */

struct burst_ctx {
    struct rte_mbuf   *rx_pkts[FWD_MAX_BURST];
    struct rte_mbuf   *tx_pkts[FWD_MAX_BURST];
    struct flow_entry *tx_flows[FWD_MAX_BURST];
    struct flow_key    keys[FWD_MAX_BURST];
    const void        *key_ptrs[FWD_MAX_BURST];
    uint16_t           ip_pkts[FWD_MAX_BURST];
    int32_t            positions[FWD_MAX_BURST];
};

/* ── worker_init ────────────────────────────────────────────────────────── */

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

    ctx->expired_keys = rte_malloc_socket("expired_keys",
                                          cfg->max_flows * sizeof(struct flow_key),
                                          RTE_CACHE_LINE_SIZE, socket);
    if (!ctx->expired_keys) {
        LOG_ERR("rte_malloc_socket failed for expired_keys (lcore %u)", lcore_id);
        flow_table_free(&ctx->ftable);
        return -1;
    }

    snprintf(ctx->csv_path, sizeof(ctx->csv_path) - 1,
             "%s/flow_stats_core_%u.csv", cfg->output_dir, lcore_id);

    ctx->csv_file = fopen(ctx->csv_path, "a");
    if (!ctx->csv_file) {
        LOG_ERR("cannot open %s", ctx->csv_path);
        return -1;
    }

    fseek(ctx->csv_file, 0, SEEK_END);
    if (ftell(ctx->csv_file) == 0) {
        fprintf(ctx->csv_file,
                "timestamp,src_ip,dst_ip,src_port,dst_port,"
                "proto,rx_bytes,tx_bytes,rx_packets,tx_packets\n");
        fflush(ctx->csv_file);
    }

    LOG_INFO("worker lcore %u: queue %u, csv=%s", lcore_id, queue_idx, ctx->csv_path);
    return 0;
}

/* ── worker_run ─────────────────────────────────────────────────────────── */

int worker_run(void *arg)
{
    struct worker_ctx *ctx = arg;
    const struct fwd_config *cfg = ctx->cfg;
    const uint16_t rx_port  = ctx->rx_port;
    const uint16_t tx_port  = ctx->tx_port;
    const uint16_t rx_queue = ctx->rx_queue_id;
    const uint16_t tx_queue = ctx->tx_queue_id;
    const uint32_t burst    = cfg->burst_size;

    struct burst_ctx bc;

    LOG_INFO("worker lcore %u started", ctx->lcore_id);
    rte_rcu_qsbr_thread_online(ctx->qsv, ctx->lcore_id);

    uint64_t idle_count = 0;

    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(rx_port, rx_queue, bc.rx_pkts, burst);

        if (nb_rx == 0) {
            ctx->perf.idle_polls++;
            rte_rcu_qsbr_quiescent(ctx->qsv, ctx->lcore_id);
            if (unlikely(++idle_count >= IDLE_EXPORT_BATCH)) {
                idle_count = 0;
                uint64_t t = rte_rdtsc();
                if (t - ctx->last_export_tsc > ctx->export_tsc_interval) {
                    ctx->last_export_tsc = t;
                    ctx->table_full_warned = false;
                }
            }
            continue;
        }

        ctx->perf.active_polls++;
        idle_count = 0;

        uint64_t now_tsc = rte_rdtsc();
        worker_check_perf_log(ctx, now_tsc);

        uint64_t proc_start = now_tsc;

        uint16_t nb_ip = burst_parse(bc.rx_pkts, nb_rx, bc.keys, bc.key_ptrs, bc.ip_pkts);

        if (nb_ip > 0)
            rte_hash_lookup_bulk(ctx->ftable.ht, bc.key_ptrs, nb_ip, bc.positions);

        uint16_t nb_tx = burst_flow_update(ctx, bc.rx_pkts, bc.tx_pkts, bc.tx_flows,
                                           bc.keys, bc.key_ptrs, bc.ip_pkts, bc.positions,
                                           nb_ip, now_tsc);

        ctx->perf.proc_cycles += rte_rdtsc() - proc_start;
        ctx->perf.rx_packets  += nb_rx;

        if (nb_tx == 0)
            continue;

        uint16_t nb_sent = rte_eth_tx_burst(tx_port, tx_queue, bc.tx_pkts, nb_tx);
        burst_tx_stats(ctx, bc.tx_pkts, bc.tx_flows, nb_sent, nb_tx);
        ctx->perf.tx_packets += nb_sent;

        rte_rcu_qsbr_quiescent(ctx->qsv, ctx->lcore_id);
    }

    rte_rcu_qsbr_thread_offline(ctx->qsv, ctx->lcore_id);
    fclose(ctx->csv_file);
    rte_free(ctx->expired_keys);
    flow_table_free(&ctx->ftable);
    LOG_INFO("worker lcore %u exiting", ctx->lcore_id);
    return 0;
}
