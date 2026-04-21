#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    int fd;
    void *addr;
    size_t size = 4 * 1024 * 1024;  /* 4MB */

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <snapshot_device>\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open snapshot device");
        return 1;
    }

    /* mmap */
    addr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    printf("Mapped %zu bytes at %p\n", size, addr);

    /* Read first page (trigger fault) */
    printf("First page content (first 32 bytes):\n");
    for (int i = 0; i < 32; i++) {
        printf("%02x ", ((unsigned char *)addr)[i]);
        if ((i + 1) % 16 == 0)
            printf("\n");
    }

    /* Cleanup */
    munmap(addr, size);
    close(fd);

    return 0;
}