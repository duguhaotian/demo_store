#ifndef UFFD_COW_SHMEM_H
#define UFFD_COW_SHMEM_H

#include "common.h"

// Create memfd regions (creator)
int create_memfds(struct config *cfg);

// Open existing memfds via proc paths (worker)
int open_memfds(struct config *cfg);

// Mmap regions
int mmap_regions(struct config *cfg);

// Print fd paths for other processes
void print_fd_paths(struct config *cfg);

// Cleanup mmap and fds
void cleanup_shmem(struct config *cfg);

#endif // UFFD_COW_SHMEM_H