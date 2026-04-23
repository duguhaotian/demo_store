#include "snapshot_types.h"
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <asm/pgtable.h>

/* Forward declarations */
extern long snapshot_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* VMA operations for proper lifecycle management */
static void snapshot_vma_open(struct vm_area_struct *vma);
static void snapshot_vma_close(struct vm_area_struct *vma);

static const struct vm_operations_struct snapshot_vm_ops = {
    .open = snapshot_vma_open,
    .close = snapshot_vma_close,
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
    struct snapshot_template *template = vma->vm_private_data;

    if (template) {
        atomic_inc(&template->ref_count);
        pr_debug("vma_open: template='%s', ref_count=%d\n",
                 template->template_id, atomic_read(&template->ref_count));
    }
}

/* VMA close: decrement template ref_count, unref mapped pages */
static void snapshot_vma_close(struct vm_area_struct *vma)
{
    struct snapshot_template *template = vma->vm_private_data;
    struct vma_snapshot_data *vma_data;
    struct rb_node *node;
    struct va_to_pa_entry *entry;

    if (!template)
        return;

    vma_data = vma->vm_private_data;

    /* Unref all mapped pages */
    if (vma_data && vma_data->va_to_pa_map.rb_node) {
        while (!RB_EMPTY_ROOT(&vma_data->va_to_pa_map)) {
            node = rb_first(&vma_data->va_to_pa_map);
            entry = rb_entry(node, struct va_to_pa_entry, rb_node);
            rb_erase(node, &vma_data->va_to_pa_map);

            if (entry->phys_entry) {
                /* Put the extra ref from get_page() in mmap */
                put_page(entry->phys_entry->page);
                /* Also unref from pool */
                snapshot_pool_unref(&g_driver_state->page_pool, entry->phys_entry);
            }
            kfree(entry);
        }
        kfree(vma_data);
    }

    pr_info("vma_close: template='%s', ref_count before unref=%d\n",
            template->template_id, atomic_read(&template->ref_count));

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
 * mmap handler - use remap_pfn_range with proper page ref management
 *
 * Key design:
 * - get_page() before remap_pfn_range for correct RSS/PSS accounting
 * - vm_ops tracks VMA lifecycle, releases refs on close
 * - Kernel handles COW for MAP_PRIVATE automatically
 */
static int snapshot_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct snapshot_template *template;
    struct vma_snapshot_data *vma_data;
    char template_id[TEMPLATE_ID_MAX_LEN];
    const char *dev_name;
    uint64_t size;
    uint64_t page_count;
    uint64_t i;
    struct page_table_entry *pte;
    struct phys_page_entry *phys_entry;
    struct va_to_pa_entry *va_entry;
    unsigned long pfn;
    int ret;

    /* Extract template ID */
    dev_name = file->f_path.dentry->d_name.name;
    if (strncmp(dev_name, "snapshot_", 9) != 0) {
        pr_err("mmap: invalid device name '%s'\n", dev_name);
        return -EINVAL;
    }

    strncpy(template_id, dev_name + 9, TEMPLATE_ID_MAX_LEN - 1);
    template_id[TEMPLATE_ID_MAX_LEN - 1] = '\0';

    pr_info("mmap: template='%s', vma[0x%lx-0x%lx], size=%lu, flags=0x%lx\n",
            template_id, vma->vm_start, vma->vm_end,
            vma->vm_end - vma->vm_start, vma->vm_flags);

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

    /* Preload if needed */
    if (atomic_cmpxchg(&template->preload_done, 0, 1) == 0) {
        pr_info("mmap: preload required for template '%s'\n", template_id);
        ret = snapshot_preload_template(template);
        if (ret) {
            atomic_set(&template->preload_done, 0);
            snapshot_template_unref(template);
            pr_err("mmap: preload FAILED ret=%d\n", ret);
            return ret;
        }
    }

    /* Allocate VMA tracking data */
    vma_data = kzalloc(sizeof(*vma_data), GFP_KERNEL);
    if (!vma_data) {
        snapshot_template_unref(template);
        return -ENOMEM;
    }
    vma_data->template = template;
    vma_data->va_to_pa_map = RB_ROOT;

    page_count = size / PAGE_SIZE;
    pr_info("mmap: mapping %llu pages\n", page_count);

    /* Map pages with proper ref counting for RSS/PSS */
    for (i = 0; i < page_count; i++) {
        pte = &template->page_table_cache[i];

        phys_entry = snapshot_pool_lookup(&g_driver_state->page_pool, pte->hash_idx);
        if (!phys_entry) {
            pr_err("mmap: page %llu hash_idx=%llu NOT in pool\n", i, pte->hash_idx);
            /* Cleanup already mapped pages */
            while (!RB_EMPTY_ROOT(&vma_data->va_to_pa_map)) {
                struct rb_node *node = rb_first(&vma_data->va_to_pa_map);
                va_entry = rb_entry(node, struct va_to_pa_entry, rb_node);
                rb_erase(node, &vma_data->va_to_pa_map);
                put_page(va_entry->phys_entry->page);
                snapshot_pool_unref(&g_driver_state->page_pool, va_entry->phys_entry);
                kfree(va_entry);
            }
            kfree(vma_data);
            snapshot_template_unref(template);
            return -EFAULT;
        }

        /* get_page() for correct RSS accounting - kernel tracks _mapcount */
        get_page(phys_entry->page);

        pfn = page_to_pfn(phys_entry->page);
        ret = remap_pfn_range(vma,
                              vma->vm_start + i * PAGE_SIZE,
                              pfn,
                              PAGE_SIZE,
                              vma->vm_page_prot);

        if (ret) {
            pr_err("mmap: remap_pfn_range failed page %llu: %d\n", i, ret);
            put_page(phys_entry->page);
            snapshot_pool_unref(&g_driver_state->page_pool, phys_entry);
            /* Cleanup... */
            while (!RB_EMPTY_ROOT(&vma_data->va_to_pa_map)) {
                struct rb_node *node = rb_first(&vma_data->va_to_pa_map);
                va_entry = rb_entry(node, struct va_to_pa_entry, rb_node);
                rb_erase(node, &vma_data->va_to_pa_map);
                put_page(va_entry->phys_entry->page);
                snapshot_pool_unref(&g_driver_state->page_pool, va_entry->phys_entry);
                kfree(va_entry);
            }
            kfree(vma_data);
            snapshot_template_unref(template);
            return ret;
        }

        /* Track mapped page for cleanup in vma_close */
        va_entry = kmalloc(sizeof(*va_entry), GFP_KERNEL);
        if (va_entry) {
            va_entry->va_offset = i * PAGE_SIZE;
            va_entry->phys_entry = phys_entry;
            va_insert(&vma_data->va_to_pa_map, va_entry);
        }

        /* Pool ref will be released in vma_close */
    }

    /* Setup VMA ops for lifecycle management */
    vma->vm_ops = &snapshot_vm_ops;
    vma->vm_private_data = vma_data;
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    /* Increment ref for VMA (vma_close will decrement) */
    atomic_inc(&template->ref_count);

    pr_info("mmap: SUCCESS - %llu pages mapped, RSS will show correctly\n", page_count);
    return 0;
}