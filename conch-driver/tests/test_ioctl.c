#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <limits.h>
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
    printf("  -h            Show this help\n");
}

/* Convert relative path to absolute path */
static char *make_absolute_path(const char *path)
{
    char cwd[PATH_MAX];
    char abs_path[PATH_MAX];

    if (path[0] == '/') {
        /* Already absolute */
        return strdup(path);
    }

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        return NULL;
    }

    snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, path);
    return strdup(abs_path);
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
    char *abs_pt_path = NULL;
    char *abs_pages_path = NULL;
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
        /* Convert paths to absolute */
        abs_pt_path = make_absolute_path(page_table_path);
        abs_pages_path = make_absolute_path(pages_path);

        if (!abs_pt_path || !abs_pages_path) {
            fprintf(stderr, "Failed to convert paths to absolute\n");
            close(fd);
            return 1;
        }

        /* Verify files exist */
        if (access(abs_pt_path, R_OK) != 0) {
            perror("page_table_path not accessible");
            fprintf(stderr, "Path: %s\n", abs_pt_path);
            fprintf(stderr, "Generate test data: tests/generate_test_snapshot.sh\n");
            free(abs_pt_path);
            free(abs_pages_path);
            close(fd);
            return 1;
        }

        if (access(abs_pages_path, R_OK) != 0) {
            perror("pages_path not accessible");
            fprintf(stderr, "Path: %s\n", abs_pages_path);
            free(abs_pt_path);
            free(abs_pages_path);
            close(fd);
            return 1;
        }

        /* Create template */
        memset(&create_args, 0, sizeof(create_args));
        strncpy(create_args.template_id, template_id, TEMPLATE_ID_MAX_LEN - 1);
        create_args.total_size = total_size_mb * 1024 * 1024;
        strncpy(create_args.page_table_path, abs_pt_path, PATH_MAX_LEN - 1);
        strncpy(create_args.pages_path, abs_pages_path, PATH_MAX_LEN - 1);

        printf("Creating template '%s' (size=%lluMB)\n", template_id, total_size_mb);
        printf("  page_table: %s\n", abs_pt_path);
        printf("  pages:      %s\n", abs_pages_path);

        ret = ioctl(fd, IOCTL_CREATE_TEMPLATE, &create_args);
        if (ret < 0) {
            perror("ioctl create");
            free(abs_pt_path);
            free(abs_pages_path);
            close(fd);
            return 1;
        }

        printf("Created template: /dev/snapshot_%s\n", template_id);
        free(abs_pt_path);
        free(abs_pages_path);
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