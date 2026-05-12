#pragma once

#include <stdint.h>
#include <rte_mempool.h>

int  port_init(uint16_t port_id, uint16_t nb_queues,
               struct rte_mempool *mbuf_pool);
void port_stats_print(uint16_t port_id);
