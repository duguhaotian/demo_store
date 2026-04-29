#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define REGION_SIZE (64 * 1024)
#define PREVIEW_LEN 48
#define WRITE_PAGE_COUNT 3

static const size_t k_write_pages[WRITE_PAGE_COUNT] = {6, 8, 11};

struct page_meta {
    off_t file_offset;
    int loaded;
};

struct demo_ctx {
    size_t page_size;
    size_t region_size;
    size_t page_count;
    int memfd;
    int datafd;
    int uffd;
    char *user_base;
    struct page_meta *meta;
    pthread_t handler_thread;
    volatile int stop_handler;
};

static void fatalf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void die_errno(const char *what)
{
    fatalf("%s: %s", what, strerror(errno));
}

static void write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die_errno("write");
        }
        p += n;
        len -= (size_t)n;
    }
}

static void log_line(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    write_all(STDOUT_FILENO, buf, (size_t)n);
}

static void read_all(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die_errno("read");
        }
        if (n == 0) {
            fatalf("unexpected EOF while reading control pipe");
        }
        p += n;
        len -= (size_t)n;
    }
}

static void make_preview(const char *src, size_t len, char *dst, size_t dst_len)
{
    size_t out = 0;

    while (out + 1 < dst_len && out < len) {
        unsigned char c = (unsigned char)src[out];
        dst[out] = (c >= 32 && c <= 126) ? (char)c : '.';
        out++;
    }
    dst[out] = '\0';
}

static void fill_page_pattern(char *page, size_t page_size, size_t page_idx)
{
    char fill = (char)('A' + (page_idx % 26));
    int n = snprintf(page,
                     page_size,
                     "page%02zu: shared file payload (%c) for uffd demo",
                     page_idx,
                     fill);
    if (n < 0) {
        fatalf("snprintf failed");
    }

    for (size_t i = (size_t)n + 1; i < page_size; ++i) {
        page[i] = fill;
    }
}

static void create_sample_file(const char *path, size_t page_size, size_t page_count)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        die_errno("open sample.bin");
    }

    char *page = malloc(page_size);
    if (page == NULL) {
        fatalf("malloc failed");
    }

    for (size_t i = 0; i < page_count; ++i) {
        memset(page, 0, page_size);
        fill_page_pattern(page, page_size, i);
        write_all(fd, page, page_size);
    }

    free(page);
    if (fsync(fd) < 0) {
        die_errno("fsync sample.bin");
    }
    if (close(fd) < 0) {
        die_errno("close sample.bin");
    }
}

static void init_demo(struct demo_ctx *ctx, const char *sample_path)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->page_size = (size_t)sysconf(_SC_PAGESIZE);
    ctx->region_size = REGION_SIZE;
    ctx->memfd = -1;
    ctx->datafd = -1;
    ctx->uffd = -1;

    if (ctx->region_size % ctx->page_size != 0) {
        fatalf("region size must be page aligned");
    }

    ctx->page_count = ctx->region_size / ctx->page_size;
    ctx->meta = calloc(ctx->page_count, sizeof(*ctx->meta));
    if (ctx->meta == NULL) {
        fatalf("calloc failed");
    }

    create_sample_file(sample_path, ctx->page_size, ctx->page_count);

    ctx->datafd = open(sample_path, O_RDONLY);
    if (ctx->datafd < 0) {
        die_errno("open sample data");
    }

    ctx->memfd = memfd_create("uffd-cow-demo", 0);
    if (ctx->memfd < 0) {
        die_errno("memfd_create");
    }

    if (ftruncate(ctx->memfd, (off_t)ctx->region_size) < 0) {
        die_errno("ftruncate memfd");
    }

    ctx->user_base = mmap(NULL,
                          ctx->region_size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          ctx->memfd,
                          0);
    if (ctx->user_base == MAP_FAILED) {
        die_errno("mmap user_base");
    }

    for (size_t i = 0; i < ctx->page_count; ++i) {
        ctx->meta[i].file_offset = (off_t)(i * ctx->page_size);
        ctx->meta[i].loaded = 0;
    }
}

static void cleanup_demo(struct demo_ctx *ctx)
{
    if (ctx->user_base != NULL && ctx->user_base != MAP_FAILED) {
        munmap(ctx->user_base, ctx->region_size);
    }
    if (ctx->uffd >= 0) {
        close(ctx->uffd);
    }
    if (ctx->memfd >= 0) {
        close(ctx->memfd);
    }
    if (ctx->datafd >= 0) {
        close(ctx->datafd);
    }
    free(ctx->meta);
}

static int uffdio_supported(__u64 ioctls, unsigned long bit)
{
    return (ioctls & (1ULL << bit)) != 0;
}

static void setup_userfaultfd(struct demo_ctx *ctx)
{
    struct uffdio_api api;
    struct uffdio_register reg;

    ctx->uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (ctx->uffd < 0) {
        die_errno("userfaultfd");
    }

    memset(&api, 0, sizeof(api));
    api.api = UFFD_API;
    if (ioctl(ctx->uffd, UFFDIO_API, &api) < 0) {
        die_errno("UFFDIO_API probe");
    }
    if ((api.features & UFFD_FEATURE_MISSING_SHMEM) == 0) {
        fatalf("kernel does not support UFFD_FEATURE_MISSING_SHMEM");
    }
    if ((api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP) == 0) {
        fatalf("kernel does not support UFFD_FEATURE_PAGEFAULT_FLAG_WP");
    }
    if ((api.features & UFFD_FEATURE_WP_HUGETLBFS_SHMEM) == 0) {
        fatalf("kernel does not support UFFD_FEATURE_WP_HUGETLBFS_SHMEM");
    }

    memset(&reg, 0, sizeof(reg));
    reg.range.start = (unsigned long)ctx->user_base;
    reg.range.len = ctx->region_size;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    if (ioctl(ctx->uffd, UFFDIO_REGISTER, &reg) < 0) {
        die_errno("UFFDIO_REGISTER");
    }

    if (!uffdio_supported(reg.ioctls, _UFFDIO_COPY)) {
        fatalf("registered range does not support UFFDIO_COPY");
    }
    if (!uffdio_supported(reg.ioctls, _UFFDIO_WRITEPROTECT)) {
        fatalf("registered range does not support UFFDIO_WRITEPROTECT");
    }
    if (!uffdio_supported(reg.ioctls, _UFFDIO_WAKE)) {
        fatalf("registered range does not support UFFDIO_WAKE");
    }
}

static size_t page_index_from_addr(struct demo_ctx *ctx, unsigned long fault_addr)
{
    size_t offset = (size_t)(fault_addr - (unsigned long)ctx->user_base);
    return offset / ctx->page_size;
}

static unsigned long page_start(struct demo_ctx *ctx, unsigned long addr)
{
    return addr & ~((unsigned long)ctx->page_size - 1UL);
}

static void source_preview(struct demo_ctx *ctx, size_t page_idx, char *preview, size_t preview_len)
{
    char raw[PREVIEW_LEN] = {0};
    ssize_t n = pread(ctx->datafd, raw, sizeof(raw), ctx->meta[page_idx].file_offset);
    if (n < 0) {
        die_errno("pread sample data");
    }
    make_preview(raw, (size_t)n, preview, preview_len);
}

static void mapping_preview(const char *base, char *preview, size_t preview_len)
{
    make_preview(base, PREVIEW_LEN, preview, preview_len);
}

static void log_before_read_fault(const char *who, struct demo_ctx *ctx, size_t page_idx)
{
    char preview[PREVIEW_LEN + 1];

    source_preview(ctx, page_idx, preview, sizeof(preview));
    log_line("[%s pid=%d] before read fault: page%zu mapped-view=<UNLOADED>; source-view=\"%s\"\n",
             who,
             getpid(),
             page_idx,
             preview);
}

static void log_mapping_page(const char *who, size_t page_idx, const char *page)
{
    char preview[PREVIEW_LEN + 1];

    mapping_preview(page, preview, sizeof(preview));
    log_line("[%s pid=%d] page%zu content=\"%s\"\n", who, getpid(), page_idx, preview);
}

static void log_selected_pages(const char *who, struct demo_ctx *ctx)
{
    for (size_t i = 0; i < WRITE_PAGE_COUNT; ++i) {
        size_t page_idx = k_write_pages[i];
        log_mapping_page(who, page_idx, ctx->user_base + page_idx * ctx->page_size);
    }
}

static void prefault_selected_pages(const char *who, struct demo_ctx *ctx)
{
    for (size_t i = 0; i < WRITE_PAGE_COUNT; ++i) {
        size_t page_idx = k_write_pages[i];
        volatile char c = ctx->user_base[page_idx * ctx->page_size];
        (void)c;
        log_mapping_page(who, page_idx, ctx->user_base + page_idx * ctx->page_size);
    }
}

static void write_selected_pages(struct demo_ctx *ctx)
{
    for (size_t i = 0; i < WRITE_PAGE_COUNT; ++i) {
        size_t page_idx = k_write_pages[i];
        char value = (char)('X' + (int)i);
        char *page = ctx->user_base + page_idx * ctx->page_size;

        log_line("[child pid=%d] triggering write fault on page%zu[0] = '%c'\n",
                 getpid(),
                 page_idx,
                 value);
        page[0] = value;
    }
}

static void handle_missing_fault(struct demo_ctx *ctx, unsigned long fault_addr)
{
    size_t page_idx = page_index_from_addr(ctx, fault_addr);
    unsigned long aligned = page_start(ctx, fault_addr);
    char *tmp;
    ssize_t nread;
    struct uffdio_copy copy;

    if (page_idx >= ctx->page_count) {
        fatalf("missing fault outside demo range: 0x%lx", fault_addr);
    }

    tmp = mmap(NULL,
               ctx->page_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);
    if (tmp == MAP_FAILED) {
        die_errno("mmap temp page for missing fault");
    }

    memset(tmp, 0, ctx->page_size);
    nread = pread(ctx->datafd, tmp, ctx->page_size, ctx->meta[page_idx].file_offset);
    if (nread < 0) {
        die_errno("pread sample page");
    }

    memset(&copy, 0, sizeof(copy));
    copy.dst = aligned;
    copy.src = (unsigned long)tmp;
    copy.len = ctx->page_size;
    copy.mode = UFFDIO_COPY_MODE_WP;

    log_line("[handler pid=%d] read fault on page%zu, loading file offset %lld\n",
             getpid(),
             page_idx,
             (long long)ctx->meta[page_idx].file_offset);

    if (ioctl(ctx->uffd, UFFDIO_COPY, &copy) < 0) {
        die_errno("UFFDIO_COPY");
    }

    ctx->meta[page_idx].loaded = 1;
    if (munmap(tmp, ctx->page_size) < 0) {
        die_errno("munmap temp page for missing fault");
    }
}

static void handle_wp_fault(struct demo_ctx *ctx, unsigned long fault_addr)
{
    unsigned long aligned = page_start(ctx, fault_addr);
    size_t page_idx = page_index_from_addr(ctx, fault_addr);
    void *snapshot;
    void *private_page;
    struct uffdio_writeprotect wp;
    struct uffdio_range wake;

    snapshot = mmap(NULL,
                    ctx->page_size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1,
                    0);
    if (snapshot == MAP_FAILED) {
        die_errno("mmap snapshot page");
    }

    memcpy(snapshot, (void *)aligned, ctx->page_size);

    memset(&wp, 0, sizeof(wp));
    wp.range.start = aligned;
    wp.range.len = ctx->page_size;
    wp.mode = UFFDIO_WRITEPROTECT_MODE_DONTWAKE;

    log_line("[handler pid=%d] write fault on page%zu, replacing shared page with private anonymous page\n",
             getpid(),
             page_idx);

    if (ioctl(ctx->uffd, UFFDIO_WRITEPROTECT, &wp) < 0) {
        die_errno("UFFDIO_WRITEPROTECT clear");
    }

    private_page = mmap((void *)aligned,
                        ctx->page_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                        -1,
                        0);
    if (private_page == MAP_FAILED) {
        die_errno("mmap private page over shared mapping");
    }

    memcpy(private_page, snapshot, ctx->page_size);

    memset(&wake, 0, sizeof(wake));
    wake.start = aligned;
    wake.len = ctx->page_size;
    if (ioctl(ctx->uffd, UFFDIO_WAKE, &wake) < 0) {
        die_errno("UFFDIO_WAKE");
    }

    if (munmap(snapshot, ctx->page_size) < 0) {
        die_errno("munmap snapshot page");
    }
}

static void *fault_handler_main(void *arg)
{
    struct demo_ctx *ctx = arg;

    while (!ctx->stop_handler) {
        struct pollfd pfd;
        struct uffd_msg msg;
        ssize_t nread;
        int ready;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = ctx->uffd;
        pfd.events = POLLIN;

        ready = poll(&pfd, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EBADF && ctx->stop_handler) {
                break;
            }
            die_errno("poll userfaultfd");
        }
        if (ready == 0 || (pfd.revents & POLLIN) == 0) {
            continue;
        }

        nread = read(ctx->uffd, &msg, sizeof(msg));
        if (nread < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            if (errno == EBADF && ctx->stop_handler) {
                break;
            }
            die_errno("read userfaultfd");
        }
        if ((size_t)nread != sizeof(msg)) {
            fatalf("short read from userfaultfd: %zd", nread);
        }
        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            continue;
        }
        if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {
            handle_wp_fault(ctx, msg.arg.pagefault.address);
        } else {
            handle_missing_fault(ctx, msg.arg.pagefault.address);
        }
    }

    return NULL;
}

static void start_fault_handler(struct demo_ctx *ctx)
{
    if (pthread_create(&ctx->handler_thread, NULL, fault_handler_main, ctx) != 0) {
        fatalf("pthread_create failed");
    }
}

static void stop_fault_handler(struct demo_ctx *ctx)
{
    ctx->stop_handler = 1;
    if (ctx->uffd >= 0) {
        close(ctx->uffd);
        ctx->uffd = -1;
    }
    if (pthread_join(ctx->handler_thread, NULL) != 0) {
        fatalf("pthread_join failed");
    }
}

static void dump_region_layout_for_pid(const char *who,
                                       pid_t pid,
                                       unsigned long region_start,
                                       size_t region_len)
{
    char path[64];
    char *line = NULL;
    size_t cap = 0;
    FILE *fp;
    unsigned long region_end = region_start + region_len;
    size_t shared_bytes = 0;
    size_t private_bytes = 0;
    int matched = 0;

    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    fp = fopen(path, "r");
    if (fp == NULL) {
        die_errno("fopen maps");
    }

    log_line("[%s pid=%d] demo region=[0x%lx-0x%lx) size=%zuKB VMA layout:\n",
             who,
             pid,
             region_start,
             region_end,
             region_len / 1024);

    while (getline(&line, &cap, fp) >= 0) {
        unsigned long vma_start;
        unsigned long vma_end;
        char perms[5] = {0};
        unsigned long overlap_start;
        unsigned long overlap_end;
        size_t overlap_len;

        if (sscanf(line, "%lx-%lx %4s", &vma_start, &vma_end, perms) != 3) {
            continue;
        }
        if (vma_end <= region_start || vma_start >= region_end) {
            continue;
        }

        overlap_start = vma_start > region_start ? vma_start : region_start;
        overlap_end = vma_end < region_end ? vma_end : region_end;
        overlap_len = (size_t)(overlap_end - overlap_start);
        matched = 1;

        if (perms[3] == 's') {
            shared_bytes += overlap_len;
        } else if (perms[3] == 'p') {
            private_bytes += overlap_len;
        }

        log_line("[%s pid=%d]   %s", who, pid, line);
    }

    free(line);
    fclose(fp);

    if (!matched) {
        log_line("[%s pid=%d]   <no overlapping VMA found>\n", who, pid);
    }

    log_line("[%s pid=%d] demo-region shared=%zuKB (%.2f%%), private=%zuKB (%.2f%%)\n",
             who,
             pid,
             shared_bytes / 1024,
             (double)shared_bytes * 100.0 / (double)region_len,
             private_bytes / 1024,
             (double)private_bytes * 100.0 / (double)region_len);
}

static void dump_region_smaps_for_pid(const char *who,
                                      pid_t pid,
                                      unsigned long region_start,
                                      size_t region_len)
{
    char path[64];
    char *line = NULL;
    size_t cap = 0;
    FILE *fp;
    unsigned long region_end = region_start + region_len;
    int print_block = 0;
    int matched = 0;

    snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
    fp = fopen(path, "r");
    if (fp == NULL) {
        die_errno("fopen smaps");
    }

    log_line("[%s pid=%d] demo region smaps blocks:\n", who, pid);

    while (getline(&line, &cap, fp) >= 0) {
        unsigned long vma_start;
        unsigned long vma_end;
        char perms[5] = {0};

        if (sscanf(line, "%lx-%lx %4s", &vma_start, &vma_end, perms) == 3) {
            print_block = !(vma_end <= region_start || vma_start >= region_end);
            if (print_block) {
                matched = 1;
                log_line("[%s pid=%d]   %s", who, pid, line);
            }
            continue;
        }

        if (print_block) {
            log_line("[%s pid=%d]   %s", who, pid, line);
        }
    }

    free(line);
    fclose(fp);

    if (!matched) {
        log_line("[%s pid=%d]   <no overlapping smaps block found for [0x%lx-0x%lx)>\n",
                 who,
                 pid,
                 region_start,
                 region_end);
    }
}

static void child_process(struct demo_ctx *ctx, int notify_fd, int control_fd)
{
    char token;

    setup_userfaultfd(ctx);
    start_fault_handler(ctx);

    log_before_read_fault("child", ctx, 0);
    token = 'B';
    write_all(notify_fd, &token, 1);

    read_all(control_fd, &token, 1);
    if (token != 'R') {
        fatalf("unexpected child control token before read: %c", token);
    }

    log_line("[child pid=%d] triggering read fault on page0\n", getpid());
    log_mapping_page("child-after-read", 0, ctx->user_base);
    token = 'L';
    write_all(notify_fd, &token, 1);

    read_all(control_fd, &token, 1);
    if (token != 'W') {
        fatalf("unexpected child control token before write: %c", token);
    }

    log_line("[child pid=%d] prefaulting sparse write pages\n", getpid());
    prefault_selected_pages("child-prefaulted", ctx);
    log_selected_pages("child-before-write", ctx);
    write_selected_pages(ctx);
    log_selected_pages("child-after-write", ctx);
    token = 'D';
    write_all(notify_fd, &token, 1);

    read_all(control_fd, &token, 1);
    if (token != 'Q') {
        fatalf("unexpected child control token before exit: %c", token);
    }

    stop_fault_handler(ctx);
}

int main(void)
{
    struct demo_ctx ctx;
    int notify_pipe[2];
    int control_pipe[2];
    pid_t child;
    char token;
    int status;

    init_demo(&ctx, "sample.bin");
    log_line("[parent pid=%d] demo region size=%zuKB (%zu pages x %zuB) at %p\n",
             getpid(),
             ctx.region_size / 1024,
             ctx.page_count,
             ctx.page_size,
             ctx.user_base);
    log_line("[parent pid=%d] sparse write pages in middle 24KB: {%zu, %zu, %zu}\n",
             getpid(),
             k_write_pages[0],
             k_write_pages[1],
             k_write_pages[2]);

    if (pipe(notify_pipe) < 0) {
        die_errno("pipe notify");
    }
    if (pipe(control_pipe) < 0) {
        die_errno("pipe control");
    }

    child = fork();
    if (child < 0) {
        die_errno("fork");
    }

    if (child == 0) {
        close(notify_pipe[0]);
        close(control_pipe[1]);
        child_process(&ctx, notify_pipe[1], control_pipe[0]);
        close(notify_pipe[1]);
        close(control_pipe[0]);
        cleanup_demo(&ctx);
        return 0;
    }

    close(notify_pipe[1]);
    close(control_pipe[0]);

    read_all(notify_pipe[0], &token, 1);
    if (token != 'B') {
        fatalf("unexpected token before child read: %c", token);
    }

    log_before_read_fault("parent", &ctx, 0);
    token = 'R';
    write_all(control_pipe[1], &token, 1);

    read_all(notify_pipe[0], &token, 1);
    if (token != 'L') {
        fatalf("unexpected token after child read: %c", token);
    }

    log_mapping_page("parent-after-read", 0, ctx.user_base);
    token = 'W';
    write_all(control_pipe[1], &token, 1);

    read_all(notify_pipe[0], &token, 1);
    if (token != 'D') {
        fatalf("unexpected token after child write: %c", token);
    }

    log_selected_pages("parent-after-write", &ctx);
    dump_region_layout_for_pid("parent", getpid(), (unsigned long)ctx.user_base, ctx.region_size);
    dump_region_layout_for_pid("child", child, (unsigned long)ctx.user_base, ctx.region_size);
    dump_region_smaps_for_pid("parent", getpid(), (unsigned long)ctx.user_base, ctx.region_size);
    dump_region_smaps_for_pid("child", child, (unsigned long)ctx.user_base, ctx.region_size);

    token = 'Q';
    write_all(control_pipe[1], &token, 1);

    if (waitpid(child, &status, 0) < 0) {
        die_errno("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fatalf("child exited abnormally");
    }

    close(notify_pipe[0]);
    close(control_pipe[1]);
    cleanup_demo(&ctx);

    log_line("[parent pid=%d] demo completed successfully\n", getpid());
    return 0;
}
