#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/sha.h>

#define PAGE_SIZE 4096
#define SLOT_SIZE (PAGE_SIZE + 8)  /* 4KB + 8B header */
#define HASH_MODULUS (1ULL << 12)  /* 4K slots = ~16MB max, for testing */
#define MAX_PROBE_COUNT 64
#define HEADER_SIZE 8

struct page_table_entry {
    uint64_t va_offset;
    uint64_t hash_idx;
};

static uint64_t compute_hash_idx(const void *data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data, PAGE_SIZE, hash);

    /* Use first 8 bytes as hash_idx */
    uint64_t hash_idx;
    memcpy(&hash_idx, hash, sizeof(hash_idx));
    return hash_idx;
}

static int write_page_to_slot(const char *pages_path, uint64_t hash_idx,
                               const void *data)
{
    int fd;
    uint64_t file_index;
    uint64_t slot_hash;
    uint32_t probe;
    uint8_t header[HEADER_SIZE];

    fd = open(pages_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return -1;

    /* Open addressing */
    file_index = hash_idx % HASH_MODULUS;

    for (probe = 0; probe < MAX_PROBE_COUNT; probe++) {
        off_t offset = file_index * SLOT_SIZE;

        /* Read existing header (if any) */
        ssize_t ret = pread(fd, header, HEADER_SIZE, offset);

        memcpy(&slot_hash, header, HEADER_SIZE);

        if (ret < HEADER_SIZE || slot_hash == 0) {
            /* Empty slot (beyond EOF or hole) */
            memcpy(header, &hash_idx, HEADER_SIZE);
            pwrite(fd, header, HEADER_SIZE, offset);
            pwrite(fd, data, PAGE_SIZE, offset + HEADER_SIZE);
            close(fd);
            return 0;
        }

        if (slot_hash == hash_idx) {
            /* Already exists, same content - dedup */
            close(fd);
            return 0;
        }

        /* Linear probing */
        file_index = (file_index + 1) % HASH_MODULUS;
    }

    close(fd);
    return -1;  /* Max probe exceeded */
}

int main(int argc, char *argv[])
{
    int snap_fd, pt_fd;
    const char *snapshot_path;
    const char *output_dir;
    char pages_path[256];
    char pt_path[256];
    char meta_path[256];
    struct stat st;
    uint64_t total_size;
    uint64_t page_count;
    uint64_t unique_pages = 0;
    void *page_data;
    struct page_table_entry pte;
    FILE *meta_fp;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <snapshot_file> <output_dir>\n", argv[0]);
        return 1;
    }

    snapshot_path = argv[1];
    output_dir = argv[2];

    snap_fd = open(snapshot_path, O_RDONLY);
    if (snap_fd < 0) {
        perror("open snapshot");
        return 1;
    }

    if (stat(snapshot_path, &st) < 0) {
        perror("stat");
        close(snap_fd);
        return 1;
    }

    total_size = st.st_size;
    page_count = total_size / PAGE_SIZE;

    /* Create output paths */
    snprintf(pages_path, sizeof(pages_path), "%s/pages.bin", output_dir);
    snprintf(pt_path, sizeof(pt_path), "%s/page_table.bin", output_dir);
    snprintf(meta_path, sizeof(meta_path), "%s/metadata.json", output_dir);

    /* Open output files */
    pt_fd = open(pt_path, O_WRONLY | O_CREAT, 0644);
    if (pt_fd < 0) {
        perror("open page_table");
        close(snap_fd);
        return 1;
    }

    page_data = malloc(PAGE_SIZE);
    if (!page_data) {
        close(snap_fd);
        close(pt_fd);
        return 1;
    }

    /* Process each page */
    for (uint64_t va = 0; va < total_size; va += PAGE_SIZE) {
        ssize_t ret = pread(snap_fd, page_data, PAGE_SIZE, va);
        if (ret != PAGE_SIZE) {
            perror("read page");
            break;
        }

        /* Compute hash */
        uint64_t hash_idx = compute_hash_idx(page_data);

        /* Write to page table */
        pte.va_offset = va;
        pte.hash_idx = hash_idx;
        write(pt_fd, &pte, sizeof(pte));

        /* Write to pages.bin (dedup) */
        if (write_page_to_slot(pages_path, hash_idx, page_data) == 0) {
            unique_pages++;
        }
    }

    /* Write metadata */
    meta_fp = fopen(meta_path, "w");
    if (meta_fp) {
        fprintf(meta_fp, "{\n");
        fprintf(meta_fp, "  \"template_id\": \"%s\",\n", output_dir);
        fprintf(meta_fp, "  \"total_size\": %llu,\n", total_size);
        fprintf(meta_fp, "  \"page_count\": %llu,\n", page_count);
        fprintf(meta_fp, "  \"unique_pages\": %llu,\n", unique_pages);
        fprintf(meta_fp, "  \"hash_modulus\": %llu\n", HASH_MODULUS);
        fprintf(meta_fp, "}\n");
        fclose(meta_fp);
    }

    free(page_data);
    close(snap_fd);
    close(pt_fd);

    printf("Processed %llu pages, %llu unique\n", page_count, unique_pages);

    return 0;
}