#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include "snapshot_types.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Snapshot Driver Team");
MODULE_DESCRIPTION("Memory snapshot driver for VM sharing");
MODULE_VERSION("1.0");

struct snapshot_driver_state *g_driver_state;

/* Sysfs attributes */
static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct snapshot_template *template;
    int count = 0;
    uint64_t total_pages;

    spin_lock(&g_driver_state->template_lock);
    list_for_each_entry(template, &g_driver_state->template_list, list) {
        count++;
    }
    spin_unlock(&g_driver_state->template_lock);

    total_pages = g_driver_state->page_pool.total_pages;

    return sprintf(buf, "templates: %d\nloaded_pages: %llu\nhash_modulus: %llu\n",
                   count, total_pages, g_driver_state->hash_modulus);
}

static struct kobj_attribute stats_attr = __ATTR(stats, 0444, stats_show, NULL);

/* Sysfs kobject */
static struct kobject *snapshot_kobj;

/* Control device for ioctl operations before any template exists */
static struct miscdevice control_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "snapshot_control",
    .fops = &snapshot_fops,
    .mode = 0600,
};

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

    /* Create sysfs directory and attributes */
    snapshot_kobj = kobject_create_and_add("snapshot_driver", kernel_kobj);
    if (!snapshot_kobj) {
        snapshot_pool_destroy(&g_driver_state->page_pool);
        kfree(g_driver_state);
        return -ENOMEM;
    }

    ret = sysfs_create_file(snapshot_kobj, &stats_attr.attr);
    if (ret) {
        kobject_put(snapshot_kobj);
        snapshot_pool_destroy(&g_driver_state->page_pool);
        kfree(g_driver_state);
        return ret;
    }

    /* Register control device for initial ioctl operations */
    ret = misc_register(&control_device);
    if (ret) {
        sysfs_remove_file(snapshot_kobj, &stats_attr.attr);
        kobject_put(snapshot_kobj);
        snapshot_pool_destroy(&g_driver_state->page_pool);
        kfree(g_driver_state);
        return ret;
    }

    pr_info("snapshot_driver: initialized, control device at /dev/%s\n",
            control_device.name);
    return 0;
}

static void __exit snapshot_exit(void)
{
    struct snapshot_template *template, *tmp;

    /* Deregister control device */
    misc_deregister(&control_device);

    /* Remove sysfs */
    sysfs_remove_file(snapshot_kobj, &stats_attr.attr);
    kobject_put(snapshot_kobj);

    spin_lock(&g_driver_state->template_lock);
    list_for_each_entry_safe(template, tmp,
                             &g_driver_state->template_list, list) {
        list_del(&template->list);
        /* Close pages file */
        if (template->pages_file)
            filp_close(template->pages_file, NULL);
        misc_deregister(&template->mdev);
        kfree(template->mdev.name);
        kfree(template);
    }
    spin_unlock(&g_driver_state->template_lock);

    snapshot_pool_destroy(&g_driver_state->page_pool);
    kfree(g_driver_state);

    pr_info("snapshot_driver: unloaded\n");
}

module_init(snapshot_init);
module_exit(snapshot_exit);