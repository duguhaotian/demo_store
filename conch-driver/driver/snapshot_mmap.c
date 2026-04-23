#include "snapshot_types.h"
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <asm/pgtable.h>

/* Forward declarations */
extern long snapshot_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* VMA operations for on-demand page loading */
static void snapshot_vma_open(struct vm_area_struct *vma);
static void snapshot_vma_close(struct vm_area_struct *vma);
static vm_fault_t snapshot_vma_fault(struct vm_fault *vmf);

static const struct vm_operations_struct snapshot_vm_ops = {
    .open = snapshot_vma_open,
    .close = snapshot_vma_close,
    .fault = snapshot_vma_fault,
};

/* File operations */
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

static int snapshot_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int snapshot_release(struct inode *inode, struct file *file)
{
    return 0;
}

/* VMA open: increment template ref_count */
static void snapshot_vma_open(struct vm_area_struct *vma)
{
    struct vma_snapshot_data *vma_data = vma->vm_private_data;

    if (vma_data && vma_data->template) {
        atomic_inc(&vma_data->template->ref_count);
        pr_debug("vma_open: template='%s', ref_count=%d\n",
                 vma_data->template->template_id,
                 atomic_read(&vma_data->template->ref_count));
    }
}

/* VMA close: decrement template ref_count, unref mapped pages */
static void snapshot_vma_close(struct vm_area_struct *vma)
{
    struct vma_snapshot_data *vma_data = vma->vm_private_data;
    struct snapshot_template *template;
    struct rb_node *node;
    struct va_to_pa_entry *entry;

    if (!vma_data)
        return;

    template = vma_data->template;
    if (!template)
        return;

    /* Unref all mapped pages */
    if (vma_data->va_to_pa_map.rb_node) {
        while (!RB_EMPTY_ROOT(&vma_data->va_to_pa_map)) {
            node = rb_first(&vma_data->va_to_pa_map);
            entry = rb_entry(node, struct va_to_pa_entry, rb_node);
            rb_erase(node, &vma_data->va_to_pa_map);

            if (entry->phys_entry) {
                /* Put the extra ref from get_page() in fault handler */
                put_page(entry->phys_entry->page);
                /* Also unref from pool */
                snapshot_pool_unref(&g_driver_state->page_pool, entry->phys_entry);
            }
            kfree(entry);
        }
    }

    pr_info("vma_close: template='%s', ref_count before unref=%d\n",
            template->template_id, atomic_read(&template->ref_count));

    kfree(vma_data);
    snapshot_template_unref(template);
}

/* RB-tree helpers for tracking mapped pages */
static struct va_to_pa_entry *va_lookup(struct rb_root *root, uint64_t va_offset)
{
    struct rb_node *node = root->rb_node;

    while (node) {
        struct va_to_pa_entry *entry = rb_entry(node, struct va_to_pa_entry, rb_node);

        if (va_offset < entry->va_offset)
            node = node->rb_left;
        else if (va_offset > entry->va_offset)
            node = node->rb_right;
        else
            return entry;
    }
    return NULL;
}

static int va_insert(struct rb_root *root, struct va_to_pa_entry *new)
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

/*
 * Fault handler - on-demand page loading
 *
 * Called when user accesses a page that hasn't been mapped yet.
 * Loads page from pages.bin into pool and maps it to user space.
 */
static vm_fault_t snapshot_vma_fault(struct vm_fault *vmf)
{
    struct vma_snapshot_data *vma_data;
    struct snapshot_template *template;
    struct page_table_entry *pte;
    struct phys_page_entry *phys_entry;
    struct va_to_pa_entry *va_entry;
    uint64_t page_index;
    uint64_t va_offset;
    void *page_data;
    vm_fault_t ret = VM_FAULT_NOPAGE;
    loff_t offset;
    ssize_t read_ret;
    uint64_t file_index;
    uint64_t slot_hash;
    uint32_t probe;
    char header[8];

    vma_data = vmf->vma->vm_private_data;
    if (!vma_data)
        return VM_FAULT_SIGBUS;

    template = vma_data->template;
    if (!template)
        return VM_FAULT_SIGBUS;

    /* Calculate page index from fault address */
    va_offset = vmf->address - vmf->vma->vm_start;
    page_index = va_offset / PAGE_SIZE;

    pr_debug("fault: template='%s', page_index=%llu, address=0x%lx\n",
             template->template_id, page_index, vmf->address);

    /* Bounds check */
    if (page_index >= template->page_count) {
        pr_err("fault: page_index %llu out of bounds (max=%llu)\n",
               page_index, template->page_count - 1);
        return VM_FAULT_SIGBUS;
    }

    /* Get hash_idx from page table */
    pte = &template->page_table_cache[page_index];

    /* Lookup page in pool (may already be loaded by another process) */
    phys_entry = snapshot_pool_lookup(&g_driver_state->page_pool, pte->hash_idx);
    if (phys_entry) {
        /* Page already in pool - map it */
        pr_debug("fault: page_index=%llu hash_idx=%llu found in pool\n",
                 page_index, pte->hash_idx);
        goto map_page;
    }

    /* Page not in pool - need to load from pages.bin */
    pr_debug("fault: loading page_index=%llu hash_idx=%llu from file\n",
             page_index, pte->hash_idx);

    /* Allocate temporary buffer for page data */
    page_data = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!page_data)
        return VM_FAULT_OOM;

    /* Open addressing lookup in pages.bin */
    file_index = pte->hash_idx % g_driver_state->hash_modulus;

    for (probe = 0; probe < g_driver_state->max_probe_count; probe++) {
        offset = file_index * SLOT_SIZE;

        /* Read header (8 bytes) */
        read_ret = kernel_read(template->pages_file, header, 8, &offset);
        if (read_ret != 8) {
            pr_err("fault: failed read header at offset %lld\n", offset);
            kfree(page_data);
            return VM_FAULT_SIGBUS;
        }

        slot_hash = *((uint64_t *)header);

        if (slot_hash == pte->hash_idx) {
            /* Found - read page data */
            offset = file_index * SLOT_SIZE + 8;
            read_ret = kernel_read(template->pages_file, page_data, PAGE_SIZE, &offset);
            if (read_ret != PAGE_SIZE) {
                pr_err("fault: failed read page data\n");
                kfree(page_data);
                return VM_FAULT_SIGBUS;
            }

            /* Add to pool */
            phys_entry = snapshot_pool_add(&g_driver_state->page_pool, pte->hash_idx, page_data);
            kfree(page_data);

            if (!phys_entry) {
                pr_err("fault: failed add to pool\n");
                return VM_FAULT_OOM;
            }

            pr_debug("fault: loaded page_index=%llu into pool\n", page_index);
            goto map_page;
        }

        if (slot_hash == 0) {
            /* Empty slot - page not found */
            pr_err("fault: hash_idx=%llu NOT_FOUND in pages.bin\n", pte->hash_idx);
            kfree(page_data);
            return VM_FAULT_SIGBUS;
        }

        /* Linear probing */
        file_index = (file_index + 1) % g_driver_state->hash_modulus;
    }

    pr_err("fault: max probes exceeded for hash_idx=%llu\n", pte->hash_idx);
    kfree(page_data);
    return VM_FAULT_SIGBUS;

map_page:
    /* Track this mapping for cleanup */
    va_entry = kmalloc(sizeof(*va_entry), GFP_ATOMIC);
    if (va_entry) {
        va_entry->va_offset = va_offset;
        va_entry->phys_entry = phys_entry;
        /* Note: va_insert may fail if already exists (race), ignore */
        va_insert(&vma_data->va_to_pa_map, va_entry);
    }

    /* get_page() for correct RSS accounting */
    get_page(phys_entry->page);

    /* Map page into user space - kernel handles COW for MAP_PRIVATE */
    ret = vmf_insert_page(vmf, phys_entry->page);

    if (ret & VM_FAULT_ERROR) {
        pr_err("fault: vmf_insert_page failed: 0x%x\n", ret);
        put_page(phys_entry->page);
        snapshot_pool_unref(&g_driver_state->page_pool, phys_entry);
    }

    return ret;
}

/*
 * mmap handler - setup VMA for on-demand loading
 *
 * Key design:
 * - No preloading, pages loaded on fault
 * - vm_ops.fault handles page loading and mapping
 * - Kernel handles COW automatically for MAP_PRIVATE
 */
static int snapshot_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct snapshot_template *template;
    struct vma_snapshot_data *vma_data;
    char template_id[TEMPLATE_ID_MAX_LEN];
    const char *dev_name;
    uint64_t size;

    /* Extract template ID */
    dev_name = file->f_path.dentry->d_name.name;
    if (strncmp(dev_name, "snapshot_", 9) != 0) {
        pr_err("mmap: invalid device name '%s'\n", dev_name);
        return -EINVAL;
    }

    strncpy(template_id, dev_name + 9, TEMPLATE_ID_MAX_LEN - 1);
    template_id[TEMPLATE_ID_MAX_LEN - 1] = '\0';

    pr_info("mmap: template='%s', vma[0x%lx-0x%lx], size=%lu\n",
            template_id, vma->vm_start, vma->vm_end,
            vma->vm_end - vma->vm_start);

    /* Find template */
    template = snapshot_template_find_ref(template_id);
    if (!template) {
        pr_err("mmap: template '%s' not found\n", template_id);
        return -EINVAL;
    }

    /* Check size */
    size = vma->vm_end - vma->vm_start;
    if (size % PAGE_SIZE != 0 || size > template->total_size) {
        pr_err("mmap: invalid size %llu (total_size=%llu)\n", size, template->total_size);
        snapshot_template_unref(template);
        return -EINVAL;
    }

    /* Allocate VMA tracking data */
    vma_data = kzalloc(sizeof(*vma_data), GFP_KERNEL);
    if (!vma_data) {
        snapshot_template_unref(template);
        return -ENOMEM;
    }
    vma_data->template = template;
    vma_data->va_to_pa_map = RB_ROOT;

    /* Setup VMA ops - pages will be loaded on fault */
    vma->vm_ops = &snapshot_vm_ops;
    vma->vm_private_data = vma_data;
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    /* Increment ref for VMA (vma_close will decrement) */
    atomic_inc(&template->ref_count);

    pr_info("mmap: SUCCESS - on-demand loading enabled\n");
    return 0;
}