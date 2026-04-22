#include "snapshot_types.h"
#include <linux/slab.h>
#include <linux/mm.h>

#define POOL_BUCKET_COUNT (1ULL << 20)  /* 1M buckets for hash lookup */

static uint64_t hash_to_bucket(uint64_t hash_idx)
{
    return hash_idx % POOL_BUCKET_COUNT;
}

int snapshot_pool_init(struct global_page_pool *pool)
{
    pool->buckets = kvmalloc_array(POOL_BUCKET_COUNT,
                                    sizeof(struct hlist_head),
                                    GFP_KERNEL | __GFP_ZERO);
    if (!pool->buckets)
        return -ENOMEM;

    spin_lock_init(&pool->lock);
    pool->total_pages = 0;
    pool->bucket_count = POOL_BUCKET_COUNT;

    return 0;
}

void snapshot_pool_destroy(struct global_page_pool *pool)
{
    struct phys_page_entry *entry;
    struct hlist_node *tmp;
    uint64_t i;

    spin_lock(&pool->lock);
    for (i = 0; i < pool->bucket_count; i++) {
        hlist_for_each_entry_safe(entry, tmp, &pool->buckets[i], node) {
            hlist_del(&entry->node);
            if (atomic_read(&entry->ref_count) == 0) {
                __free_page(entry->page);
                kfree(entry);
            }
        }
    }
    kvfree(pool->buckets);
    spin_unlock(&pool->lock);
}

struct phys_page_entry *snapshot_pool_lookup_noref(struct global_page_pool *pool,
                                                     uint64_t hash_idx)
{
    struct phys_page_entry *entry;
    uint64_t bucket = hash_to_bucket(hash_idx);

    spin_lock(&pool->lock);
    hlist_for_each_entry(entry, &pool->buckets[bucket], node) {
        if (entry->hash_idx == hash_idx) {
            spin_unlock(&pool->lock);
            return entry;  /* Return without incrementing ref_count */
        }
    }
    spin_unlock(&pool->lock);

    return NULL;
}

struct phys_page_entry *snapshot_pool_lookup(struct global_page_pool *pool,
                                               uint64_t hash_idx)
{
    struct phys_page_entry *entry;
    uint64_t bucket = hash_to_bucket(hash_idx);

    spin_lock(&pool->lock);
    hlist_for_each_entry(entry, &pool->buckets[bucket], node) {
        if (entry->hash_idx == hash_idx) {
            atomic_inc(&entry->ref_count);
            spin_unlock(&pool->lock);
            return entry;
        }
    }
    spin_unlock(&pool->lock);

    return NULL;
}

struct phys_page_entry *snapshot_pool_add(struct global_page_pool *pool,
                                           uint64_t hash_idx,
                                           void *data)
{
    struct phys_page_entry *entry, *existing;
    struct page *page;
    uint64_t bucket;

    bucket = hash_to_bucket(hash_idx);

    /* Check if already exists under lock */
    spin_lock(&pool->lock);
    hlist_for_each_entry(existing, &pool->buckets[bucket], node) {
        if (existing->hash_idx == hash_idx) {
            atomic_inc(&existing->ref_count);
            spin_unlock(&pool->lock);
            return existing;
        }
    }
    spin_unlock(&pool->lock);

    /* Not found - create new entry (preload context allows blocking) */
    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return NULL;

    page = alloc_page(GFP_KERNEL);
    if (!page) {
        kfree(entry);
        return NULL;
    }

    /* Copy data to page */
    memcpy(page_address(page), data, PAGE_SIZE);

    entry->hash_idx = hash_idx;
    entry->page = page;
    atomic_set(&entry->ref_count, 1);

    /* Add under lock, re-check for race */
    spin_lock(&pool->lock);
    hlist_for_each_entry(existing, &pool->buckets[bucket], node) {
        if (existing->hash_idx == hash_idx) {
            /* Race: another thread added it first */
            atomic_inc(&existing->ref_count);
            spin_unlock(&pool->lock);
            __free_page(page);
            kfree(entry);
            return existing;
        }
    }
    hlist_add_head(&entry->node, &pool->buckets[bucket]);
    pool->total_pages++;
    spin_unlock(&pool->lock);

    return entry;
}

void snapshot_pool_ref(struct phys_page_entry *entry)
{
    atomic_inc(&entry->ref_count);
}

void snapshot_pool_unref(struct global_page_pool *pool,
                          struct phys_page_entry *entry)
{
    spin_lock(&pool->lock);
    if (atomic_dec_and_test(&entry->ref_count)) {
        hlist_del(&entry->node);
        pool->total_pages--;
        spin_unlock(&pool->lock);

        __free_page(entry->page);
        kfree(entry);
    } else {
        spin_unlock(&pool->lock);
    }
}