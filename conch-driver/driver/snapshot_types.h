#ifndef _SNAPSHOT_TYPES_H
#define _SNAPSHOT_TYPES_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/rbtree.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>

/* Configuration constants */
#define HASH_MODULUS_DEFAULT    (1ULL << 20)   /* N = 2^20 (1M slots) */
#define MAX_PROBE_COUNT_DEFAULT 64
#define PAGE_TABLE_ENTRY_SIZE   16              /* 8B va + 8B hash_idx */
#define SLOT_SIZE               (PAGE_SIZE + 8) /* 4KB + 8B header */
#define TEMPLATE_ID_MAX_LEN     64
#define PATH_MAX_LEN            256

/* ioctl structures */
struct ioctl_create_template {
    char template_id[TEMPLATE_ID_MAX_LEN];
    uint64_t total_size;
    char page_table_path[PATH_MAX_LEN];
    char pages_path[PATH_MAX_LEN];
};

struct ioctl_delete_template {
    char template_id[TEMPLATE_ID_MAX_LEN];
};

struct ioctl_template_status {
    char template_id[TEMPLATE_ID_MAX_LEN];
    uint64_t mmap_count;
    uint64_t loaded_pages;
};

#define IOCTL_CREATE_TEMPLATE   _IOW('S', 1, struct ioctl_create_template)
#define IOCTL_DELETE_TEMPLATE   _IOW('S', 2, struct ioctl_delete_template)
#define IOCTL_TEMPLATE_STATUS   _IOWR('S', 3, struct ioctl_template_status)

/* Page table entry (file format) */
struct page_table_entry {
    uint64_t va_offset;
    uint64_t hash_idx;
};

/* Global physical page pool */
struct global_page_pool {
    struct hlist_head *buckets;
    spinlock_t lock;
    uint64_t total_pages;
    uint64_t bucket_count;
};

/* Physical page entry */
struct phys_page_entry {
    uint64_t hash_idx;
    struct page *page;
    atomic_t ref_count;
    struct hlist_node node;
};

/* VA to PA mapping entry (for VMA) */
struct va_to_pa_entry {
    uint64_t va_offset;
    struct phys_page_entry *phys_entry;
    struct rb_node rb_node;
};

/* VMA snapshot data */
struct vma_snapshot_data {
    struct snapshot_template *template;
    struct rb_root va_to_pa_map;
    bool is_first_vma;
};

/* Snapshot template */
struct snapshot_template {
    char template_id[TEMPLATE_ID_MAX_LEN];
    uint64_t total_size;

    char page_table_path[PATH_MAX_LEN];
    char pages_path[PATH_MAX_LEN];

    struct vma_snapshot_data *first_vma_data;

    struct miscdevice mdev;
    atomic_t ref_count;
    struct list_head list;
};

/* Driver global state */
struct snapshot_driver_state {
    struct list_head template_list;
    spinlock_t template_lock;

    struct global_page_pool page_pool;

    uint64_t hash_modulus;
    uint32_t max_probe_count;
};

/* Global state accessor */
extern struct snapshot_driver_state *g_driver_state;

/* File operations (defined in snapshot_mmap.c) */
extern const struct file_operations snapshot_fops;

/* Template functions (defined in snapshot_template.c) */
extern struct snapshot_template *snapshot_template_find(const char *id);

/* Pool functions (defined in snapshot_pool.c) */
extern struct phys_page_entry *snapshot_pool_lookup(struct global_page_pool *pool, uint64_t hash_idx);
extern struct phys_page_entry *snapshot_pool_add(struct global_page_pool *pool, uint64_t hash_idx, void *data);
extern void snapshot_pool_ref(struct phys_page_entry *entry);
extern void snapshot_pool_unref(struct global_page_pool *pool, struct phys_page_entry *entry);

/* Page pool initialization */
int snapshot_pool_init(struct global_page_pool *pool);
void snapshot_pool_destroy(struct global_page_pool *pool);

#endif /* _SNAPSHOT_TYPES_H */