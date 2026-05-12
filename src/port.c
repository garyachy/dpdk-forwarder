#include <rte_ethdev.h>
#include <string.h>

#include "port.h"
#include "log.h"

/* Symmetric Toeplitz RSS key (52 bytes).
 * RSS(A→B) == RSS(B→A): forward and return traffic land on the same queue. */
static const uint8_t rss_key[52] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A,
};

static const uint16_t RX_DESC = 1024;
static const uint16_t TX_DESC = 1024;

int port_init(uint16_t port_id, uint16_t nb_queues,
              struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_RSS,
        },
        .rx_adv_conf.rss_conf = {
            .rss_key     = (uint8_t *)rss_key,
            .rss_key_len = sizeof(rss_key),
            .rss_hf      = RTE_ETH_RSS_IPV4
                         | RTE_ETH_RSS_NONFRAG_IPV4_TCP
                         | RTE_ETH_RSS_NONFRAG_IPV4_UDP
                         | RTE_ETH_RSS_NONFRAG_IPV4_OTHER,
        },
        .txmode = {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
        },
    };

    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        LOG_ERR("port %u: rte_eth_dev_info_get failed (%d)", port_id, ret);
        return ret;
    }

    /* Disable RSS entirely if the PMD doesn't support it */
    if (dev_info.flow_type_rss_offloads == 0 || dev_info.hash_key_size == 0) {
        LOG_WARN("port %u: no RSS support, using single queue", port_id);
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
        port_conf.rx_adv_conf.rss_conf.rss_key     = NULL;
        port_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
        port_conf.rx_adv_conf.rss_conf.rss_hf      = 0;
    } else {
        /* Mask RSS hash types to what the device supports */
        port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
        /* Truncate RSS key to device's preferred length (virtio=40B, NICs=52B) */
        if (dev_info.hash_key_size < port_conf.rx_adv_conf.rss_conf.rss_key_len)
            port_conf.rx_adv_conf.rss_conf.rss_key_len = dev_info.hash_key_size;
    }

    /* Enable fast-free TX offload if supported */
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    /* Cap queues to what the device supports */
    if (nb_queues > dev_info.max_rx_queues)
        nb_queues = dev_info.max_rx_queues;
    if (nb_queues > dev_info.max_tx_queues)
        nb_queues = dev_info.max_tx_queues;

    ret = rte_eth_dev_configure(port_id, nb_queues, nb_queues, &port_conf);
    if (ret != 0) {
        LOG_ERR("port %u: rte_eth_dev_configure failed (%d)", port_id, ret);
        return ret;
    }

    uint16_t rx_desc = RX_DESC;
    uint16_t tx_desc = TX_DESC;
    rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_desc, &tx_desc);

    int socket = rte_eth_dev_socket_id(port_id);

    for (uint16_t q = 0; q < nb_queues; q++) {
        ret = rte_eth_rx_queue_setup(port_id, q, rx_desc, socket, NULL, mbuf_pool);
        if (ret != 0) {
            LOG_ERR("port %u: rx queue %u setup failed (%d)", port_id, q, ret);
            return ret;
        }
        ret = rte_eth_tx_queue_setup(port_id, q, tx_desc, socket, NULL);
        if (ret != 0) {
            LOG_ERR("port %u: tx queue %u setup failed (%d)", port_id, q, ret);
            return ret;
        }
    }

    ret = rte_eth_dev_start(port_id);
    if (ret != 0) {
        LOG_ERR("port %u: rte_eth_dev_start failed (%d)", port_id, ret);
        return ret;
    }

    /* Configure RETA: map 128 buckets round-robin across queues */
    uint16_t reta_size = dev_info.reta_size;
    if (reta_size == 0)
        reta_size = 128;

    uint16_t nb_groups = (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1)
                         / RTE_ETH_RETA_GROUP_SIZE;
    struct rte_eth_rss_reta_entry64 reta[nb_groups];
    memset(reta, 0, sizeof(reta));

    for (uint16_t i = 0; i < reta_size; i++) {
        uint16_t g = i / RTE_ETH_RETA_GROUP_SIZE;
        uint16_t b = i % RTE_ETH_RETA_GROUP_SIZE;
        reta[g].mask |= (1ULL << b);
        reta[g].reta[b] = i % nb_queues;
    }
    /* Non-fatal: some virtual PMDs don't support RETA update */
    ret = rte_eth_dev_rss_reta_update(port_id, reta, reta_size);
    if (ret != 0 && ret != -ENOTSUP)
        LOG_WARN("port %u: RETA update failed (%d), RSS may not be even", port_id, ret);

    rte_eth_promiscuous_enable(port_id);

    struct rte_ether_addr addr;
    rte_eth_macaddr_get(port_id, &addr);
    LOG_INFO("port %u up: MAC=" RTE_ETHER_ADDR_PRT_FMT " queues=%u",
             port_id, RTE_ETHER_ADDR_BYTES(&addr), nb_queues);

    return nb_queues;
}

void port_stats_print(uint16_t port_id)
{
    struct rte_eth_stats stats;
    if (rte_eth_stats_get(port_id, &stats) != 0)
        return;
    LOG_INFO("port %u: rx=%" PRIu64 " pkts / %" PRIu64 " bytes"
             "  tx=%" PRIu64 " pkts / %" PRIu64 " bytes"
             "  dropped=%" PRIu64,
             port_id,
             stats.ipackets, stats.ibytes,
             stats.opackets, stats.obytes,
             stats.imissed + stats.ierrors);
}
