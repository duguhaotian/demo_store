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

    ret = misc_register(&template->mdev);
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

    misc_deregister(&template->mdev);
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