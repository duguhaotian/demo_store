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