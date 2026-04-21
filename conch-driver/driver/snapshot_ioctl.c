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