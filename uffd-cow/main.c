#include "common.h"
#include "shmem.h"
#include "uffd_handler.h"
#include "page_meta.h"
#include "test_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

static void demo_operations(struct config *cfg) {
    printf("[Worker] Starting demo operations...\n");
    fflush(stdout);

    // Read page 0 (triggers MISSING)
    char *page0 = (char *)cfg->data_base;
    printf("[Worker] About to read page 0...\n");
    fflush(stdout);
    char first_char = page0[0];
    printf("[Worker] Read page 0: '%c%c%c%c...' (size=%d)\n",
           first_char, first_char, first_char, first_char, PAGE_SIZE);
    fflush(stdout);

    // Read page 1 (triggers MISSING)
    char *page1 = (char *)cfg->data_base + PAGE_SIZE;
    printf("[Worker] About to read page 1...\n");
    fflush(stdout);
    char second_char = page1[0];
    printf("[Worker] Read page 1: '%c%c%c%c...' (size=%d)\n",
           second_char, second_char, second_char, second_char, PAGE_SIZE);
    fflush(stdout);

    // Write to page 0 (triggers WP -> COW)
    printf("[Worker] About to write page 0...\n");
    fflush(stdout);
    memset(page0, 'X', PAGE_SIZE);
    printf("[Worker] Write page 0 complete\n");
    fflush(stdout);

    // Read page 0 again (should see modified value in private page)
    printf("[Worker] About to read page 0 after write...\n");
    fflush(stdout);
    first_char = page0[0];
    printf("[Worker] Read page 0 after write: '%c%c%c%c...' (private COW page)\n",
           first_char, first_char, first_char, first_char);
    fflush(stdout);

    printf("[Worker] Demo complete\n");
    fflush(stdout);
}

static int run_worker(struct config *cfg) {
    printf("[Worker] Starting...\n");
    fflush(stdout);
    printf("[Worker] meta_path: %s\n", cfg->meta_path);
    fflush(stdout);
    printf("[Worker] data_path: %s\n", cfg->data_path);
    fflush(stdout);

    // Open existing memfds
    printf("[Worker] Step 1: Opening memfds...\n");
    fflush(stdout);
    if (open_memfds(cfg) < 0) {
        fprintf(stderr, "Failed to open memfds\n");
        return -1;
    }
    printf("[Worker] Step 1 done: meta_fd=%d, data_fd=%d\n", cfg->meta_fd, cfg->data_fd);
    fflush(stdout);

    // Mmap regions
    printf("[Worker] Step 2: Mapping regions...\n");
    fflush(stdout);
    if (mmap_regions(cfg) < 0) {
        fprintf(stderr, "Failed to mmap regions\n");
        cleanup_shmem(cfg);
        return -1;
    }
    printf("[Worker] Step 2 done: meta_base=%p, data_base=%p\n", cfg->meta_base, cfg->data_base);
    fflush(stdout);

    // Initialize local pages (shared pages already initialized by creator)
    printf("[Worker] Step 3: Initializing local pages...\n");
    fflush(stdout);
    if (init_local_pages(&cfg->local_pages, cfg->page_count) < 0) {
        cleanup_shmem(cfg);
        return -1;
    }
    printf("[Worker] Step 3 done\n");
    fflush(stdout);

    // Initialize UFFD
    printf("[Worker] Step 4: Initializing UFFD...\n");
    fflush(stdout);
    if (init_uffd(cfg) < 0) {
        fprintf(stderr, "Failed to init UFFD\n");
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }
    printf("[Worker] Step 4 done: uffd=%d\n", cfg->uffd);
    fflush(stdout);

    // Register UFFD
    printf("[Worker] Step 5: Registering UFFD...\n");
    fflush(stdout);
    if (register_uffd(cfg) < 0) {
        fprintf(stderr, "Failed to register UFFD\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }
    printf("[Worker] Step 5 done\n");
    fflush(stdout);

    // Enable write protection
    printf("[Worker] Step 6: Enabling write protection...\n");
    fflush(stdout);
    if (enable_writeprotect(cfg) < 0) {
        fprintf(stderr, "Failed to enable write protection\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }
    printf("[Worker] Step 6 done\n");
    fflush(stdout);

    // Start handler thread
    printf("[Worker] Step 7: Starting UFFD thread...\n");
    fflush(stdout);
    pthread_t thread;
    if (start_uffd_thread(cfg, &thread) < 0) {
        fprintf(stderr, "Failed to start UFFD thread\n");
        cleanup_uffd(cfg);
        free_local_pages(cfg->local_pages);
        cleanup_shmem(cfg);
        return -1;
    }
    printf("[Worker] Step 7 done\n");
    fflush(stdout);

    // Run demo operations
    printf("[Worker] Step 8: Running demo...\n");
    fflush(stdout);
    demo_operations(cfg);

    // Cleanup
    stop_uffd_thread(cfg, &thread);
    cleanup_uffd(cfg);
    free_local_pages(cfg->local_pages);
    cleanup_shmem(cfg);

    printf("[Worker] Exiting\n");
    fflush(stdout);
    return 0;
}

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