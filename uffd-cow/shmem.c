#include "shmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

int create_memfds(struct config *cfg) {
    // Create meta memfd
    cfg->meta_fd = memfd_create("uffd-cow-meta", MFD_CLOEXEC);
    if (cfg->meta_fd < 0) {
        perror("memfd_create meta");
        return -1;
    }
    if (ftruncate(cfg->meta_fd, META_SIZE) < 0) {
        perror("ftruncate meta");
        close(cfg->meta_fd);
        return -1;
    }

    // Create data memfd
    cfg->data_fd = memfd_create("uffd-cow-data", MFD_CLOEXEC);
    if (cfg->data_fd < 0) {
        perror("memfd_create data");
        close(cfg->meta_fd);
        return -1;
    }
    if (ftruncate(cfg->data_fd, cfg->region_size) < 0) {
        perror("ftruncate data");
        close(cfg->meta_fd);
        close(cfg->data_fd);
        return -1;
    }

    return 0;
}

int open_memfds(struct config *cfg) {
    // Open meta via proc path
    cfg->meta_fd = open(cfg->meta_path, O_RDWR);
    if (cfg->meta_fd < 0) {
        perror("open meta_path");
        return -1;
    }

    // Open data via proc path
    cfg->data_fd = open(cfg->data_path, O_RDWR);
    if (cfg->data_fd < 0) {
        perror("open data_path");
        close(cfg->meta_fd);
        return -1;
    }

    return 0;
}

int mmap_regions(struct config *cfg) {
    // Mmap meta region (RW, no UFFD)
    cfg->meta_base = mmap(NULL, META_SIZE,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED, cfg->meta_fd, 0);
    if (cfg->meta_base == MAP_FAILED) {
        perror("mmap meta");
        return -1;
    }
    cfg->shared_pages = (struct shared_page_meta *)cfg->meta_base;

    // Mmap data region (RW, will enable WP via UFFD)
    cfg->data_base = mmap(NULL, cfg->region_size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED, cfg->data_fd, 0);
    if (cfg->data_base == MAP_FAILED) {
        perror("mmap data");
        munmap(cfg->meta_base, META_SIZE);
        return -1;
    }

    return 0;
}

void print_fd_paths(struct config *cfg) {
    printf("Created shmem:\n");
    printf("  meta: /proc/self/fd/%d\n", cfg->meta_fd);
    printf("  data: /proc/self/fd/%d\n", cfg->data_fd);
    printf("Test file: %s\n", cfg->test_file_path);
    printf("Waiting for workers... Press Enter to exit\n");
}

void cleanup_shmem(struct config *cfg) {
    if (cfg->data_base && cfg->data_base != MAP_FAILED) {
        munmap(cfg->data_base, cfg->region_size);
    }
    if (cfg->meta_base && cfg->meta_base != MAP_FAILED) {
        munmap(cfg->meta_base, META_SIZE);
    }
    if (cfg->data_fd >= 0) {
        close(cfg->data_fd);
    }
    if (cfg->meta_fd >= 0) {
        close(cfg->meta_fd);
    }
}