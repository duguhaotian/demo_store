#ifndef UFFD_COW_TEST_DATA_H
#define UFFD_COW_TEST_DATA_H

#include "common.h"

// Create test file with PAGE_COUNT pages of distinct patterns
int create_test_file(const char *path);

// Read a page from test file at given offset
int read_page_from_file(const char *path, uint64_t offset,
                        void *buffer, size_t size);

#endif // UFFD_COW_TEST_DATA_H