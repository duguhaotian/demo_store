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