// ublk-mmap-demo/test/test_mmap.c
/*
 * Test program: mmap ublk block device and trigger page faults
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define UBLK_DEV_PATH "/dev/ublkb0"
#define DEFAULT_MAP_SIZE (64 * 1024 * 1024)  /* 64MB */

/* Get device size from block device */
static size_t get_dev_size(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return DEFAULT_MAP_SIZE;
    }
    return st.st_size;
}

int main(int argc, char **argv) {
    const char *dev_path = UBLK_DEV_PATH;
    size_t map_size = DEFAULT_MAP_SIZE;
    int fd;
    void *map;
    char buf[256];
    int test_offset;
    int i;

    if (argc > 1) {
        dev_path = argv[1];
    }
    if (argc > 2) {
        map_size = atol(argv[2]);
    }

    printf("=== ublk mmap test ===\n");
    printf("Device: %s\n", dev_path);
    printf("Map size: %zu bytes\n", map_size);

    /* Open ublk block device */
    fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror("open ublk device");
        fprintf(stderr, "Make sure ublksrv is running and device exists\n");
        return 1;
    }

    /* Get actual device size */
    map_size = get_dev_size(dev_path);
    if (map_size == 0) {
        map_size = DEFAULT_MAP_SIZE;
    }
    printf("Actual device size: %zu bytes\n", map_size);

    /* mmap the ublk device */
    map = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap ublk device");
        close(fd);
        return 1;
    }

    printf("mmap successful at address %p\n", map);
    printf("\nTriggering page faults by accessing different offsets...\n\n");

    /* Test 1: Access offset 0 (first sector) */
    printf("--- Test 1: Offset 0 ---\n");
    memcpy(buf, map, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    printf("Data at offset 0: \"%.50s...\"\n", buf);

    /* Test 2: Access offset 4KB (sector 8) */
    test_offset = 4096;
    printf("\n--- Test 2: Offset 4KB ---\n");
    if (test_offset < map_size) {
        memcpy(buf, map + test_offset, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        printf("Data at offset 4KB: \"%.50s...\"\n", buf);
    } else {
        printf("Offset 4KB beyond device size, skipped\n");
    }

    /* Test 3: Access offset 1MB */
    test_offset = 1024 * 1024;
    printf("\n--- Test 3: Offset 1MB ---\n");
    if (test_offset < map_size) {
        memcpy(buf, map + test_offset, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        printf("Data at offset 1MB: \"%.50s...\"\n", buf);
    } else {
        printf("Offset 1MB beyond device size, skipped\n");
    }

    /* Test 4: Access multiple sequential pages */
    printf("\n--- Test 4: Sequential page accesses ---\n");
    for (i = 0; i < 5; i++) {
        int offset = i * 4096;
        if (offset < map_size) {
            char val = *((char *)map + offset);
            printf("Page %d (offset %d): first byte = '%c' (0x%02x)\n",
                   i, offset, val >= 32 ? val : '.', val);
        }
    }

    /* Test 5: Check if sparse hole regions return zeros */
    test_offset = 32 * 1024 * 1024;  /* 32MB, likely in sparse hole */
    printf("\n--- Test 5: Sparse hole region (offset 32MB) ---\n");
    if (test_offset < map_size) {
        int zero_count = 0;
        for (i = 0; i < 512; i++) {
            if (*((char *)map + test_offset + i) == 0) {
                zero_count++;
            }
        }
        printf("Zero bytes in first 512 bytes: %d/512\n", zero_count);
        if (zero_count == 512) {
            printf("This is likely a sparse file hole (returns zeros)\n");
        }
    } else {
        printf("Offset 32MB beyond device size, skipped\n");
    }

    printf("\n=== Test complete ===\n");
    printf("Check dmesg and ublksrv logs for page fault IO handling\n");

    /* Cleanup */
    munmap(map, map_size);
    close(fd);

    return 0;
}