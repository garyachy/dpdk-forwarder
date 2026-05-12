#include <string.h>
#include <stdlib.h>

#include "flow.h"
#include "log.h"

/* ── DPDK build ──────────────────────────────────────────────────────────── */
#ifndef UNIT_TEST

#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

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
        .extra_flag         = RTE_HASH_EXTRA_FLAGS_NO_FREE_ON_DEL,
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

    /* Miss — create new entry */
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

/* ── Unit-test shim ──────────────────────────────────────────────────────── */
#else /* UNIT_TEST */

#include <stdint.h>
#include <time.h>

/* Minimal open-addressing hash table (linear probing) for unit tests.
 * Not thread-safe; single-threaded test use only. */

typedef struct {
    uint32_t           capacity;
    uint32_t           count;
    struct flow_entry *slots;
    uint8_t           *occupied;
} ut_ht_t;

static uint32_t ut_hash(const struct flow_key *k)
{
    const uint32_t *p = (const uint32_t *)k;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(*k) / 4; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

int flow_table_init(struct flow_table *ft, uint32_t capacity,
                    unsigned core_id, int socket_id)
{
    (void)socket_id;
    ut_ht_t *ht = calloc(1, sizeof(ut_ht_t));
    ht->capacity = capacity;
    ht->count    = 0;
    ht->slots    = calloc(capacity, sizeof(struct flow_entry));
    ht->occupied = calloc(capacity, 1);

    ft->ht       = ht;
    ft->entries  = ht->slots;
    ft->capacity = capacity;
    ft->count    = 0;
    ft->core_id  = core_id;
    return 0;
}

void flow_table_free(struct flow_table *ft)
{
    ut_ht_t *ht = ft->ht;
    if (ht) {
        free(ht->slots);
        free(ht->occupied);
        free(ht);
        ft->ht = NULL;
    }
}

struct flow_entry *flow_lookup_or_create(struct flow_table *ft,
                                         const struct flow_key *key,
                                         uint64_t now_tsc)
{
    ut_ht_t *ht = ft->ht;
    uint32_t start = ut_hash(key) % ht->capacity;
    uint32_t i = start;

    do {
        if (ht->occupied[i] &&
            memcmp(&ht->slots[i].key, key, sizeof(*key)) == 0) {
            ht->slots[i].last_seen_tsc = now_tsc;
            return &ht->slots[i];
        }
        i = (i + 1) % ht->capacity;
    } while (i != start);

    /* Miss — find empty slot */
    if (ft->count >= ft->capacity)
        return NULL;

    i = start;
    do {
        if (!ht->occupied[i]) {
            ht->occupied[i] = 1;
            memset(&ht->slots[i], 0, sizeof(ht->slots[i]));
            ht->slots[i].key           = *key;
            ht->slots[i].created_tsc   = now_tsc;
            ht->slots[i].last_seen_tsc = now_tsc;
            ft->count++;
            return &ht->slots[i];
        }
        i = (i + 1) % ht->capacity;
    } while (i != start);

    return NULL;
}

void flow_expire(struct flow_table *ft, uint64_t now_tsc, uint64_t timeout_tsc)
{
    ut_ht_t *ht = ft->ht;
    for (uint32_t i = 0; i < ht->capacity; i++) {
        if (ht->occupied[i]) {
            struct flow_entry *e = &ht->slots[i];
            if (now_tsc - e->last_seen_tsc > timeout_tsc) {
                ht->occupied[i] = 0;
                memset(e, 0, sizeof(*e));
                ft->count--;
            }
        }
    }
}

#endif /* UNIT_TEST */
