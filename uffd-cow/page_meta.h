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