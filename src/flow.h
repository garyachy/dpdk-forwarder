#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef UNIT_TEST
#include <rte_hash.h>
#include <rte_memory.h>
#define FLOW_CACHE_ALIGNED __rte_cache_aligned
#define FLOW_PACKED        __rte_packed
#else
#define FLOW_CACHE_ALIGNED
#define FLOW_PACKED
#endif

struct flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  _pad[3];
} FLOW_PACKED;

struct flow_entry {
    struct flow_key key;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t last_seen_tsc;
    uint64_t created_tsc;
} FLOW_CACHE_ALIGNED;

struct flow_table {
#ifndef UNIT_TEST
    struct rte_hash   *ht;
#else
    void              *ht;   /* opaque in unit-test build */
#endif
    struct flow_entry *entries;
    uint32_t           capacity;
    uint32_t           count;
    unsigned           core_id;
};

int  flow_table_init(struct flow_table *ft, uint32_t capacity,
                     unsigned core_id, int socket_id);
void flow_table_free(struct flow_table *ft);

/* Returns the entry for the key (creating if absent), or NULL if table full. */
struct flow_entry *flow_lookup_or_create(struct flow_table *ft,
                                         const struct flow_key *key,
                                         uint64_t now_tsc);

#ifndef UNIT_TEST
#include <string.h>
/* Delete one flow: remove from hash, zero the entry, decrement count. */
static inline void flow_delete(struct flow_table *ft, const struct flow_key *key)
{
    int32_t p = rte_hash_del_key(ft->ht, key);
    if (p >= 0) {
        memset(&ft->entries[p], 0, sizeof(ft->entries[p]));
        ft->count--;
    }
}
#else
/* Test-only helper: expire flows inline (production expiry is in stats_export_and_expire). */
void flow_expire(struct flow_table *ft, uint64_t now_tsc, uint64_t timeout_tsc);
#endif
