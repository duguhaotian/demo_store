#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#define PAGE_SIZE 4096
#define SLOT_SIZE (PAGE_SIZE + 8)  /* 4KB + 8B header */
#define HASH_MODULUS (1ULL << 12)  /* 4K slots */
#define MAX_PROBE_COUNT 64
#define HEADER_SIZE 8
#define PAGE_TABLE_ENTRY_SIZE 16   /* 8B va + 8B hash_idx */

struct page_table_entry {
    uint64_t va_offset;
    uint64_t hash_idx;
};

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s -H <hash_idx> <pages.bin>      # Query by hash_idx\n", prog);
    fprintf(stderr, "  %s -V <va_offset> <page_table.bin> <pages.bin>  # Query by va_offset\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -H <hash_idx>   Query pages.bin by hash_idx\n");
    fprintf(stderr, "  -V <va_offset>  Query page_table.bin then pages.bin by va_offset\n");
    fprintf(stderr, "  -h              Show this help\n");
}

/* Lookup page in pages.bin by hash_idx using open addressing */
static int lookup_page_by_hash(const char *pages_path, uint64_t hash_idx,
                               uint64_t *slot_out, uint32_t *probes_out)
{
    int fd;
    uint64_t file_index;
    uint64_t slot_hash;
    uint32_t probe;
    uint8_t header[HEADER_SIZE];

    fd = open(pages_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open pages.bin: %s\n", strerror(errno));
        return -1;
    }

    /* Open addressing lookup */
    file_index = hash_idx % HASH_MODULUS;

    for (probe = 0; probe < MAX_PROBE_COUNT; probe++) {
        off_t offset = file_index * SLOT_SIZE;

        /* Read header */
        ssize_t ret = pread(fd, header, HEADER_SIZE, offset);
        if (ret < HEADER_SIZE) {
            /* Empty or EOF - slot not found */
            close(fd);
            *slot_out = file_index;
            *probes_out = probe;
            return 0;  /* NOT_FOUND: empty slot */
        }

        memcpy(&slot_hash, header, HEADER_SIZE);

        if (slot_hash == hash_idx) {
            /* Found */
            close(fd);
            *slot_out = file_index;
            *probes_out = probe;
            return 1;  /* FOUND */
        }

        if (slot_hash == 0) {
            /* Empty slot */
            close(fd);
            *slot_out = file_index;
            *probes_out = probe;
            return 0;  /* NOT_FOUND: empty slot */
        }

        /* Linear probing */
        file_index = (file_index + 1) % HASH_MODULUS;
    }

    close(fd);
    *slot_out = file_index;
    *probes_out = probe;
    return -2;  /* NOT_FOUND: max probes exceeded */
}

int main(int argc, char *argv[])
{
    int opt;
    uint64_t hash_idx = 0;
    uint64_t va_offset = 0;
    int mode = 0;  /* 0=none, 1=hash_idx, 2=va_offset */
    const char *pages_path = NULL;
    const char *page_table_path = NULL;
    int result;
    uint64_t slot;
    uint32_t probes;

    while ((opt = getopt(argc, argv, "H:V:h")) != -1) {
        switch (opt) {
        case 'H':
            hash_idx = strtoull(optarg, NULL, 10);
            mode = 1;
            break;
        case 'V':
            va_offset = strtoull(optarg, NULL, 10);
            mode = 2;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (mode == 0) {
        fprintf(stderr, "Error: must specify -H or -V\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Mode 1: Query by hash_idx directly */
    if (mode == 1) {
        if (optind >= argc) {
            fprintf(stderr, "Error: missing pages.bin path\n");
            print_usage(argv[0]);
            return 1;
        }
        pages_path = argv[optind];

        result = lookup_page_by_hash(pages_path, hash_idx, &slot, &probes);

        if (result == 1) {
            printf("hash_idx=%llu: FOUND at slot %llu, probes=%u\n",
                   hash_idx, slot, probes);
        } else if (result == 0) {
            printf("hash_idx=%llu: NOT_FOUND (empty slot at %llu)\n",
                   hash_idx, slot);
        } else {
            printf("hash_idx=%llu: NOT_FOUND (max probes exceeded)\n",
                   hash_idx);
        }
        return (result == 1) ? 0 : 1;
    }

    /* Mode 2: Query by va_offset */
    if (mode == 2) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "Error: missing page_table.bin and pages.bin paths\n");
            print_usage(argv[0]);
            return 1;
        }
        page_table_path = argv[optind];
        pages_path = argv[optind + 1];

        /* Read hash_idx from page_table.bin */
        int pt_fd = open(page_table_path, O_RDONLY);
        if (pt_fd < 0) {
            fprintf(stderr, "Error: cannot open page_table.bin: %s\n",
                    strerror(errno));
            return 1;
        }

        /* Each entry is 16 bytes: 8B va_offset + 8B hash_idx */
        off_t pt_offset = (va_offset / PAGE_SIZE) * PAGE_TABLE_ENTRY_SIZE;
        struct page_table_entry pte;

        ssize_t ret = pread(pt_fd, &pte, sizeof(pte), pt_offset);
        close(pt_fd);

        if (ret != sizeof(pte)) {
            fprintf(stderr, "Error: cannot read page_table entry at offset %lld\n",
                    (long long)pt_offset);
            return 1;
        }

        /* Verify va_offset matches */
        if (pte.va_offset != va_offset) {
            printf("va_offset=%llu: NOT_FOUND in page_table (entry has va=%llu)\n",
                   va_offset, pte.va_offset);
            return 1;
        }

        hash_idx = pte.hash_idx;

        /* Now lookup in pages.bin */
        result = lookup_page_by_hash(pages_path, hash_idx, &slot, &probes);

        if (result == 1) {
            printf("va_offset=%llu -> hash_idx=%llu: FOUND at slot %llu\n",
                   va_offset, hash_idx, slot);
        } else if (result == 0) {
            printf("va_offset=%llu -> hash_idx=%llu: NOT_FOUND (empty slot at %llu)\n",
                   va_offset, hash_idx, slot);
        } else {
            printf("va_offset=%llu -> hash_idx=%llu: NOT_FOUND (max probes exceeded)\n",
                   va_offset, hash_idx);
        }
        return (result == 1) ? 0 : 1;
    }

    return 0;
}