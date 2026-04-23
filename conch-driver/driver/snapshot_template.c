#include "snapshot_types.h"
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>

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

/* Find template and increment ref_count (caller must call snapshot_template_unref) */
struct snapshot_template *snapshot_template_find_ref(const char *id)
{
    struct snapshot_template *template;

    spin_lock(&g_driver_state->template_lock);
    list_for_each_entry(template, &g_driver_state->template_list, list) {
        if (strncmp(template->template_id, id, TEMPLATE_ID_MAX_LEN) == 0) {
            atomic_inc(&template->ref_count);
            spin_unlock(&g_driver_state->template_lock);
            return template;
        }
    }
    spin_unlock(&g_driver_state->template_lock);

    return NULL;
}

/* Decrement template ref_count, free if zero */
void snapshot_template_unref(struct snapshot_template *template)
{
    char template_id[TEMPLATE_ID_MAX_LEN];

    /* Save template_id before potential free */
    strncpy(template_id, template->template_id, TEMPLATE_ID_MAX_LEN);

    if (atomic_dec_and_test(&template->ref_count)) {
        /* Ref count hit zero - safe to free */
        spin_lock(&g_driver_state->template_lock);
        list_del(&template->list);
        spin_unlock(&g_driver_state->template_lock);

        if (template->pages_file)
            filp_close(template->pages_file, NULL);

        misc_deregister(&template->mdev);
        kfree(template->mdev.name);
        kfree(template);

        pr_info("snapshot_driver: freed template '%s'\n", template_id);
    }
}

/* Find without ref increment (for status query only) */
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
    struct file *pages_file;
    struct file *page_table_file;
    uint64_t page_count;
    ssize_t read_ret;
    int ret;

    /* Calculate page count */
    page_count = args->total_size / PAGE_SIZE;

    pr_info("create_template: id='%s', size=%llu, page_count=%llu\n",
            args->template_id, args->total_size, page_count);
    pr_info("create_template: page_table='%s', pages='%s'\n",
            args->page_table_path, args->pages_path);

    /* Check if template ID already exists */
    if (template_id_exists(args->template_id))
        return -EEXIST;

    /* Open page_table file to cache it */
    page_table_file = filp_open(args->page_table_path, O_RDONLY, 0);
    if (IS_ERR(page_table_file)) {
        pr_err("snapshot_driver: failed to open page_table_path: %ld\n",
               PTR_ERR(page_table_file));
        return -EINVAL;
    }

    /* Open pages file for preload */
    pages_file = filp_open(args->pages_path, O_RDONLY, 0);
    if (IS_ERR(pages_file)) {
        pr_err("snapshot_driver: failed to open pages_path: %ld\n",
               PTR_ERR(pages_file));
        filp_close(page_table_file, NULL);
        return -EINVAL;
    }

    /* Allocate and cache page_table in memory */
    template = kzalloc(sizeof(*template) + page_count * sizeof(struct page_table_entry),
                       GFP_KERNEL);
    if (!template) {
        filp_close(page_table_file, NULL);
        filp_close(pages_file, NULL);
        return -ENOMEM;
    }

    /* Read entire page_table into memory */
    template->page_table_cache = (struct page_table_entry *)(template + 1);
    read_ret = kernel_read(page_table_file, template->page_table_cache,
                           page_count * sizeof(struct page_table_entry),
                           &(loff_t){0});
    filp_close(page_table_file, NULL);

    if (read_ret != page_count * sizeof(struct page_table_entry)) {
        pr_err("snapshot_driver: failed to read page_table: %zd (expected %llu)\n",
               read_ret, page_count * sizeof(struct page_table_entry));
        filp_close(pages_file, NULL);
        kfree(template);
        return -EIO;
    }

    strncpy(template->template_id, args->template_id, TEMPLATE_ID_MAX_LEN);
    template->total_size = args->total_size;
    template->page_count = page_count;
    template->pages_file = pages_file;

    atomic_set(&template->ref_count, 0);
    atomic_set(&template->preload_done, 0);
    template->first_vma_data = NULL;

    /* Setup miscdevice */
    template->mdev.minor = MISC_DYNAMIC_MINOR;
    template->mdev.name = kasprintf(GFP_KERNEL, "snapshot_%s", args->template_id);
    if (!template->mdev.name) {
        filp_close(pages_file, NULL);
        kfree(template);
        return -ENOMEM;
    }
    template->mdev.fops = &snapshot_fops;
    template->mdev.mode = 0600;

    ret = misc_register(&template->mdev);
    if (ret) {
        filp_close(pages_file, NULL);
        kfree(template->mdev.name);
        kfree(template);
        return ret;
    }

    spin_lock(&g_driver_state->template_lock);
    list_add_tail(&template->list, &g_driver_state->template_list);
    spin_unlock(&g_driver_state->template_lock);

    pr_info("create_template: SUCCESS, device='/dev/%s', page_table_entries=%llu\n",
            template->mdev.name, page_count);

    return 0;
}

int snapshot_template_delete(struct ioctl_delete_template *args)
{
    struct snapshot_template *template;

    spin_lock(&g_driver_state->template_lock);
    list_for_each_entry(template, &g_driver_state->template_list, list) {
        if (strncmp(template->template_id, args->template_id, TEMPLATE_ID_MAX_LEN) == 0) {
            /* Found - check ref_count under lock */
            if (atomic_read(&template->ref_count) > 0) {
                spin_unlock(&g_driver_state->template_lock);
                return -EBUSY;
            }

            /* Safe to delete - remove from list under lock */
            list_del(&template->list);
            spin_unlock(&g_driver_state->template_lock);

            /* Close pages file */
            if (template->pages_file)
                filp_close(template->pages_file, NULL);

            misc_deregister(&template->mdev);
            kfree(template->mdev.name);
            kfree(template);

            /* Check if any templates remain - if not, clear pool */
            spin_lock(&g_driver_state->template_lock);
            if (list_empty(&g_driver_state->template_list)) {
                spin_unlock(&g_driver_state->template_lock);
                /* No templates left - destroy pool pages */
                snapshot_pool_destroy(&g_driver_state->page_pool);
                snapshot_pool_init(&g_driver_state->page_pool);
                pr_info("snapshot_driver: pool cleared (no templates remain)\n");
            } else {
                spin_unlock(&g_driver_state->template_lock);
            }

            pr_info("snapshot_driver: deleted template '%s'\n", args->template_id);
            return 0;
        }
    }
    spin_unlock(&g_driver_state->template_lock);

    return -ENOENT;
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

/* Preload all pages from pages.bin into global pool */
int snapshot_preload_template(struct snapshot_template *template)
{
    struct page_table_entry *pte;
    void *page_data;
    struct phys_page_entry *phys_entry;
    uint64_t i;
    int ret;
    int failed_pages = 0;
    loff_t offset;
    ssize_t read_ret;
    uint64_t file_index;
    uint64_t slot_hash;
    uint32_t probe;
    char header[8];

    pr_info("preload: template='%s', page_count=%llu\n",
            template->template_id, template->page_count);

    page_data = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!page_data)
        return -ENOMEM;

    /* Load each unique page */
    for (i = 0; i < template->page_count; i++) {
        pte = &template->page_table_cache[i];

        pr_debug("preload[%llu]: hash_idx=%llu\n", i, pte->hash_idx);

        /* Skip if already in pool (dedup) */
        phys_entry = snapshot_pool_lookup_noref(&g_driver_state->page_pool, pte->hash_idx);
        if (phys_entry) {
            pr_debug("preload[%llu]: hash_idx=%llu already in pool\n", i, pte->hash_idx);
            continue;
        }

        /* Open addressing lookup in pages.bin */
        file_index = pte->hash_idx % g_driver_state->hash_modulus;

        for (probe = 0; probe < g_driver_state->max_probe_count; probe++) {
            offset = file_index * SLOT_SIZE;

            /* Read header (8 bytes) */
            read_ret = kernel_read(template->pages_file, header, 8, &offset);
            if (read_ret != 8) {
                pr_err("preload[%llu]: failed read header at offset %lld\n", i, offset);
                failed_pages++;
                break;
            }

            slot_hash = *((uint64_t *)header);

            if (slot_hash == pte->hash_idx) {
                /* Found - read page data */
                offset = file_index * SLOT_SIZE + 8;
                read_ret = kernel_read(template->pages_file, page_data, PAGE_SIZE, &offset);
                if (read_ret != PAGE_SIZE) {
                    pr_err("preload[%llu]: failed read page data\n", i);
                    failed_pages++;
                    break;
                }

                /* Add to pool */
                phys_entry = snapshot_pool_add(&g_driver_state->page_pool, pte->hash_idx, page_data);
                if (!phys_entry) {
                    pr_err("preload[%llu]: failed add to pool\n", i);
                    failed_pages++;
                }
                break;
            }

            if (slot_hash == 0) {
                /* Empty slot - page not found */
                pr_err("preload[%llu]: hash_idx=%llu NOT_FOUND (empty slot)\n", i, pte->hash_idx);
                failed_pages++;
                break;
            }

            /* Linear probing */
            file_index = (file_index + 1) % g_driver_state->hash_modulus;
        }

        if (probe >= g_driver_state->max_probe_count) {
            pr_err("preload[%llu]: max probes exceeded\n", i);
            failed_pages++;
        }
    }

    kfree(page_data);

    if (failed_pages > 0) {
        pr_err("preload: FAILED, %d pages failed\n", failed_pages);
        return -EIO;
    }

    pr_info("preload: SUCCESS, loaded %llu unique pages\n",
            g_driver_state->page_pool.total_pages);
    return 0;
}