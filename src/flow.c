#include <string.h>
#include <stdlib.h>

#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

#include "flow.h"
#include "log.h"

int flow_table_init(struct flow_table *ft, uint32_t capacity,
                    unsigned core_id, int socket_id)
{
    char name[64];
    snprintf(name, sizeof(name), "flow_core_%u", core_id);

    struct rte_hash_parameters hp = {
        .name               = name,
        .entries            = capacity,
        .key_len            = sizeof(struct flow_key),
        .hash_func          = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id          = socket_id,
        .extra_flag         = RTE_HASH_EXTRA_FLAGS_NO_FREE_ON_DEL |
                              RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY,
    };

    ft->ht = rte_hash_create(&hp);
    if (!ft->ht) {
        LOG_ERR("rte_hash_create failed for core %u", core_id);
        return -1;
    }

    ft->entries = rte_zmalloc_socket(name,
                                     capacity * sizeof(struct flow_entry),
                                     RTE_CACHE_LINE_SIZE,
                                     socket_id);
    if (!ft->entries) {
        rte_hash_free(ft->ht);
        ft->ht = NULL;
        LOG_ERR("rte_zmalloc_socket failed for flow entries (core %u)", core_id);
        return -1;
    }

    ft->capacity = capacity;
    ft->count    = 0;
    ft->core_id  = core_id;
    return 0;
}

void flow_table_free(struct flow_table *ft)
{
    if (ft->ht) {
        rte_hash_free(ft->ht);
        ft->ht = NULL;
    }
    if (ft->entries) {
        rte_free(ft->entries);
        ft->entries = NULL;
    }
}

struct flow_entry *flow_lookup_or_create(struct flow_table *ft,
                                         const struct flow_key *key,
                                         uint64_t now_tsc)
{
    int32_t pos = rte_hash_lookup(ft->ht, key);
    if (likely(pos >= 0)) {
        ft->entries[pos].last_seen_tsc = now_tsc;
        return &ft->entries[pos];
    }

    if (ft->count >= ft->capacity)
        return NULL;

    pos = rte_hash_add_key(ft->ht, key);
    if (pos < 0)
        return NULL;

    struct flow_entry *e = &ft->entries[pos];
    memset(e, 0, sizeof(*e));
    e->key           = *key;
    e->created_tsc   = now_tsc;
    e->last_seen_tsc = now_tsc;
    ft->count++;
    return e;
}

void flow_expire(struct flow_table *ft, uint64_t now_tsc, uint64_t timeout_tsc)
{
    if (ft->count == 0)
        return;

    struct flow_key *expired = malloc(ft->count * sizeof(*expired));
    if (!expired)
        return;

    uint32_t nb_expired = 0;
    uint32_t iter = 0;
    const void *key;
    void *data;
    int32_t pos;

    while ((pos = rte_hash_iterate(ft->ht, &key, &data, &iter)) >= 0) {
        if (now_tsc - ft->entries[pos].last_seen_tsc > timeout_tsc)
            expired[nb_expired++] = ft->entries[pos].key;
    }

    for (uint32_t i = 0; i < nb_expired; i++)
        flow_delete(ft, &expired[i]);

    free(expired);
}
