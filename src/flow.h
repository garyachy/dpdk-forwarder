#pragma once

#include <stdint.h>
#include <string.h>

#include <rte_hash.h>
#include <rte_memory.h>

struct flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  _pad[3];
} __rte_packed;

struct flow_entry {
    struct flow_key key;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t last_seen_tsc;
    uint64_t created_tsc;
} __rte_cache_aligned;

struct flow_table {
    struct rte_hash   *ht;
    struct flow_entry *entries;
    uint32_t           capacity;
    uint32_t           count;
    unsigned           core_id;
};

int  flow_table_init(struct flow_table *ft, uint32_t capacity,
                     unsigned core_id, int socket_id);
void flow_table_free(struct flow_table *ft);

struct flow_entry *flow_lookup_or_create(struct flow_table *ft,
                                         const struct flow_key *key,
                                         uint64_t now_tsc);

/* Delete one flow: remove from hash, zero the entry, decrement count. */
static inline void flow_delete(struct flow_table *ft, const struct flow_key *key)
{
    int32_t p = rte_hash_del_key(ft->ht, key);
    if (p >= 0) {
        memset(&ft->entries[p], 0, sizeof(ft->entries[p]));
        ft->count--;
    }
}

/* Scan for stale flows and delete them. Uses malloc; not for the hot path. */
void flow_expire(struct flow_table *ft, uint64_t now_tsc, uint64_t timeout_tsc);
