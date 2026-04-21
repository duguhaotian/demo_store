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