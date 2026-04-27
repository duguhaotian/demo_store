#ifndef UFFD_COW_COMMON_H
#define UFFD_COW_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Constants
#define PAGE_SIZE       4096
#define PAGE_COUNT      4
#define REGION_SIZE     (PAGE_SIZE * PAGE_COUNT)
#define META_SIZE       PAGE_SIZE
#define TEST_FILE_PATH  "/tmp/uffd-cow-test.bin"

// Shared page state (stored in shared meta_region)
enum shared_page_state {
    PAGE_UNLOADED = 0,  // Not loaded from file
    PAGE_LOADED   = 1,  // Loaded in shared shmem
};

// Shared page metadata (in meta_region, visible to all processes)
struct shared_page_meta {
    uint64_t file_offset;           // File offset for this page
    enum shared_page_state state;   // Shared state only
};

// Local page state (per-process, malloc'd)
struct local_page_state {
    bool is_private;    // True if this process has COW'd this page
    void *private_addr; // Address of private page (if is_private)
};

// Global configuration
struct config {
    size_t region_size;
    size_t page_count;

    // Paths (worker gets from CLI)
    char *meta_path;
    char *data_path;

    // File descriptors
    int meta_fd;
    int data_fd;

    // Mmap addresses
    void *meta_base;
    void *data_base;

    // UFFD
    int uffd;

    // Page metadata
    struct shared_page_meta *shared_pages;  // Points to meta_region
    struct local_page_state *local_pages;   // Process-local malloc

    // Test file
    char *test_file_path;

    // Runtime state
    bool is_creator;
    volatile bool running;
};

#endif // UFFD_COW_COMMON_H