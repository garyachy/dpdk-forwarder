#pragma once

#include <stdint.h>
#include <rte_mempool.h>

/* Returns the actual number of queues configured (may be < nb_queues if
 * the device doesn't support that many), or -1 on error. */
int  port_init(uint16_t port_id, uint16_t nb_queues,
               struct rte_mempool *mbuf_pool);
void port_stats_print(uint16_t port_id);
