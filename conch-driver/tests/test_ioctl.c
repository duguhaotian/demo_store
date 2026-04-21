#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include "snapshot_types.h"

#define DEFAULT_CONTROL_DEVICE "/dev/snapshot_control"

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s                           # Create template 'test_001' (default)\n", prog);
    printf("  %s -c <id> -t <size> -p <pt> -g <pages>  # Create custom template\n", prog);
    printf("  %s -s <id>                   # Query template status\n", prog);
    printf("  %s -d <id>                   # Delete template\n", prog);
    printf("\nOptions:\n");
    printf("  -c <id>       Template ID (default: test_001)\n");
    printf("  -t <size>     Total size in MB (default: 4)\n");
    printf("  -p <path>     Page table file path (default: test_snapshot/page_table.bin)\n");
    printf("  -g <path>     Pages file path (default: test_snapshot/pages.bin)\n");
    printf("  -s <id>       Query template status\n");
    printf("  -d <id>       Delete template\n");
    printf("  -f <device>   Control device path (default: %s)\n", DEFAULT_CONTROL_DEVICE);
}

int main(int argc, char *argv[])
{
    int fd;
    struct ioctl_create_template create_args;
    struct ioctl_template_status status_args;
    struct ioctl_delete_template delete_args;
    int ret;
    int opt;
    char *control_device = DEFAULT_CONTROL_DEVICE;
    char *template_id = "test_001";
    char *page_table_path = "test_snapshot/page_table.bin";
    char *pages_path = "test_snapshot/pages.bin";
    uint64_t total_size_mb = 4;
    int do_create = 1;  /* Default: create */
    int do_status = 0;
    int do_delete = 0;

    /* Parse options */
    while ((opt = getopt(argc, argv, "c:t:p:g:s:d:f:h")) != -1) {
        switch (opt) {
        case 'c':
            template_id = optarg;
            break;
        case 't':
            total_size_mb = atoi(optarg);
            break;
        case 'p':
            page_table_path = optarg;
            break;
        case 'g':
            pages_path = optarg;
            break;
        case 's':
            template_id = optarg;
            do_create = 0;
            do_status = 1;
            break;
        case 'd':
            template_id = optarg;
            do_create = 0;
            do_delete = 1;
            break;
        case 'f':
            control_device = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Open control device */
    fd = open(control_device, O_RDWR);
    if (fd < 0) {
        perror("open control device");
        fprintf(stderr, "Make sure driver is loaded: insmod driver/snapshot_driver.ko\n");
        return 1;
    }

    if (do_create) {
        /* Create template */
        memset(&create_args, 0, sizeof(create_args));
        strncpy(create_args.template_id, template_id, TEMPLATE_ID_MAX_LEN - 1);
        create_args.total_size = total_size_mb * 1024 * 1024;
        strncpy(create_args.page_table_path, page_table_path, PATH_MAX_LEN - 1);
        strncpy(create_args.pages_path, pages_path, PATH_MAX_LEN - 1);

        printf("Creating template '%s' (size=%lluMB, pt=%s, pages=%s)\n",
               template_id, total_size_mb, page_table_path, pages_path);

        ret = ioctl(fd, IOCTL_CREATE_TEMPLATE, &create_args);
        if (ret < 0) {
            perror("ioctl create");
            close(fd);
            return 1;
        }

        printf("Created template: /dev/snapshot_%s\n", template_id);
    }

    if (do_status) {
        /* Get status */
        memset(&status_args, 0, sizeof(status_args));
        strncpy(status_args.template_id, template_id, TEMPLATE_ID_MAX_LEN - 1);

        ret = ioctl(fd, IOCTL_TEMPLATE_STATUS, &status_args);
        if (ret < 0) {
            perror("ioctl status");
            close(fd);
            return 1;
        }

        printf("Template '%s': mmap_count=%llu, loaded_pages=%llu\n",
               template_id, status_args.mmap_count, status_args.loaded_pages);
    }

    if (do_delete) {
        /* Delete template */
        memset(&delete_args, 0, sizeof(delete_args));
        strncpy(delete_args.template_id, template_id, TEMPLATE_ID_MAX_LEN - 1);

        printf("Deleting template '%s'\n", template_id);
        ret = ioctl(fd, IOCTL_DELETE_TEMPLATE, &delete_args);
        if (ret < 0) {
            perror("ioctl delete");
            close(fd);
            return 1;
        }

        printf("Deleted template '%s'\n", template_id);
    }

    close(fd);
    return 0;
}