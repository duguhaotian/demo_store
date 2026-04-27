#include "page_meta.h"
#include <stdio.h>
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