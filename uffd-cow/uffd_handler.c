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
#include <sys/syscall.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>

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

        if (event.event != UFFD_EVENT_PAGEFAULT) {
            continue;
        }

        uint64_t fault_addr = event.arg.pagefault.address;
        uint64_t flags = event.arg.pagefault.flags;
        int page_idx = get_page_index(cfg->data_base, fault_addr);
        uint64_t page_addr = (uint64_t)cfg->data_base + page_idx * PAGE_SIZE;

        printf("[UFFD Handler] Fault at page %d, addr=0x%lx, flags=0x%lx\n",
               page_idx, fault_addr, flags);
        fflush(stdout);

        // MISSING fault (page not loaded)
        if (!(flags & UFFD_PAGEFAULT_FLAG_WP)) {
            printf("[UFFD Handler] MISSING fault on page %d\n", page_idx);

            // Always load from file and copy (to wake the fault thread)
            char buffer[PAGE_SIZE];
            if (read_page_from_file(cfg->test_file_path,
                                    page_idx * PAGE_SIZE,
                                    buffer, PAGE_SIZE) < 0) {
                fprintf(stderr, "Failed to read page from file\n");
                continue;
            }

            struct uffdio_copy copy = {
                .dst = page_addr,
                .src = (uint64_t)buffer,
                .len = PAGE_SIZE,
                .mode = 0  // Wake thread after copy
            };

            if (ioctl(cfg->uffd, UFFDIO_COPY, &copy) < 0) {
                perror("UFFDIO_COPY");
                continue;
            }

            // Update shared state (may already be LOADED)
            if (cfg->shared_pages[page_idx].state == PAGE_UNLOADED) {
                cfg->shared_pages[page_idx].state = PAGE_LOADED;
            }
            printf("[UFFD Handler] Page %d loaded\n", page_idx);
            fflush(stdout);
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