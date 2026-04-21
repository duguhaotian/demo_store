#include "snapshot_types.h"
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <asm/pgtable.h>

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

/* Read page_table entry from pre-opened file */
static int read_page_table_entry(struct file *file, uint64_t va,
                                  struct page_table_entry *pte)
{
    loff_t offset;
    ssize_t ret;

    offset = (va / PAGE_SIZE) * PAGE_TABLE_ENTRY_SIZE;

    ret = kernel_read(file, pte, sizeof(*pte), &offset);

    if (ret != sizeof(*pte))
        return -EIO;

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

    for (probe = 0; probe < g_driver_state->max_probe_count; probe++) {
        offset = file_index * SLOT_SIZE;

        /* Read header */
        ret = kernel_read(file, header, 8, &offset);
        if (ret != 8) {
            return -EIO;
        }

        slot_hash = *((uint64_t *)header);

        if (slot_hash == hash_idx) {
            /* Found matching slot */
            offset = file_index * SLOT_SIZE + 8;
            ret = kernel_read(file, data, PAGE_SIZE, &offset);

            if (ret != PAGE_SIZE)
                return -EIO;

            return 0;
        }

        if (slot_hash == 0) {
            /* Empty slot, page not found */
            return -ENOENT;
        }

        /* Linear probing */
        file_index = (file_index + 1) % g_driver_state->hash_modulus;
    }

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

    pr_debug("snapshot_driver: fault at va_offset=%llu\n", va_offset);

    /* Check if already mapped */
    va_entry = va_to_pa_lookup(&vma_data->va_to_pa_map, va_offset);
    if (va_entry) {
        /* Already mapped in this VMA */
        return VM_FAULT_NOPAGE;
    }

    /* Read page_table entry from pre-opened file */
    ret = read_page_table_entry(template->page_table_file, va_offset, &pte);
    if (ret) {
        pr_err("snapshot_driver: read_page_table_entry failed: %d\n", ret);
        return VM_FAULT_SIGBUS;
    }

    /* Lookup in global pool */
    phys_entry = snapshot_pool_lookup(&g_driver_state->page_pool, pte.hash_idx);

    if (phys_entry) {
        /* Page already loaded */
        snapshot_pool_ref(phys_entry);
    } else {
        /* Load from file using pre-opened file pointer */
        page_data = kmalloc(PAGE_SIZE, GFP_ATOMIC);
        if (!page_data)
            return VM_FAULT_OOM;

        ret = read_page_data(template->pages_file, pte.hash_idx, page_data);
        if (ret) {
            pr_err("snapshot_driver: read_page_data failed: %d (hash_idx=%llu)\n",
                   ret, pte.hash_idx);
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
    ret = vmf_insert_pfn_prot(vmf->vma, vmf->address, pfn, vmf->vma->vm_page_prot);
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
             va_offset, pte.hash_idx);

    return VM_FAULT_NOPAGE;
}

/* VMA open: increment ref count and copy mappings if applicable */
static void snapshot_vma_open(struct vm_area_struct *vma)
{
    struct vma_snapshot_data *vma_data;
    struct snapshot_template *template;
    struct va_to_pa_entry *entry;
    int ret;

    vma_data = vma->vm_private_data;
    template = vma_data->template;

    atomic_inc(&template->ref_count);

    pr_debug("snapshot_driver: vma_open, ref_count=%d\n",
             atomic_read(&template->ref_count));

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
            new_entry = kzalloc(sizeof(*new_entry), GFP_ATOMIC);
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

/* mmap handler - corrected version from Step 2 */
static int snapshot_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct snapshot_template *template;
    struct vma_snapshot_data *vma_data;
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
    vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot) & ~_PAGE_RW);
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    return 0;
}