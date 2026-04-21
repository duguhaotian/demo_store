#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "snapshot_types.h"

int main(int argc, char *argv[])
{
    int fd;
    struct ioctl_create_template create_args;
    struct ioctl_template_status status_args;
    struct ioctl_delete_template delete_args;
    int ret;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <control_device>\n", argv[0]);
        return 1;
    }

    /* Open control device */
    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open control device");
        return 1;
    }

    /* Create template */
    memset(&create_args, 0, sizeof(create_args));
    strcpy(create_args.template_id, "test_001");
    create_args.total_size = 4 * 1024 * 1024;  /* 4MB */
    strcpy(create_args.page_table_path, "test_snapshot/page_table.bin");
    strcpy(create_args.pages_path, "test_snapshot/pages.bin");

    ret = ioctl(fd, IOCTL_CREATE_TEMPLATE, &create_args);
    if (ret < 0) {
        perror("ioctl create");
        close(fd);
        return 1;
    }

    printf("Created template: /dev/snapshot_test_001\n");

    /* Get status */
    memset(&status_args, 0, sizeof(status_args));
    strcpy(status_args.template_id, "test_001");

    ret = ioctl(fd, IOCTL_TEMPLATE_STATUS, &status_args);
    if (ret < 0) {
        perror("ioctl status");
    } else {
        printf("Status: mmap_count=%llu, loaded_pages=%llu\n",
               status_args.mmap_count, status_args.loaded_pages);
    }

    /* Delete template */
    memset(&delete_args, 0, sizeof(delete_args));
    strcpy(delete_args.template_id, "test_001");

    ret = ioctl(fd, IOCTL_DELETE_TEMPLATE, &delete_args);
    if (ret < 0) {
        perror("ioctl delete");
    } else {
        printf("Deleted template\n");
    }

    close(fd);
    return 0;
}