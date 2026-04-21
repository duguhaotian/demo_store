# Memory Snapshot Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a kernel driver + user-space tool that enables memory sharing across multiple Cloud Hypervisor VMs from the same snapshot template, without modifying the kernel.

**Architecture:** Driver creates per-template device files (`/dev/snapshot_{id}`). VMs mmap these devices for Guest memory. Driver's fault handler loads pages on-demand, shares RO physical pages across VMs, and relies on kernel COW for writes. Split tool converts full snapshots into page_table.bin + pages.bin format.

**Tech Stack:** Linux kernel module (C), user-space tool (C), Linux kernel APIs (miscdevice, vm_ops, file I/O)

---

## File Structure

```
driver/
  ├── snapshot_types.h      # Data structure definitions
  ├── snapshot_main.c       # Module init/exit, global state
  ├── snapshot_ioctl.c      # ioctl handlers (create/delete/status)
  ├── snapshot_mmap.c       # mmap handler, vm_ops, fault handler
  ├── snapshot_pool.c       # Global page pool management
  ├── snapshot_template.c   # Template lifecycle management
  ├── Makefile              # Kernel module build
  └── Kbuild                # Kernel build config

tools/
  ├── split_snapshot.c      # Snapshot split tool
  ├── Makefile              # Tool build

tests/
  ├── test_ioctl.c          # ioctl test program
  ├── test_mmap.c           # mmap test program
  ├── test_split.c          # Split tool test
  ├── generate_test_snapshot.sh  # Generate test data
  ├── Makefile              # Test programs build
```

---

## Component 1: Driver Data Structures and Module Init

### Task 1: Driver Types Header

**Files:**
- Create: `driver/snapshot_types.h`

- [ ] **Step 1: Write data structure definitions**

```c
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
#define HASH_MODULUS_DEFAULT    (1ULL << 30)   /* N = 2^30 */
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

#endif /* _SNAPSHOT_TYPES_H */
```

- [ ] **Step 2: Commit**

```bash
git add driver/snapshot_types.h
git commit -m "feat(driver): add data structure definitions"
```

### Task 2: Global Page Pool Implementation

**Files:**
- Create: `driver/snapshot_pool.c`

- [ ] **Step 1: Write global page pool implementation**

```c
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
            if (atomic_read(&entry->ref_count) == 0) {
                __free_page(entry->page);
                kfree(entry);
            }
        }
    }
    kvfree(pool->buckets);
    spin_unlock(&pool->lock);
}

struct phys_page_entry *snapshot_pool_lookup(struct global_page_pool *pool,
                                               uint64_t hash_idx)
{
    struct phys_page_entry *entry;
    uint64_t bucket = hash_to_bucket(hash_idx);
    
    spin_lock(&pool->lock);
    hlist_for_each_entry(entry, &pool->buckets[bucket], node) {
        if (entry->hash_idx == hash_idx) {
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
    struct phys_page_entry *entry;
    struct page *page;
    uint64_t bucket;
    
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
    
    bucket = hash_to_bucket(hash_idx);
    
    spin_lock(&pool->lock);
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
    if (atomic_dec_and_test(&entry->ref_count)) {
        spin_lock(&pool->lock);
        hlist_del(&entry->node);
        pool->total_pages--;
        spin_unlock(&pool->lock);
        
        __free_page(entry->page);
        kfree(entry);
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add driver/snapshot_pool.c
git commit -m "feat(driver): add global page pool implementation"
```

### Task 3: Module Init/Exit and Global State

**Files:**
- Create: `driver/snapshot_main.c`
- Create: `driver/Kbuild`

- [ ] **Step 1: Write module init/exit**

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include "snapshot_types.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Snapshot Driver Team");
MODULE_DESCRIPTION("Memory snapshot driver for VM sharing");
MODULE_VERSION("1.0");

struct snapshot_driver_state *g_driver_state;

static int __init snapshot_init(void)
{
    int ret;
    
    g_driver_state = kzalloc(sizeof(*g_driver_state), GFP_KERNEL);
    if (!g_driver_state)
        return -ENOMEM;
    
    INIT_LIST_HEAD(&g_driver_state->template_list);
    spin_lock_init(&g_driver_state->template_lock);
    
    g_driver_state->hash_modulus = HASH_MODULUS_DEFAULT;
    g_driver_state->max_probe_count = MAX_PROBE_COUNT_DEFAULT;
    
    ret = snapshot_pool_init(&g_driver_state->page_pool);
    if (ret) {
        kfree(g_driver_state);
        return ret;
    }
    
    pr_info("snapshot_driver: initialized\n");
    return 0;
}

static void __exit snapshot_exit(void)
{
    struct snapshot_template *template, *tmp;
    
    spin_lock(&g_driver_state->template_lock);
    list_for_each_entry_safe(template, tmp, 
                             &g_driver_state->template_list, list) {
        list_del(&template->list);
        misc_device_unregister(&template->mdev);
        kfree(template);
    }
    spin_unlock(&g_driver_state->template_lock);
    
    snapshot_pool_destroy(&g_driver_state->page_pool);
    kfree(g_driver_state);
    
    pr_info("snapshot_driver: unloaded\n");
}

module_init(snapshot_init);
module_exit(snapshot_exit);
```

- [ ] **Step 2: Create Kbuild file**

```make
obj-m := snapshot_driver.o
snapshot_driver-objs := snapshot_main.o snapshot_pool.o snapshot_ioctl.o \
                        snapshot_mmap.o snapshot_template.o
```

- [ ] **Step 3: Create Makefile for kernel module build**

```make
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install:
	make -C $(KDIR) M=$(PWD) modules_install
```

- [ ] **Step 4: Commit**

```bash
git add driver/snapshot_main.c driver/Kbuild driver/Makefile
git commit -m "feat(driver): add module init/exit"
```

---

## Component 2: Template Management

### Task 4: Template Lifecycle Implementation

**Files:**
- Create: `driver/snapshot_template.c`

- [ ] **Step 1: Write template management implementation**

```c
#include "snapshot_types.h"
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

static int template_id_exists(const char *id)
{
    struct snapshot_template *template;
    
    spin_lock(&g_driver_state->template_lock);
    list_for_each_entry(template, &g_driver_state->template_list, list) {
        if (strncmp(template->template_id, id, TEMPLATE_ID_MAX_LEN) == 0) {
            spin_unlock(&g_driver_state->template_lock);
            return 1;
        }
    }
    spin_unlock(&g_driver_state->template_lock);
    
    return 0;
}

struct snapshot_template *snapshot_template_find(const char *id)
{
    struct snapshot_template *template;
    
    spin_lock(&g_driver_state->template_lock);
    list_for_each_entry(template, &g_driver_state->template_list, list) {
        if (strncmp(template->template_id, id, TEMPLATE_ID_MAX_LEN) == 0) {
            spin_unlock(&g_driver_state->template_lock);
            return template;
        }
    }
    spin_unlock(&g_driver_state->template_lock);
    
    return NULL;
}

int snapshot_template_create(struct ioctl_create_template *args)
{
    struct snapshot_template *template;
    struct file *pt_file, *pages_file;
    int ret;
    
    /* Validate paths */
    pt_file = filp_open(args->page_table_path, O_RDONLY, 0);
    if (IS_ERR(pt_file))
        return -EINVAL;
    filp_close(pt_file, NULL);
    
    pages_file = filp_open(args->pages_path, O_RDONLY, 0);
    if (IS_ERR(pages_file))
        return -EINVAL;
    filp_close(pages_file, NULL);
    
    /* Check if template ID already exists */
    if (template_id_exists(args->template_id))
        return -EEXIST;
    
    template = kzalloc(sizeof(*template), GFP_KERNEL);
    if (!template)
        return -ENOMEM;
    
    strncpy(template->template_id, args->template_id, TEMPLATE_ID_MAX_LEN);
    template->total_size = args->total_size;
    strncpy(template->page_table_path, args->page_table_path, PATH_MAX_LEN);
    strncpy(template->pages_path, args->pages_path, PATH_MAX_LEN);
    
    atomic_set(&template->ref_count, 0);
    template->first_vma_data = NULL;
    
    /* Setup miscdevice */
    template->mdev.minor = MISC_DYNAMIC_MINOR;
    template->mdev.name = kasprintf(GFP_KERNEL, "snapshot_%s", args->template_id);
    if (!template->mdev.name) {
        kfree(template);
        return -ENOMEM;
    }
    template->mdev.fops = &snapshot_fops;  /* Defined in snapshot_mmap.c */
    template->mdev.mode = 0600;
    
    ret = misc_device_register(&template->mdev);
    if (ret) {
        kfree(template->mdev.name);
        kfree(template);
        return ret;
    }
    
    spin_lock(&g_driver_state->template_lock);
    list_add_tail(&template->list, &g_driver_state->template_list);
    spin_unlock(&g_driver_state->template_lock);
    
    pr_info("snapshot_driver: created template %s at /dev/%s\n",
            args->template_id, template->mdev.name);
    
    return 0;
}

int snapshot_template_delete(struct ioctl_delete_template *args)
{
    struct snapshot_template *template;
    
    template = snapshot_template_find(args->template_id);
    if (!template)
        return -ENOENT;
    
    /* Check ref_count */
    if (atomic_read(&template->ref_count) > 0)
        return -EBUSY;
    
    spin_lock(&g_driver_state->template_lock);
    list_del(&template->list);
    spin_unlock(&g_driver_state->template_lock);
    
    misc_device_unregister(&template->mdev);
    kfree(template->mdev.name);
    kfree(template);
    
    pr_info("snapshot_driver: deleted template %s\n", args->template_id);
    
    return 0;
}

int snapshot_template_status(struct ioctl_template_status *args)
{
    struct snapshot_template *template;
    
    template = snapshot_template_find(args->template_id);
    if (!template)
        return -ENOENT;
    
    args->mmap_count = atomic_read(&template->ref_count);
    args->loaded_pages = g_driver_state->page_pool.total_pages;
    
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add driver/snapshot_template.c
git commit -m "feat(driver): add template lifecycle management"
```

---

## Component 3: ioctl Handlers

### Task 5: ioctl Implementation

**Files:**
- Create: `driver/snapshot_ioctl.c`

- [ ] **Step 1: Write ioctl handlers**

```c
#include "snapshot_types.h"
#include <linux/uaccess.h>
#include <linux/fs.h>

extern int snapshot_template_create(struct ioctl_create_template *args);
extern int snapshot_template_delete(struct ioctl_delete_template *args);
extern int snapshot_template_status(struct ioctl_template_status *args);

long snapshot_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    void __user *argp = (void __user *)arg;
    int ret;
    
    switch (cmd) {
    case IOCTL_CREATE_TEMPLATE: {
        struct ioctl_create_template args;
        if (copy_from_user(&args, argp, sizeof(args)))
            return -EFAULT;
        ret = snapshot_template_create(&args);
        return ret;
    }
    
    case IOCTL_DELETE_TEMPLATE: {
        struct ioctl_delete_template args;
        if (copy_from_user(&args, argp, sizeof(args)))
            return -EFAULT;
        ret = snapshot_template_delete(&args);
        return ret;
    }
    
    case IOCTL_TEMPLATE_STATUS: {
        struct ioctl_template_status args;
        if (copy_from_user(&args, argp, sizeof(args)))
            return -EFAULT;
        ret = snapshot_template_status(&args);
        if (copy_to_user(argp, &args, sizeof(args)))
            return -EFAULT;
        return ret;
    }
    
    default:
        return -ENOTTY;
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add driver/snapshot_ioctl.c
git commit -m "feat(driver): add ioctl handlers"
```

---

## Component 4: mmap and Fault Handler

### Task 6: VMA Operations and Fault Handler

**Files:**
- Create: `driver/snapshot_mmap.c`

- [ ] **Step 1: Write mmap and fault handler implementation**

```c
#include "snapshot_types.h"
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/slab.h>

/* Forward declarations */
extern long snapshot_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* File operations structure (will be completed in this file) */
static int snapshot_open(struct inode *inode, struct file *file);
static int snapshot_release(struct inode *inode, struct file *file);
static int snapshot_mmap(struct file *file, struct vm_area_struct *vma);

const struct file_operations snapshot_fops = {
    .owner = THIS_MODULE,
    .open = snapshot_open,
    .release = snapshot_release,
    .mmap = snapshot_mmap,
    .unlocked_ioctl = snapshot_ioctl,
};

/* VA to PA map helpers */
static struct va_to_pa_entry *va_to_pa_lookup(struct rb_root *root, uint64_t va)
{
    struct rb_node *node = root->rb_node;
    
    while (node) {
        struct va_to_pa_entry *entry = rb_entry(node, struct va_to_pa_entry, rb_node);
        
        if (va < entry->va_offset)
            node = node->rb_left;
        else if (va > entry->va_offset)
            node = node->rb_right;
        else
            return entry;
    }
    
    return NULL;
}

static int va_to_pa_insert(struct rb_root *root, struct va_to_pa_entry *new)
{
    struct rb_node **link = &root->rb_node;
    struct rb_node *parent = NULL;
    
    while (*link) {
        struct va_to_pa_entry *entry = rb_entry(*link, struct va_to_pa_entry, rb_node);
        parent = *link;
        
        if (new->va_offset < entry->va_offset)
            link = &(*link)->rb_left;
        else if (new->va_offset > entry->va_offset)
            link = &(*link)->rb_right;
        else
            return -EEXIST;
    }
    
    rb_link_node(&new->rb_node, parent, link);
    rb_insert_color(&new->rb_node, root);
    
    return 0;
}

/* Read page_table entry from file */
static int read_page_table_entry(const char *path, uint64_t va,
                                  struct page_table_entry *pte)
{
    struct file *file;
    loff_t offset;
    ssize_t ret;
    
    offset = (va / PAGE_SIZE) * PAGE_TABLE_ENTRY_SIZE;
    
    file = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(file))
        return PTR_ERR(file);
    
    ret = kernel_read(file, pte, sizeof(*pte), &offset);
    filp_close(file, NULL);
    
    if (ret != sizeof(*pte))
        return -EIO;
    
    return 0;
}

/* Read page from pages.bin using open addressing */
static int read_page_data(const char *path, uint64_t hash_idx,
                           void *data)
{
    struct file *file;
    loff_t offset;
    ssize_t ret;
    uint64_t file_index;
    uint64_t slot_hash;
    uint32_t probe;
    char header[8];
    
    file = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(file))
        return PTR_ERR(file);
    
    /* Open addressing lookup */
    file_index = hash_idx % g_driver_state->hash_modulus;
    
    for (probe = 0; probe < g_driver_state->max_probe_count; probe++) {
        offset = file_index * SLOT_SIZE;
        
        /* Read header */
        ret = kernel_read(file, header, 8, &offset);
        if (ret != 8) {
            filp_close(file, NULL);
            return -EIO;
        }
        
        slot_hash = *((uint64_t *)header);
        
        if (slot_hash == hash_idx) {
            /* Found matching slot */
            offset = file_index * SLOT_SIZE + 8;
            ret = kernel_read(file, data, PAGE_SIZE, &offset);
            filp_close(file, NULL);
            
            if (ret != PAGE_SIZE)
                return -EIO;
            
            return 0;
        }
        
        if (slot_hash == 0) {
            /* Empty slot, page not found */
            filp_close(file, NULL);
            return -ENOENT;
        }
        
        /* Linear probing */
        file_index = (file_index + 1) % g_driver_state->hash_modulus;
    }
    
    filp_close(file, NULL);
    return -ENOENT;  /* Max probe count exceeded */
}

/* Fault handler */
static vm_fault_t snapshot_fault(struct vm_fault *vmf)
{
    struct vma_snapshot_data *vma_data;
    struct snapshot_template *template;
    struct page_table_entry pte;
    struct phys_page_entry *phys_entry;
    struct va_to_pa_entry *va_entry;
    uint64_t va_offset;
    unsigned long pfn;
    void *page_data;
    int ret;
    
    vma_data = vmf->vma->vm_private_data;
    template = vma_data->template;
    va_offset = vmf->address - vmf->vma->vm_start;
    
    /* Check if already mapped */
    va_entry = va_to_pa_lookup(&vma_data->va_to_pa_map, va_offset);
    if (va_entry) {
        /* Already mapped in this VMA */
        return VM_FAULT_NOPAGE;
    }
    
    /* Read page_table entry */
    ret = read_page_table_entry(template->page_table_path, va_offset, &pte);
    if (ret)
        return VM_FAULT_SIGBUS;
    
    /* Lookup in global pool */
    phys_entry = snapshot_pool_lookup(&g_driver_state->page_pool, pte.hash_idx);
    
    if (phys_entry) {
        /* Page already loaded */
        snapshot_pool_ref(phys_entry);
    } else {
        /* Load from file */
        page_data = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (!page_data)
            return VM_FAULT_OOM;
        
        ret = read_page_data(template->pages_path, pte.hash_idx, page_data);
        if (ret) {
            kfree(page_data);
            return VM_FAULT_SIGBUS;
        }
        
        phys_entry = snapshot_pool_add(&g_driver_state->page_pool,
                                        pte.hash_idx, page_data);
        kfree(page_data);
        
        if (!phys_entry)
            return VM_FAULT_OOM;
    }
    
    /* Map page to VMA */
    pfn = page_to_pfn(phys_entry->page);
    ret = vmf_insert_pfn_prot(vmf, pfn, vmf->vma->vm_page_prot);
    if (ret != VM_FAULT_NOPAGE) {
        snapshot_pool_unref(&g_driver_state->page_pool, phys_entry);
        return ret;
    }
    
    /* Record mapping */
    va_entry = kzalloc(sizeof(*va_entry), GFP_KERNEL);
    if (va_entry) {
        va_entry->va_offset = va_offset;
        va_entry->phys_entry = phys_entry;
        va_to_pa_insert(&vma_data->va_to_pa_map, va_entry);
    }
    
    return VM_FAULT_NOPAGE;
}

/* VMA open: increment ref count and copy mappings if applicable */
static void snapshot_vma_open(struct vm_area_struct *vma)
{
    struct vma_snapshot_data *vma_data;
    struct snapshot_template *template;
    struct va_to_pa_entry *entry;
    
    vma_data = vma->vm_private_data;
    template = vma_data->template;
    
    atomic_inc(&template->ref_count);
    
    /* Copy mappings from first VMA if exists */
    if (template->first_vma_data && template->first_vma_data != vma_data) {
        struct rb_node *node;
        struct va_to_pa_entry *new_entry;
        unsigned long pfn;
        
        for (node = rb_first(&template->first_vma_data->va_to_pa_map); 
             node; 
             node = rb_next(node)) {
            entry = rb_entry(node, struct va_to_pa_entry, rb_node);
            
            /* Map to this VMA using remap_pfn_range */
            pfn = page_to_pfn(entry->phys_entry->page);
            ret = remap_pfn_range(vma, 
                                   (vma->vm_start + entry->va_offset),
                                   pfn,
                                   PAGE_SIZE,
                                   vma->vm_page_prot);
            if (ret)
                continue;  /* Skip failed mappings */
            
            snapshot_pool_ref(entry->phys_entry);
            
            /* Record in this VMA's map */
            new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
            if (new_entry) {
                new_entry->va_offset = entry->va_offset;
                new_entry->phys_entry = entry->phys_entry;
                va_to_pa_insert(&vma_data->va_to_pa_map, new_entry);
            }
        }
    } else if (!template->first_vma_data) {
        template->first_vma_data = vma_data;
        vma_data->is_first_vma = true;
    }
}

/* VMA close: decrement ref count and cleanup */
static void snapshot_vma_close(struct vm_area_struct *vma)
{
    struct vma_snapshot_data *vma_data;
    struct snapshot_template *template;
    struct va_to_pa_entry *entry;
    struct rb_node *node;
    
    vma_data = vma->vm_private_data;
    template = vma_data->template;
    
    /* Unref all mapped pages */
    for (node = rb_first(&vma_data->va_to_pa_map); node; node = rb_next(node)) {
        entry = rb_entry(node, struct va_to_pa_entry, rb_node);
        snapshot_pool_unref(&g_driver_state->page_pool, entry->phys_entry);
    }
    
    /* Free VA to PA entries */
    while (!RB_EMPTY_ROOT(&vma_data->va_to_pa_map)) {
        node = rb_first(&vma_data->va_to_pa_map);
        entry = rb_entry(node, struct va_to_pa_entry, rb_node);
        rb_erase(node, &vma_data->va_to_pa_map);
        kfree(entry);
    }
    
    /* Clear first_vma_data if this was it */
    if (template->first_vma_data == vma_data)
        template->first_vma_data = NULL;
    
    kfree(vma_data);
    
    atomic_dec(&template->ref_count);
}

static const struct vm_operations_struct snapshot_vm_ops = {
    .fault = snapshot_fault,
    .open = snapshot_vma_open,
    .close = snapshot_vma_close,
};

/* File open */
static int snapshot_open(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

/* File release */
static int snapshot_release(struct inode *inode, struct file *file)
{
    return 0;
}

/* mmap handler */
static int snapshot_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct snapshot_template *template;
    struct vma_snapshot_data *vma_data;
    
    /* Find template by device name */
    template = snapshot_template_find(file->f_path.dentry->d_iname + 
                                       strlen("snapshot_"));
    if (!template)
        return -EINVAL;
    
    /* Check size */
    if (vma->vm_end - vma->vm_start > template->total_size)
        return -EINVAL;
    
    /* Setup VMA data */
    vma_data = kzalloc(sizeof(*vma_data), GFP_KERNEL);
    if (!vma_data)
        return -ENOMEM;
    
    vma_data->template = template;
    vma_data->va_to_pa_map = RB_ROOT;
    vma_data->is_first_vma = false;
    
    vma->vm_private_data = vma_data;
    vma->vm_ops = &snapshot_vm_ops;
    
    /* Make mappings read-only for COW */
    vma->vm_page_prot = pgprot_writeprotect(vma->vm_page_prot);
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
    
    return 0;
}
```

- [ ] **Step 2: Fix template lookup in mmap**

The template lookup by device name needs correction. Let me fix it:

```c
static int snapshot_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct snapshot_template *template;
    struct vma_snapshot_data *vma_data;
    struct miscdevice *mdev = file->private_data;
    char template_id[TEMPLATE_ID_MAX_LEN];
    const char *dev_name;
    
    /* Get device name and extract template ID */
    dev_name = file->f_path.dentry->d_name.name;
    if (strncmp(dev_name, "snapshot_", 9) != 0)
        return -EINVAL;
    
    strncpy(template_id, dev_name + 9, TEMPLATE_ID_MAX_LEN);
    
    template = snapshot_template_find(template_id);
    if (!template)
        return -EINVAL;
    
    /* Check size */
    if (vma->vm_end - vma->vm_start > template->total_size)
        return -EINVAL;
    
    /* Setup VMA data */
    vma_data = kzalloc(sizeof(*vma_data), GFP_KERNEL);
    if (!vma_data)
        return -ENOMEM;
    
    vma_data->template = template;
    vma_data->va_to_pa_map = RB_ROOT;
    vma_data->is_first_vma = false;
    
    vma->vm_private_data = vma_data;
    vma->vm_ops = &snapshot_vm_ops;
    
    /* Make mappings read-only for COW */
    vma->vm_page_prot = pgprot_writeprotect(vma->vm_page_prot);
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
    
    return 0;
}
```

- [ ] **Step 3: Update snapshot_types.h to export snapshot_fops**

Add to snapshot_types.h:

```c
/* File operations (defined in snapshot_mmap.c) */
extern const struct file_operations snapshot_fops;

/* Template functions (defined in snapshot_template.c) */
extern struct snapshot_template *snapshot_template_find(const char *id);
```

- [ ] **Step 4: Commit**

```bash
git add driver/snapshot_mmap.c driver/snapshot_types.h
git commit -m "feat(driver): add mmap and fault handler implementation"
```

---

## Component 5: Split Tool

### Task 7: Snapshot Split Tool Implementation

**Files:**
- Create: `tools/split_snapshot.c`
- Create: `tools/Makefile`

- [ ] **Step 1: Write split tool implementation**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/sha.h>

#define PAGE_SIZE 4096
#define SLOT_SIZE (PAGE_SIZE + 8)  /* 4KB + 8B header */
#define HASH_MODULUS (1ULL << 30)
#define MAX_PROBE_COUNT 64
#define HEADER_SIZE 8

struct page_table_entry {
    uint64_t va_offset;
    uint64_t hash_idx;
};

static uint64_t compute_hash_idx(const void *data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, PAGE_SIZE, hash);
    
    /* Use first 8 bytes as hash_idx */
    uint64_t hash_idx;
    memcpy(&hash_idx, hash, sizeof(hash_idx));
    return hash_idx;
}

static int write_page_to_slot(const char *pages_path, uint64_t hash_idx,
                               const void *data)
{
    int fd;
    uint64_t file_index;
    uint64_t slot_hash;
    uint32_t probe;
    uint8_t header[HEADER_SIZE];
    
    fd = open(pages_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    
    /* Open addressing */
    file_index = hash_idx % HASH_MODULUS;
    
    for (probe = 0; probe < MAX_PROBE_COUNT; probe++) {
        off_t offset = file_index * SLOT_SIZE;
        
        /* Read existing header (if any) */
        ssize_t ret = pread(fd, header, HEADER_SIZE, offset);
        
        if (ret < HEADER_SIZE) {
            /* Empty or new slot */
            memcpy(header, &hash_idx, HEADER_SIZE);
            pwrite(fd, header, HEADER_SIZE, offset);
            pwrite(fd, data, PAGE_SIZE, offset + HEADER_SIZE);
            close(fd);
            return 0;
        }
        
        memcpy(&slot_hash, header, HEADER_SIZE);
        
        if (slot_hash == hash_idx) {
            /* Already exists, same content - dedup */
            close(fd);
            return 0;
        }
        
        /* Linear probing */
        file_index = (file_index + 1) % HASH_MODULUS;
    }
    
    close(fd);
    return -1;  /* Max probe exceeded */
}

int main(int argc, char *argv[])
{
    int snap_fd, pt_fd;
    const char *snapshot_path;
    const char *output_dir;
    char pages_path[256];
    char pt_path[256];
    char meta_path[256];
    struct stat st;
    uint64_t total_size;
    uint64_t page_count;
    uint64_t unique_pages = 0;
    void *page_data;
    struct page_table_entry pte;
    FILE *meta_fp;
    
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <snapshot_file> <output_dir>\n", argv[0]);
        return 1;
    }
    
    snapshot_path = argv[1];
    output_dir = argv[2];
    
    snap_fd = open(snapshot_path, O_RDONLY);
    if (snap_fd < 0) {
        perror("open snapshot");
        return 1;
    }
    
    if (stat(snapshot_path, &st) < 0) {
        perror("stat");
        close(snap_fd);
        return 1;
    }
    
    total_size = st.st_size;
    page_count = total_size / PAGE_SIZE;
    
    /* Create output paths */
    snprintf(pages_path, sizeof(pages_path), "%s/pages.bin", output_dir);
    snprintf(pt_path, sizeof(pt_path), "%s/page_table.bin", output_dir);
    snprintf(meta_path, sizeof(meta_path), "%s/metadata.json", output_dir);
    
    /* Open output files */
    pt_fd = open(pt_path, O_WRONLY | O_CREAT, 0644);
    if (pt_fd < 0) {
        perror("open page_table");
        close(snap_fd);
        return 1;
    }
    
    page_data = malloc(PAGE_SIZE);
    if (!page_data) {
        close(snap_fd);
        close(pt_fd);
        return 1;
    }
    
    /* Process each page */
    for (uint64_t va = 0; va < total_size; va += PAGE_SIZE) {
        ssize_t ret = pread(snap_fd, page_data, PAGE_SIZE, va);
        if (ret != PAGE_SIZE) {
            perror("read page");
            break;
        }
        
        /* Compute hash */
        uint64_t hash_idx = compute_hash_idx(page_data);
        
        /* Write to page table */
        pte.va_offset = va;
        pte.hash_idx = hash_idx;
        write(pt_fd, &pte, sizeof(pte));
        
        /* Write to pages.bin (dedup) */
        if (write_page_to_slot(pages_path, hash_idx, page_data) == 0) {
            unique_pages++;
        }
    }
    
    /* Write metadata */
    meta_fp = fopen(meta_path, "w");
    if (meta_fp) {
        fprintf(meta_fp, "{\n");
        fprintf(meta_fp, "  \"template_id\": \"%s\",\n", output_dir);
        fprintf(meta_fp, "  \"total_size\": %llu,\n", total_size);
        fprintf(meta_fp, "  \"page_count\": %llu,\n", page_count);
        fprintf(meta_fp, "  \"unique_pages\": %llu,\n", unique_pages);
        fprintf(meta_fp, "  \"hash_modulus\": %llu\n", HASH_MODULUS);
        fprintf(meta_fp, "}\n");
        fclose(meta_fp);
    }
    
    free(page_data);
    close(snap_fd);
    close(pt_fd);
    
    printf("Processed %llu pages, %llu unique\n", page_count, unique_pages);
    
    return 0;
}
```

- [ ] **Step 2: Create tools Makefile**

```make
CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lcrypto

all: split_snapshot

split_snapshot: split_snapshot.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f split_snapshot

install:
	cp split_snapshot /usr/local/bin/
```

- [ ] **Step 3: Commit**

```bash
git add tools/split_snapshot.c tools/Makefile
git commit -m "feat(tool): add snapshot split tool"
```

---

## Component 6: Testing

### Task 8: Test Infrastructure

**Files:**
- Create: `tests/generate_test_snapshot.sh`
- Create: `tests/test_ioctl.c`
- Create: `tests/test_mmap.c`
- Create: `tests/Makefile`

- [ ] **Step 1: Write test snapshot generator**

```bash
#!/bin/bash
# Generate test snapshot files

OUTPUT_DIR=${1:-"test_snapshot"}
SIZE_MB=${2:-4}

mkdir -p "$OUTPUT_DIR"

# Generate 4MB test snapshot with repeating patterns
total_pages=$((SIZE_MB * 256))
page_size=4096

# Create snapshot.mem with repeating patterns (for dedup test)
dd if=/dev/zero bs=$page_size count=$total_pages of="$OUTPUT_DIR/snapshot.mem" 2>/dev/null

# Add some unique pages
for i in 1 2 3 4; do
    offset=$((i * page_size))
    dd if=/dev/urandom bs=$page_size count=1 of="$OUTPUT_DIR/snapshot.mem" \
       seek=$((offset / page_size)) conv=notrunc 2>/dev/null
done

# Run split tool
split_snapshot "$OUTPUT_DIR/snapshot.mem" "$OUTPUT_DIR"

echo "Generated test snapshot in $OUTPUT_DIR"
ls -la "$OUTPUT_DIR"
```

- [ ] **Step 2: Write ioctl test program**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "snapshot_types.h"

int main(int argc, char *argv[])
{
    int fd;
    struct ioctl_create_template create_args;
    struct ioctl_template_status status_args;
    struct ioctl_delete_template delete_args;
    int ret;
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <control_device>\n", argv[0]);
        return 1;
    }
    
    /* Open control device */
    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open control device");
        return 1;
    }
    
    /* Create template */
    memset(&create_args, 0, sizeof(create_args));
    strcpy(create_args.template_id, "test_001");
    create_args.total_size = 4 * 1024 * 1024;  /* 4MB */
    strcpy(create_args.page_table_path, "test_snapshot/page_table.bin");
    strcpy(create_args.pages_path, "test_snapshot/pages.bin");
    
    ret = ioctl(fd, IOCTL_CREATE_TEMPLATE, &create_args);
    if (ret < 0) {
        perror("ioctl create");
        close(fd);
        return 1;
    }
    
    printf("Created template: /dev/snapshot_test_001\n");
    
    /* Get status */
    memset(&status_args, 0, sizeof(status_args));
    strcpy(status_args.template_id, "test_001");
    
    ret = ioctl(fd, IOCTL_TEMPLATE_STATUS, &status_args);
    if (ret < 0) {
        perror("ioctl status");
    } else {
        printf("Status: mmap_count=%llu, loaded_pages=%llu\n",
               status_args.mmap_count, status_args.loaded_pages);
    }
    
    /* Delete template */
    memset(&delete_args, 0, sizeof(delete_args));
    strcpy(delete_args.template_id, "test_001");
    
    ret = ioctl(fd, IOCTL_DELETE_TEMPLATE, &delete_args);
    if (ret < 0) {
        perror("ioctl delete");
    } else {
        printf("Deleted template\n");
    }
    
    close(fd);
    return 0;
}
```

- [ ] **Step 3: Write mmap test program**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    int fd;
    void *addr;
    size_t size = 4 * 1024 * 1024;  /* 4MB */
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <snapshot_device>\n", argv[0]);
        return 1;
    }
    
    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open snapshot device");
        return 1;
    }
    
    /* mmap */
    addr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    printf("Mapped %zu bytes at %p\n", size, addr);
    
    /* Read first page (trigger fault) */
    printf("First page content (first 32 bytes):\n");
    for (int i = 0; i < 32; i++) {
        printf("%02x ", ((unsigned char *)addr)[i]);
        if ((i + 1) % 16 == 0)
            printf("\n");
    }
    
    /* Cleanup */
    munmap(addr, size);
    close(fd);
    
    return 0;
}
```

- [ ] **Step 4: Create tests Makefile**

```make
CC = gcc
CFLAGS = -Wall -O2

all: test_ioctl test_mmap

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) -I../driver -o $@ $<

test_mmap: test_mmap.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f test_ioctl test_mmap
```

- [ ] **Step 5: Commit**

```bash
git add tests/generate_test_snapshot.sh tests/test_ioctl.c tests/test_mmap.c tests/Makefile
git commit -m "feat(test): add test infrastructure"
```

---

## Build and Integration

### Task 9: Full Build System

**Files:**
- Modify: `driver/snapshot_types.h`
- Create: `Makefile` (root)

- [ ] **Step 1: Add user-space ioctl header copy for tests**

Create `tests/snapshot_types.h` as copy of driver header for user-space tests:

```c
/* User-space copy of ioctl structures for testing */
#ifndef _SNAPSHOT_TYPES_USER_H
#define _SNAPSHOT_TYPES_USER_H

#include <stdint.h>
#include <sys/ioctl.h>

#define TEMPLATE_ID_MAX_LEN     64
#define PATH_MAX_LEN            256

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

#endif
```

- [ ] **Step 2: Create root Makefile**

```make
.PHONY: all driver tools tests clean

all: driver tools tests

driver:
	$(MAKE) -C driver

tools:
	$(MAKE) -C tools

tests:
	$(MAKE) -C tests

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean
```

- [ ] **Step 3: Commit**

```bash
git add tests/snapshot_types.h Makefile
git commit -m "feat: add root build system and user-space headers"
```

---

## Summary

This plan implements:
1. **Snapshot Driver**: Kernel module with template management, global page pool, mmap/fault handlers
2. **Split Tool**: User-space tool to convert full snapshots to dedup format
3. **Tests**: ioctl and mmap test programs

**Key features implemented:**
- Per-template device files (`/dev/snapshot_{id}`)
- Open addressing for pages.bin lookup
- Global RO page pool with reference counting
- VMA mapping copy for subsequent VMs
- Kernel standard COW for writes

Plan complete and saved to `docs/superpowers/plans/2026-04-20-memory-snapshot-driver.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints for review

**Which approach?**