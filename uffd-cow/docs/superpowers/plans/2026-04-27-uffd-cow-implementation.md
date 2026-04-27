# UFFD + shmem + COW Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a multi-process shared memory demo with UFFD-based page fault handling and copy-on-write (COW) functionality.

**Architecture:** Creator process creates two memfd regions (meta for shared state, data for content) and a test file. Worker processes join via `/proc/<pid>/fd/<fd>` paths. Each process has independent UFFD handler thread. Missing faults load pages from test file; WP faults trigger COW via mmap(MAP_FIXED).

**Tech Stack:** C, Linux userfaultfd API, memfd_create, pthread, mmap

---

## File Structure

| File | Responsibility |
|------|----------------|
| `main.c` | Entry point, CLI parsing, creator/worker orchestration |
| `shmem.c` | memfd creation, mmap setup, path generation |
| `uffd_handler.c` | UFFD thread, MISSING/WP fault handling, COW logic |
| `page_meta.c` | Shared and local page state management |
| `test_data.c` | Test file creation and page data reading |
| `Makefile` | Build configuration |

---

## Task 1: Project Setup and Makefile

**Files:**
- Create: `Makefile`

- [ ] **Step 1: Create Makefile**

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -g -O0 -D_GNU_SOURCE
LDFLAGS = -pthread

TARGET = uffd-cow
SRCS = main.c shmem.c uffd_handler.c page_meta.c test_data.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
```

- [ ] **Step 2: Verify Makefile syntax**

Run: `make -n`
Expected: Print build commands without executing

- [ ] **Step 3: Commit Makefile**

```bash
git add Makefile
git commit -m "chore: add Makefile for UFFD demo build"
```

---

## Task 2: Common Header Definitions

**Files:**
- Create: `common.h`

- [ ] **Step 1: Create common.h with constants and data structures**

```c
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
```

- [ ] **Step 2: Commit common.h**

```bash
git add common.h
git commit -m "feat: add common header with data structures"
```

---

## Task 3: Test Data Module

**Files:**
- Create: `test_data.c`
- Create: `test_data.h`

- [ ] **Step 1: Create test_data.h**

```c
#ifndef UFFD_COW_TEST_DATA_H
#define UFFD_COW_TEST_DATA_H

#include "common.h"

// Create test file with PAGE_COUNT pages of distinct patterns
int create_test_file(const char *path);

// Read a page from test file at given offset
int read_page_from_file(const char *path, uint64_t offset,
                        void *buffer, size_t size);

#endif // UFFD_COW_TEST_DATA_H
```

- [ ] **Step 2: Create test_data.c**

```c
#include "test_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int create_test_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open test file");
        return -1;
    }

    // Each page has a distinct byte pattern
    const char patterns[PAGE_COUNT] = { 'A', 'B', 'C', 'D' };

    char page_buf[PAGE_SIZE];
    for (int i = 0; i < PAGE_COUNT; i++) {
        memset(page_buf, patterns[i], PAGE_SIZE);
        if (write(fd, page_buf, PAGE_SIZE) != PAGE_SIZE) {
            perror("write test file");
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

int read_page_from_file(const char *path, uint64_t offset,
                        void *buffer, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open test file for read");
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) < 0) {
        perror("lseek test file");
        close(fd);
        return -1;
    }

    if (read(fd, buffer, size) != (ssize_t)size) {
        perror("read test file");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}
```

- [ ] **Step 3: Commit test_data module**

```bash
git add test_data.h test_data.c
git commit -m "feat: add test file creation and reading module"
```

---

## Task 4: Page Metadata Module

**Files:**
- Create: `page_meta.c`
- Create: `page_meta.h`

- [ ] **Step 1: Create page_meta.h**

```c
#ifndef UFFD_COW_PAGE_META_H
#define UFFD_COW_PAGE_META_H

#include "common.h"

// Initialize shared_pages in meta_region (creator only)
void init_shared_pages(struct shared_page_meta *pages, size_t count);

// Initialize local_pages (each process)
int init_local_pages(struct local_page_state **pages, size_t count);

// Free local_pages
void free_local_pages(struct local_page_state *pages);

// Get page index from fault address
int get_page_index(void *data_base, uint64_t fault_addr);

#endif // UFFD_COW_PAGE_META_H
```

- [ ] **Step 2: Create page_meta.c**

```c
#include "page_meta.h"
#include <stdlib.h>

void init_shared_pages(struct shared_page_meta *pages, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pages[i].file_offset = i * PAGE_SIZE;
        pages[i].state = PAGE_UNLOADED;
    }
}

int init_local_pages(struct local_page_state **pages, size_t count) {
    *pages = calloc(count, sizeof(struct local_page_state));
    if (*pages == NULL) {
        perror("calloc local_pages");
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        (*pages)[i].is_private = false;
        (*pages)[i].private_addr = NULL;
    }
    return 0;
}

void free_local_pages(struct local_page_state *pages) {
    free(pages);
}

int get_page_index(void *data_base, uint64_t fault_addr) {
    return (fault_addr - (uint64_t)data_base) / PAGE_SIZE;
}
```

- [ ] **Step 3: Commit page_meta module**

```bash
git add page_meta.h page_meta.c
git commit -m "feat: add page metadata management module"
```

---

## Task 5: Shared Memory Module

**Files:**
- Create: `shmem.c`
- Create: `shmem.h`

- [ ] **Step 1: Create shmem.h**

```c
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
```

- [ ] **Step 2: Create shmem.c**

```c
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
```

- [ ] **Step 3: Commit shmem module**

```bash
git add shmem.h shmem.c
git commit -m "feat: add shared memory memfd creation and mmap module"
```

---

## Task 6: UFFD Handler Module

**Files:**
- Create: `uffd_handler.c`
- Create: `uffd_handler.h`

- [ ] **Step 1: Create uffd_handler.h**

```c
#ifndef UFFD_COW_UFFD_HANDLER_H
#define UFFD_COW_UFFD_HANDLER_H

#include "common.h"
#include <pthread.h>

// Initialize UFFD with capability checks
int init_uffd(struct config *cfg);

// Register region with MISSING | WP modes
int register_uffd(struct config *cfg);

// Enable write protection on data region
int enable_writeprotect(struct config *cfg);

// Start handler thread
int start_uffd_thread(struct config *cfg, pthread_t *thread);

// Stop handler thread
void stop_uffd_thread(struct config *cfg, pthread_t *thread);

// Cleanup UFFD
void cleanup_uffd(struct config *cfg);

#endif // UFFD_COW_UFFD_HANDLER_H
```

- [ ] **Step 2: Create uffd_handler.c (part 1: initialization)**

```c
#include "uffd_handler.h"
#include "page_meta.h"
#include "test_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/userfaultfd.h>
#include <pthread.h>

int init_uffd(struct config *cfg) {
    // Create UFFD fd
    cfg->uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (cfg->uffd < 0) {
        perror("userfaultfd syscall");
        return -1;
    }

    // API negotiation - request WP feature
    struct uffdio_api api = {
        .api = UFFD_API,
        .features = UFFD_FEATURE_PAGEFAULT_FLAG_WP
    };
    if (ioctl(cfg->uffd, UFFDIO_API, &api) < 0) {
        perror("UFFDIO_API");
        close(cfg->uffd);
        cfg->uffd = -1;
        return -1;
    }

    // Check WP feature support
    if (!(api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
        fprintf(stderr, "Kernel does not support UFFD write-protect (need Linux 5.7+)\n");
        close(cfg->uffd);
        cfg->uffd = -1;
        return -1;
    }

    return 0;
}

int register_uffd(struct config *cfg) {
    struct uffdio_register reg = {
        .range = {
            .start = (uint64_t)cfg->data_base,
            .len = cfg->region_size
        },
        .mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP
    };

    if (ioctl(cfg->uffd, UFFDIO_REGISTER, &reg) < 0) {
        perror("UFFDIO_REGISTER");
        return -1;
    }

    return 0;
}

int enable_writeprotect(struct config *cfg) {
    struct uffdio_writeprotect wp = {
        .range = {
            .start = (uint64_t)cfg->data_base,
            .len = cfg->region_size
        },
        .mode = UFFDIO_WRITEPROTECT_MODE_WP
    };

    if (ioctl(cfg->uffd, UFFDIO_WRITEPROTECT, &wp) < 0) {
        perror("UFFDIO_WRITEPROTECT enable");
        return -1;
    }

    return 0;
}

void cleanup_uffd(struct config *cfg) {
    if (cfg->uffd >= 0) {
        close(cfg->uffd);
        cfg->uffd = -1;
    }
}
```

- [ ] **Step 3: Add handler thread function to uffd_handler.c**

```c
static void *uffd_handler_thread(void *arg) {
    struct config *cfg = arg;

    while (cfg->running) {
        struct uffd_msg event;
        int n = read(cfg->uffd, &event, sizeof(event));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            perror("read uffd");
            break;
        }

        if (event.type != UFFD_MSG_PAGEFAULT) {
            continue;
        }

        uint64_t fault_addr = event.arg.pagefault.address;
        uint64_t flags = event.arg.pagefault.flags;
        int page_idx = get_page_index(cfg->data_base, fault_addr);
        uint64_t page_addr = (uint64_t)cfg->data_base + page_idx * PAGE_SIZE;

        printf("[UFFD Handler] Fault at page %d, addr=0x%lx, flags=0x%lx\n",
               page_idx, fault_addr, flags);

        // MISSING fault (page not loaded)
        if (!(flags & UFFD_PAGEFAULT_FLAG_WP)) {
            if (cfg->shared_pages[page_idx].state == PAGE_UNLOADED) {
                printf("[UFFD Handler] Loading page %d from file\n", page_idx);

                char buffer[PAGE_SIZE];
                if (read_page_from_file(cfg->test_file_path,
                                        cfg->shared_pages[page_idx].file_offset,
                                        buffer, PAGE_SIZE) < 0) {
                    fprintf(stderr, "Failed to read page from file\n");
                    continue;
                }

                struct uffdio_copy copy = {
                    .dst = page_addr,
                    .src = (uint64_t)buffer,
                    .len = PAGE_SIZE,
                    .mode = UFFDIO_COPY_MODE_WP  // Keep write-protect after copy
                };

                if (ioctl(cfg->uffd, UFFDIO_COPY, &copy) < 0) {
                    perror("UFFDIO_COPY");
                    continue;
                }

                cfg->shared_pages[page_idx].state = PAGE_LOADED;
                printf("[UFFD Handler] Page %d loaded\n", page_idx);
            } else {
                printf("[UFFD Handler] Page %d already loaded\n", page_idx);
            }
        }

        // WP fault (write-protect triggered)
        if (flags & UFFD_PAGEFAULT_FLAG_WP) {
            printf("[UFFD Handler] WP fault on page %d, performing COW\n", page_idx);

            if (cfg->local_pages[page_idx].is_private) {
                // Already private, just unprotect
                printf("[UFFD Handler] Page %d already private, unprotecting\n", page_idx);
                struct uffdio_writeprotect wp = {
                    .range = { .start = page_addr, .len = PAGE_SIZE },
                    .mode = 0  // Unprotect
                };
                ioctl(cfg->uffd, UFFDIO_WRITEPROTECT, &wp);
            } else {
                // COW: mmap private page at fault address
                void *private_page = mmap((void *)page_addr, PAGE_SIZE,
                                          PROT_READ | PROT_WRITE,
                                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                                          -1, 0);
                if (private_page == MAP_FAILED) {
                    perror("COW mmap");
                    continue;
                }

                // Copy content from shared page (if loaded)
                if (cfg->shared_pages[page_idx].state == PAGE_LOADED) {
                    memcpy(private_page, (void *)page_addr, PAGE_SIZE);
                }

                cfg->local_pages[page_idx].is_private = true;
                cfg->local_pages[page_idx].private_addr = private_page;
                printf("[UFFD Handler] COW complete for page %d\n", page_idx);
            }
        }
    }

    return NULL;
}
```

- [ ] **Step 4: Add thread start/stop functions to uffd_handler.c**

```c
int start_uffd_thread(struct config *cfg, pthread_t *thread) {
    cfg->running = true;
    int ret = pthread_create(thread, NULL, uffd_handler_thread, cfg);
    if (ret != 0) {
        perror("pthread_create");
        cfg->running = false;
        return -1;
    }
    return 0;
}

void stop_uffd_thread(struct config *cfg, pthread_t *thread) {
    cfg->running = false;
    pthread_join(*thread, NULL);
}
```

- [ ] **Step 5: Commit UFFD handler module**

```bash
git add uffd_handler.h uffd_handler.c
git commit -m "feat: add UFFD handler with MISSING/WP fault handling and COW"
```

---

## Task 7: Main Program - CLI and Orchestration

**Files:**
- Create: `main.c`

- [ ] **Step 1: Create main.c skeleton**

```c
#include "common.h"
#include "shmem.h"
#include "uffd_handler.h"
#include "page_meta.h"
#include "test_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s --create\n", prog);
    fprintf(stderr, "  %s --join --meta <path> --data <path>\n", prog);
}

static int parse_args(int argc, char **argv, struct config *cfg) {
    if (argc < 2) {
        usage(argv[0]);
        return -1;
    }

    if (strcmp(argv[1], "--create") == 0) {
        cfg->is_creator = true;
        cfg->meta_path = NULL;
        cfg->data_path = NULL;
        return 0;
    }

    if (strcmp(argv[1], "--join") == 0) {
        cfg->is_creator = false;
        if (argc < 6) {
            usage(argv[0]);
            return -1;
        }
        // Parse --meta and --data
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
                cfg->meta_path = argv[i + 1];
                i++;
            } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
                cfg->data_path = argv[i + 1];
                i++;
            }
        }
        if (!cfg->meta_path || !cfg->data_path) {
            usage(argv[0]);
            return -1;
        }
        return 0;
    }

    usage(argv[0]);
    return -1;
}

static void init_config(struct config *cfg) {
    cfg->region_size = REGION_SIZE;
    cfg->page_count = PAGE_COUNT;
    cfg->meta_fd = -1;
    cfg->data_fd = -1;
    cfg->meta_base = NULL;
    cfg->data_base = NULL;
    cfg->uffd = -1;
    cfg->shared_pages = NULL;
    cfg->local_pages = NULL;
    cfg->test_file_path = TEST_FILE_PATH;
    cfg->running = false;
}
```

- [ ] **Step 2: Add creator main flow to main.c**

```c
static int run_creator(struct config *cfg) {
    printf("[Creator] Starting...\n");

    // Create memfds
    if (create_memfds(cfg) < 0) {
        fprintf(stderr, "Failed to create memfds\n");
        return -1;
    }

    // Create test file
    if (create_test_file(cfg->test_file_path) < 0) {
        fprintf(stderr, "Failed to create test file\n");
        cleanup_shmem(cfg);
        return -1;
    }

    // Mmap regions
    if (mmap_regions(cfg) < 0) {
        fprintf(stderr, "Failed to mmap regions\n");
        cleanup_shmem(cfg);
        unlink(cfg->test_file_path);
        return -1;
    }

    // Initialize shared pages
    init_shared_pages(cfg->shared_pages, cfg->page_count);

    // Initialize local pages
    if (init_local_pages(&cfg->local_pages, cfg->page_count) < 0) {
        cleanup_shmem(cfg);
        unlink(cfg->test_file_path);
        return -1;
    }

    // Initialize UFFD
    if (init_uffd(cfg) < 0) {
        fprintf(stderr, "Failed to init UFFD\n");
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        unlink(cfg->test_file_path);
        return -1;
    }

    // Register UFFD
    if (register_uffd(cfg) < 0) {
        fprintf(stderr, "Failed to register UFFD\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        unlink(cfg->test_file_path);
        return -1;
    }

    // Enable write protection
    if (enable_writeprotect(cfg) < 0) {
        fprintf(stderr, "Failed to enable write protection\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        unlink(cfg->test_file_path);
        return -1;
    }

    // Start handler thread
    pthread_t thread;
    if (start_uffd_thread(cfg, &thread) < 0) {
        fprintf(stderr, "Failed to start UFFD thread\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        unlink(cfg->test_file_path);
        return -1;
    }

    // Print paths for workers
    print_fd_paths(cfg);

    // Wait for user input
    getchar();

    // Cleanup
    stop_uffd_thread(cfg, &thread);
    cleanup_uffd(cfg);
    free_local_pages(cfg->local_pages);
    cleanup_shmem(cfg);
    unlink(cfg->test_file_path);

    printf("[Creator] Exiting\n");
    return 0;
}
```

- [ ] **Step 3: Add worker main flow and demo operations to main.c**

```c
static void demo_operations(struct config *cfg) {
    printf("[Worker] Starting demo operations...\n");

    // Read page 0 (triggers MISSING)
    char *page0 = (char *)cfg->data_base;
    printf("[Worker] Reading page 0...\n");
    char first_char = page0[0];
    printf("[Worker] Read page 0: '%c%c%c%c...' (size=%d)\n",
           first_char, first_char, first_char, first_char, PAGE_SIZE);

    // Read page 1 (triggers MISSING)
    char *page1 = (char *)cfg->data_base + PAGE_SIZE;
    printf("[Worker] Reading page 1...\n");
    char second_char = page1[0];
    printf("[Worker] Read page 1: '%c%c%c%c...' (size=%d)\n",
           second_char, second_char, second_char, second_char, PAGE_SIZE);

    // Write to page 0 (triggers WP -> COW)
    printf("[Worker] Writing to page 0 (changing to 'XXXX')...\n");
    memset(page0, 'X', PAGE_SIZE);
    printf("[Worker] Write page 0 complete\n");

    // Read page 0 again (should see modified value in private page)
    printf("[Worker] Reading page 0 after write...\n");
    first_char = page0[0];
    printf("[Worker] Read page 0 after write: '%c%c%c%c...' (private COW page)\n",
           first_char, first_char, first_char, first_char);

    printf("[Worker] Demo complete\n");
}

static int run_worker(struct config *cfg) {
    printf("[Worker] Starting...\n");
    printf("[Worker] meta_path: %s\n", cfg->meta_path);
    printf("[Worker] data_path: %s\n", cfg->data_path);

    // Open existing memfds
    if (open_memfds(cfg) < 0) {
        fprintf(stderr, "Failed to open memfds\n");
        return -1;
    }

    // Mmap regions
    if (mmap_regions(cfg) < 0) {
        fprintf(stderr, "Failed to mmap regions\n");
        cleanup_shmem(cfg);
        return -1;
    }

    // Initialize local pages (shared pages already initialized by creator)
    if (init_local_pages(&cfg->local_pages, cfg->page_count) < 0) {
        cleanup_shmem(cfg);
        return -1;
    }

    // Initialize UFFD
    if (init_uffd(cfg) < 0) {
        fprintf(stderr, "Failed to init UFFD\n");
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }

    // Register UFFD
    if (register_uffd(cfg) < 0) {
        fprintf(stderr, "Failed to register UFFD\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }

    // Enable write protection
    if (enable_writeprotect(cfg) < 0) {
        fprintf(stderr, "Failed to enable write protection\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }

    // Start handler thread
    pthread_t thread;
    if (start_uffd_thread(cfg, &thread) < 0) {
        fprintf(stderr, "Failed to start UFFD thread\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }

    // Run demo operations
    demo_operations(cfg);

    // Cleanup
    stop_uffd_thread(cfg, &thread);
    cleanup_uffd(cfg);
    free_local_pages(cfg->local_pages);
    cleanup_shmem(cfg);

    printf("[Worker] Exiting\n");
    return 0;
}
```

- [ ] **Step 4: Add main function to main.c**

```c
int main(int argc, char **argv) {
    struct config cfg;
    init_config(&cfg);

    if (parse_args(argc, argv, &cfg) < 0) {
        return 1;
    }

    int ret;
    if (cfg.is_creator) {
        ret = run_creator(&cfg);
    } else {
        ret = run_worker(&cfg);
    }

    return ret < 0 ? 1 : 0;
}
```

- [ ] **Step 5: Commit main program**

```bash
git add main.c
git commit -m "feat: add main program with CLI parsing and creator/worker flows"
```

---

## Task 8: Build and Verify

**Files:**
- Modify: All source files (verify compilation)

- [ ] **Step 1: Build the project**

Run: `make clean && make`
Expected: Compile all files, produce `uffd-cow` executable

- [ ] **Step 2: Verify binary exists**

Run: `ls -la uffd-cow`
Expected: Binary exists, ~50-100KB

- [ ] **Step 3: Check kernel version for UFFD WP support**

Run: `uname -r`
Expected: Version >= 5.7 (e.g., 5.15, 6.x)

- [ ] **Step 4: Commit if any fixes needed**

If build required fixes:
```bash
git add -u
git commit -m "fix: resolve build issues"
```

---

## Task 9: Functional Test - Creator Only

**Files:**
- None (runtime test)

- [ ] **Step 1: Run creator process**

Run: `./uffd-cow --create`
Expected: Print memfd paths and wait for input

- [ ] **Step 2: Verify output format**

Expected output:
```
[Creator] Starting...
Created shmem:
  meta: /proc/self/fd/3
  data: /proc/self/fd/4
Test file: /tmp/uffd-cow-test.bin
Waiting for workers... Press Enter to exit
```

- [ ] **Step 3: Verify test file created**

Run: `ls -la /tmp/uffd-cow-test.bin`
Expected: File exists, size 16384 bytes (16KB)

- [ ] **Step 4: Exit creator**

Press Enter in creator terminal
Expected: Clean exit with "[Creator] Exiting"

---

## Task 10: Functional Test - Multi-Process Demo

**Files:**
- None (runtime test)

- [ ] **Step 1: Start creator in terminal 1**

Run: `./uffd-cow --create`
Expected: Print paths, note the fd numbers

- [ ] **Step 2: Get creator PID**

Run: `ps aux | grep uffd-cow | grep --creator`
Or: Creator process prints its PID via printf
Note: If needed, add PID printing to creator output.

- [ ] **Step 3: Start worker 1 in terminal 2**

Run: `./uffd-cow --join --meta /proc/<pid>/fd/<meta_fd> --data /proc/<pid>/fd/<data_fd>`
Expected: Worker starts, triggers faults, prints demo output

- [ ] **Step 4: Verify worker 1 output**

Expected output:
```
[Worker] Starting...
[Worker] Reading page 0...
[UFFD Handler] Fault at page 0...
[UFFD Handler] Loading page 0 from file...
[Worker] Read page 0: 'AAAA...' (size=4096)
[Worker] Reading page 1...
[UFFD Handler] Fault at page 1...
[Worker] Read page 1: 'BBBB...' (size=4096)
[Worker] Writing to page 0 (changing to 'XXXX')...
[UFFD Handler] WP fault on page 0, performing COW...
[Worker] Write page 0 complete
[Worker] Read page 0 after write: 'XXXX...' (private COW page)
[Worker] Demo complete
```

- [ ] **Step 5: Start worker 2 in terminal 3**

Run: `./uffd-cow --join --meta /proc/<pid>/fd/<meta_fd> --data /proc/<pid>/fd/<data_fd>`
Expected: Worker 2 reads page 0 and sees 'AAAA' (original shared value, not Worker 1's 'XXXX')

- [ ] **Step 6: Verify COW isolation**

Worker 2 output should show:
```
[Worker] Read page 0: 'AAAA...' (original shared page)
```

This proves COW works: Worker 1's private modification doesn't affect shared page.

- [ ] **Step 7: Exit creator**

Press Enter in creator terminal

---

## Task 11: Final Cleanup and Documentation

**Files:**
- Modify: `Makefile` (add .PHONY, clean improvements)
- Update: `design.md` reference if needed

- [ ] **Step 1: Verify all commits**

Run: `git log --oneline -15`
Expected: All task commits visible

- [ ] **Step 2: Final status check**

Run: `git status`
Expected: Clean working tree

- [ ] **Step 3: Tag working version**

```bash
git tag v1.0-uffd-cow-demo
git push origin --tags
```

- [ ] **Step 4: Summary commit if needed**

```bash
git add -A
git commit -m "chore: final cleanup for UFFD COW demo"
```

---

## Self-Review Checklist

**1. Spec coverage:**
- [x] Dual memfd regions (meta + data) → Task 5
- [x] Runtime capability checks → Task 6
- [x] MISSING fault handling → Task 6
- [x] WP fault handling with COW → Task 6
- [x] CLI with --create and --join → Task 7
- [x] Test file with distinct patterns → Task 3
- [x] Shared state (UNLOADED/LOADED) + local state (PRIVATE) → Task 4
- [x] Multi-process demo verification → Task 10

**2. Placeholder scan:**
- [x] No TBD/TODO found
- [x] All code blocks contain actual implementation
- [x] All commands specify expected output

**3. Type consistency:**
- [x] struct config fields match across all modules
- [x] page_meta functions use consistent signatures
- [x] UFFD handler uses correct ioctl structures