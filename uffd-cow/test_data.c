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