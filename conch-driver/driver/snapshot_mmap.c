#include "snapshot_types.h"
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/compiler.h>
#include <asm/pgtable.h>

/* Forward declarations */
extern long snapshot_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int read_page_data(struct file *file, uint64_t hash_idx, void *data);

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

/* Preload all pages for a template (called during mmap, before fault handling) */
int snapshot_preload_template(struct snapshot_template *template)
{
    struct page_table_entry *pte;
    void *page_data;
    uint64_t i;
    int ret;
    int failed_pages = 0;

    pr_info("snapshot_driver: preloading template '%s' (%llu pages)\n",
            template->template_id, template->page_count);

    page_data = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!page_data)
        return -ENOMEM;

    /* Load each page using cached page_table */
    for (i = 0; i < template->page_count; i++) {
        pte = &template->page_table_cache[i];
        pr_info("preload[%llu]: va=%llu, hash_idx=%llu\n",
                i, pte->va_offset, pte->hash_idx);

        /* Check if page already in pool (no ref increment) */
        if (snapshot_pool_lookup_noref(&g_driver_state->page_pool, pte->hash_idx)) {
            pr_info("preload[%llu]: hash_idx=%llu already in pool (dedup)\n",
                    i, pte->hash_idx);
            continue;  /* Already loaded (dedup) */
        }

        /* Read page data from pages.bin */
        ret = read_page_data(template->pages_file, pte->hash_idx, page_data);
        if (ret) {
            pr_err("snapshot_driver: failed to read page data for hash_idx=%llu\n",
                   pte->hash_idx);
            failed_pages++;
            continue;
        }

        /* Add to pool (ref_count = 1, will be balanced by fault handler) */
        if (!snapshot_pool_add(&g_driver_state->page_pool, pte->hash_idx, page_data)) {
            pr_err("snapshot_driver: failed to add page to pool\n");
            failed_pages++;
        }
    }

    kfree(page_data);

    /* Fail if any pages couldn't be loaded */
    if (failed_pages > 0) {
        pr_err("snapshot_driver: preload failed for %d pages\n", failed_pages);
        return -EIO;
    }

    pr_info("snapshot_driver: preload complete, loaded %llu unique pages\n",
            g_driver_state->page_pool.total_pages);
    return 0;
}

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

/* Read page from pre-opened pages.bin using open addressing */
static int read_page_data(struct file *file, uint64_t hash_idx,
                           void *data)
{
    loff_t offset;
    ssize_t ret;
    uint64_t file_index;
    uint64_t slot_hash;
    uint32_t probe;
    char header[8];

    /* Open addressing lookup */
    file_index = hash_idx % g_driver_state->hash_modulus;
    pr_info("read_page: hash_idx=%llu, modulus=%llu, initial_file_index=%llu\n",
            hash_idx, g_driver_state->hash_modulus, file_index);

    for (probe = 0; probe < g_driver_state->max_probe_count; probe++) {
        offset = file_index * SLOT_SIZE;

        /* Read header */
        ret = kernel_read(file, header, 8, &offset);
        if (ret != 8) {
            return -EIO;
        }

        slot_hash = *((uint64_t *)header);

        pr_info("read_page: probe=%u, file_index=%llu, offset=%lld, slot_hash=%llu\n",
                probe, file_index, (long long)offset, slot_hash);

        if (slot_hash == hash_idx) {
            /* Found matching slot */
            pr_info("read_page: MATCH at probe=%u, file_index=%llu\n",
                    probe, file_index);
            offset = file_index * SLOT_SIZE + 8;
            ret = kernel_read(file, data, PAGE_SIZE, &offset);

            if (ret != PAGE_SIZE)
                return -EIO;

            return 0;
        }

        if (slot_hash == 0) {
            /* Empty slot, page not found */
            pr_info("read_page: NOT_FOUND (empty slot at probe=%u, file_index=%llu)\n",
                    probe, file_index);
            return -ENOENT;
        }

        /* Linear probing */
        file_index = (file_index + 1) % g_driver_state->hash_modulus;
    }

    pr_info("read_page: NOT_FOUND (max probes=%u exceeded)\n",
            g_driver_state->max_probe_count);
    return -ENOENT;  /* Max probe count exceeded */
}

/* Fault handler - only maps preloaded pages, uses cached page_table */
static vm_fault_t snapshot_fault(struct vm_fault *vmf)
{
    struct vma_snapshot_data *vma_data;
    struct snapshot_template *template;
    struct page_table_entry *pte;
    struct phys_page_entry *phys_entry;
    struct va_to_pa_entry *va_entry;
    uint64_t va_offset;
    uint64_t page_index;
    unsigned long pfn;
    vm_fault_t ret;

    vma_data = vmf->vma->vm_private_data;
    template = vma_data->template;
    va_offset = vmf->address - vmf->vma->vm_start;
    page_index = va_offset / PAGE_SIZE;

    pr_info("fault: address=0x%lx, va_offset=%llu, page_index=%llu\n",
            vmf->address, va_offset, page_index);

    /* Check bounds */
    if (page_index >= template->page_count) {
        pr_err("snapshot_driver: page_index out of bounds: %llu >= %llu\n",
               page_index, template->page_count);
        return VM_FAULT_SIGBUS;
    }

    /* Check if already mapped */
    va_entry = va_to_pa_lookup(&vma_data->va_to_pa_map, va_offset);
    if (va_entry) {
        /* Already mapped in this VMA */
        return VM_FAULT_NOPAGE;
    }

    /* Get hash_idx from cached page_table */
    pte = &template->page_table_cache[page_index];

    pr_info("fault: page_index=%llu -> hash_idx=%llu\n",
            page_index, pte->hash_idx);

    /* Lookup in global pool (should be preloaded) */
    phys_entry = snapshot_pool_lookup(&g_driver_state->page_pool, pte->hash_idx);
    if (!phys_entry) {
        pr_err("fault: hash_idx=%llu NOT in pool - preload missing?\n",
               pte->hash_idx);
        return VM_FAULT_SIGBUS;
    }
    pr_info("fault: hash_idx=%llu FOUND in pool\n", pte->hash_idx);

    /* Map page to VMA - use vmf_insert_pfn for PFN mappings */
    pfn = page_to_pfn(phys_entry->page);
    ret = vmf_insert_pfn(vmf->vma, vmf->address, pfn);
    if (ret != VM_FAULT_NOPAGE) {
        pr_err("snapshot_driver: vmf_insert_pfn_prot failed: %d\n", ret);
        snapshot_pool_unref(&g_driver_state->page_pool, phys_entry);
        return ret;
    }

    /* Record mapping */
    va_entry = kzalloc(sizeof(*va_entry), GFP_ATOMIC);
    if (va_entry) {
        va_entry->va_offset = va_offset;
        va_entry->phys_entry = phys_entry;
        va_to_pa_insert(&vma_data->va_to_pa_map, va_entry);
    }

    pr_debug("snapshot_driver: mapped page at va_offset=%llu, hash_idx=%llu\n",
             va_offset, pte->hash_idx);

    return VM_FAULT_NOPAGE;
}

/* VMA open: increment ref count and copy mappings if applicable */
static void snapshot_vma_open(struct vm_area_struct *vma)
{
    struct vma_snapshot_data *vma_data;
    struct snapshot_template *template;
    struct vma_snapshot_data *first_vma;
    struct va_to_pa_entry *entry;
    int ret;

    vma_data = vma->vm_private_data;
    template = vma_data->template;

    atomic_inc(&template->ref_count);

    pr_debug("snapshot_driver: vma_open, ref_count=%d\n",
             atomic_read(&template->ref_count));

    /* Read first_vma_data atomically */
    first_vma = READ_ONCE(template->first_vma_data);

    if (!first_vma) {
        /* Try to set as first_vma_data */
        if (cmpxchg(&template->first_vma_data, NULL, vma_data) == NULL) {
            vma_data->is_first_vma = true;
            return;  /* Successfully set as first */
        }
        /* Someone else set it first, re-read */
        first_vma = READ_ONCE(template->first_vma_data);
    }

    /* Copy mappings from first VMA if not this one */
    if (first_vma && first_vma != vma_data) {
        struct rb_node *node;
        struct va_to_pa_entry *new_entry;
        unsigned long pfn;

        for (node = rb_first(&first_vma->va_to_pa_map);
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
            new_entry = kzalloc(sizeof(*new_entry), GFP_ATOMIC);
            if (new_entry) {
                new_entry->va_offset = entry->va_offset;
                new_entry->phys_entry = entry->phys_entry;
                va_to_pa_insert(&vma_data->va_to_pa_map, new_entry);
            }
        }
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

    /* Clear first_vma_data if this was it (atomic) */
    if (READ_ONCE(template->first_vma_data) == vma_data) {
        WRITE_ONCE(template->first_vma_data, NULL);
    }

    kfree(vma_data);

    /* Decrement template ref_count, may free if zero */
    snapshot_template_unref(template);
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

/* mmap handler - preload pages on first mmap */
static int snapshot_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct snapshot_template *template;
    struct vma_snapshot_data *vma_data;
    char template_id[TEMPLATE_ID_MAX_LEN];
    const char *dev_name;
    int ret;

    /* Get device name and extract template ID */
    dev_name = file->f_path.dentry->d_name.name;
    if (strncmp(dev_name, "snapshot_", 9) != 0)
        return -EINVAL;

    strncpy(template_id, dev_name + 9, TEMPLATE_ID_MAX_LEN);

    pr_info("mmap: template='%s', vm_start=0x%lx, vm_end=0x%lx, size=%lu\n",
            template_id, vma->vm_start, vma->vm_end, vma->vm_end - vma->vm_start);

    /* Get template with ref_count increment */
    template = snapshot_template_find_ref(template_id);
    if (!template)
        return -EINVAL;

    /* Check size */
    if (vma->vm_end - vma->vm_start > template->total_size) {
        snapshot_template_unref(template);
        return -EINVAL;
    }

    /* Preload all pages if not already done */
    if (atomic_cmpxchg(&template->preload_done, 0, 1) == 0) {
        /* First mmap - preload pages */
        pr_info("mmap: preload_needed=true, starting preload\n");
        ret = snapshot_preload_template(template);
        if (ret) {
            atomic_set(&template->preload_done, 0);  /* Reset for retry */
            snapshot_template_unref(template);
            pr_err("mmap: preload FAILED, ret=%d\n", ret);
            return ret;
        }
    } else {
        pr_info("mmap: preload_needed=false (already done)\n");
    }

    /* Setup VMA data */
    vma_data = kzalloc(sizeof(*vma_data), GFP_KERNEL);
    if (!vma_data) {
        snapshot_template_unref(template);
        return -ENOMEM;
    }

    vma_data->template = template;
    vma_data->va_to_pa_map = RB_ROOT;
    vma_data->is_first_vma = false;

    vma->vm_private_data = vma_data;
    vma->vm_ops = &snapshot_vm_ops;

    /* Setup VMA for PFN mappings (read-only by default) */
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_PFNMAP | VM_DONTCOPY;

    pr_debug("snapshot_driver: mmap complete, ref_count=%d\n",
             atomic_read(&template->ref_count));

    return 0;
}