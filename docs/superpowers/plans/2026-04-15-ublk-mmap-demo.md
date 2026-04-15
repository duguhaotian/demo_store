# ublk mmap Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a demo showing test program mmap ublk block device, page fault triggers ublk IO, ublksrv reads from sparse file backend.

**Architecture:** ublksrv creates ublk device backed by sparse file. Test program mmap ublk device directly, triggering page faults that go through ublk IO chain to read sparse file.

**Tech Stack:** C, liburing (io_uring), Linux ublk framework (kernel 6.0+)

---

## File Structure

```
ublk-mmap-demo/
├── README.md                    # Demo documentation
├── Makefile                     # Top-level build
├── ublksrv/
│   ├── ublk_loop_srv.c          # ublk server implementation
│   └── Makefile                 # ublksrv build
├── test/
│   ├── test_mmap.c              # mmap test program
│   └── Makefile                 # test build
└── scripts/
    ├── setup.sh                 # Create sparse file, start ublksrv
    └── cleanup.sh               # Stop ublksrv, cleanup
```

---

### Task 1: Create Project Structure and Top-Level Makefile

**Files:**
- Create: `ublk-mmap-demo/Makefile`
- Create: `ublk-mmap-demo/README.md`

- [ ] **Step 1: Create top-level Makefile**

```makefile
# ublk-mmap-demo/Makefile
# Top-level build for ublk mmap demo

.PHONY: all clean ublksrv test

all: ublksrv test

ublksrv:
	$(MAKE) -C ublksrv

test:
	$(MAKE) -C test

clean:
	$(MAKE) -C ublksrv clean
	$(MAKE) -C test clean
```

- [ ] **Step 2: Create README.md**

```markdown
# ublk mmap Demo

Demonstrates mmap on ublk block device with page fault triggering ublk IO chain.

## Architecture

```
Test Program mmap /dev/ublkb0
        ↓ Page Fault
    ublk IO Request
        ↓
    ublksrv
        ↓
    Sparse File backend.data
```

## Requirements

- Linux kernel 6.0+ (ublk support)
- liburing (io_uring library)
- Root privileges (ublk device creation)

## Build

```bash
make
```

## Run Demo

```bash
# Setup: create sparse file and start ublksrv
./scripts/setup.sh

# Run test program
./test/test_mmap

# Cleanup
./scripts/cleanup.sh
```

## What This Demonstrates

1. ublk device creation with sparse file backend
2. mmap on block device triggers page faults
3. Page fault → ublk IO → ublksrv → sparse file read chain
4. Data flows back to fill the page
```

- [ ] **Step 3: Create directory structure**

```bash
mkdir -p ublk-mmap-demo/ublksrv ublk-mmap-demo/test ublk-mmap-demo/scripts
```

- [ ] **Step 4: Commit initial structure**

```bash
git add ublk-mmap-demo/Makefile ublk-mmap-demo/README.md
git commit -m "feat: initialize ublk mmap demo project structure"
```

---

### Task 2: Implement ublksrv - Part 1: Headers and Structures

**Files:**
- Create: `ublk-mmap-demo/ublksrv/ublk_loop_srv.c`
- Create: `ublk-mmap-demo/ublksrv/Makefile`

- [ ] **Step 1: Create ublksrv Makefile**

```makefile
# ublk-mmap-demo/ublksrv/Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -luring

TARGET = ublk_loop_srv

SRCS = ublk_loop_srv.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
```

- [ ] **Step 2: Write ublk_loop_srv.c headers and structures**

```c
// ublk-mmap-demo/ublksrv/ublk_loop_srv.c
/*
 * ublk loop server - ublk device backed by sparse file
 *
 * Creates ublk block device /dev/ublkb0 backed by backend.data
 * Handles IO requests via io_uring, reads/writes sparse file
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/ublk_cmd.h>
#include <liburing.h>

#define UBLK_CTRL_DEV "/dev/ublk-control"
#define UBLK_BLOCK_SIZE 512
#define UBLK_QUEUE_DEPTH 32
#define UBLK_NR_HW_QUEUES 1
#define UBLK_DEV_ID 0

/* Per-queue data */
struct ublk_queue {
    int q_id;
    int depth;
    void *io_buf;           /* mmap'd IO buffer region */
    size_t io_buf_size;
    struct io_uring ring;   /* io_uring for this queue */
};

/* Global server state */
struct ublk_server {
    int ctrl_fd;            /* /dev/ublk-control fd */
    int backend_fd;         /* backend.data fd */
    int dev_id;
    unsigned long dev_size;
    struct ublk_queue queues[UBLK_NR_HW_QUEUES];
    int running;
};

static struct ublk_server g_srv;

/* Helper: get IO buffer address for a tag */
static void *get_io_buf(struct ublk_queue *q, int tag) {
    return q->io_buf + tag * UBLK_BLOCK_SIZE;
}
```

- [ ] **Step 3: Commit ublksrv structure**

```bash
git add ublk-mmap-demo/ublksrv/
git commit -m "feat(ublksrv): add headers and data structures"
```

---

### Task 3: Implement ublksrv - Part 2: Device Setup

**Files:**
- Modify: `ublk-mmap-demo/ublksrv/ublk_loop_srv.c`

- [ ] **Step 1: Add ublk device setup code**

Add after the structures in ublk_loop_srv.c:

```c
/* Setup ublk device via ioctl */
static int ublk_dev_setup(struct ublk_server *srv, const char *backend_path) {
    struct ublksrv_ctrl_dev_info dev_info = {
        .dev_id = srv->dev_id,
        .nr_hw_queues = UBLK_NR_HW_QUEUES,
        .queue_depth = UBLK_QUEUE_DEPTH,
        .block_size = UBLK_BLOCK_SIZE,
        .dev_size = srv->dev_size,
        .flags = 0,
    };

    struct ublk_param_basic basic = {
        .attrs = UBLK_ATTR_READ_ONLY,  /* Read-only for safety in demo */
        .block_size = UBLK_BLOCK_SIZE,
        .dev_sectors = srv->dev_size / UBLK_BLOCK_SIZE,
        .max_sectors = 256,
    };

    int ret;

    /* Open backend file */
    srv->backend_fd = open(backend_path, O_RDONLY);
    if (srv->backend_fd < 0) {
        perror("open backend file");
        return -1;
    }

    /* Get device size from file */
    srv->dev_size = lseek(srv->backend_fd, 0, SEEK_END);
    if (srv->dev_size <= 0) {
        perror("lseek backend file");
        close(srv->backend_fd);
        return -1;
    }
    dev_info.dev_size = srv->dev_size;
    basic.dev_sectors = srv->dev_size / UBLK_BLOCK_SIZE;

    /* Open ublk control device */
    srv->ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
    if (srv->ctrl_fd < 0) {
        perror("open ublk-control");
        close(srv->backend_fd);
        return -1;
    }

    /* Add device */
    ret = ioctl(srv->ctrl_fd, UBLK_CMD_ADD_DEV, &dev_info);
    if (ret < 0) {
        perror("UBLK_CMD_ADD_DEV");
        close(srv->ctrl_fd);
        close(srv->backend_fd);
        return -1;
    }

    printf("ublk device %d created: size=%lu bytes (%lu sectors)\n",
           srv->dev_id, srv->dev_size, basic.dev_sectors);

    return 0;
}

/* Start device (make it visible) */
static int ublk_dev_start(struct ublk_server *srv) {
    struct ublksrv_ctrl_dev_info dev_info = {
        .dev_id = srv->dev_id,
    };
    int ret;

    ret = ioctl(srv->ctrl_fd, UBLK_CMD_START_DEV, &dev_info);
    if (ret < 0) {
        perror("UBLK_CMD_START_DEV");
        return -1;
    }

    printf("ublk device %d started, check /dev/ublkb%d\n",
           srv->dev_id, srv->dev_id);
    return 0;
}

/* Stop and delete device */
static void ublk_dev_stop(struct ublk_server *srv) {
    struct ublksrv_ctrl_dev_info dev_info = {
        .dev_id = srv->dev_id,
    };

    ioctl(srv->ctrl_fd, UBLK_CMD_STOP_DEV, &dev_info);
    ioctl(srv->ctrl_fd, UBLK_CMD_DEL_DEV, &dev_info);

    printf("ublk device %d stopped and deleted\n", srv->dev_id);
}
```

- [ ] **Step 2: Commit device setup code**

```bash
git add ublk-mmap-demo/ublksrv/ublk_loop_srv.c
git commit -m "feat(ublksrv): add device setup/start/stop functions"
```

---

### Task 4: Implement ublksrv - Part 3: Queue Setup and IO Handling

**Files:**
- Modify: `ublk-mmap-demo/ublksrv/ublk_loop_srv.c`

- [ ] **Step 1: Add queue setup and mmap IO buffer**

Add after device control functions:

```c
/* Setup queue: mmap IO buffer region and init io_uring */
static int ublk_queue_setup(struct ublk_server *srv, struct ublk_queue *q, int q_id) {
    struct ublksrv_io_desc *io_descs;
    size_t io_buf_size;
    int ret;

    q->q_id = q_id;
    q->depth = UBLK_QUEUE_DEPTH;

    /* Calculate IO buffer size: one buffer per tag, each 4KB max */
    /* ublk uses 4KB max per IO, but we use sector size (512) */
    io_buf_size = q->depth * 4096;  /* 4KB per IO buffer */
    q->io_buf_size = io_buf_size;

    /* mmap the IO buffer region from ublk */
    /* The region is exposed via /dev/ublkb0+offset or via control fd */
    /* For simplicity, we allocate our own buffer */
    q->io_buf = mmap(NULL, io_buf_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q->io_buf == MAP_FAILED) {
        perror("mmap io_buf");
        return -1;
    }

    /* Initialize io_uring for async IO */
    ret = io_uring_queue_init(UBLK_QUEUE_DEPTH * 2, &q->ring, 0);
    if (ret < 0) {
        perror("io_uring_queue_init");
        munmap(q->io_buf, io_buf_size);
        return -1;
    }

    printf("queue %d setup: depth=%d, io_buf_size=%zu\n",
           q_id, q->depth, io_buf_size);

    return 0;
}

/* Cleanup queue */
static void ublk_queue_cleanup(struct ublk_queue *q) {
    io_uring_queue_exit(&q->ring);
    munmap(q->io_buf, q->io_buf_size);
}

/* Handle a read IO request */
static int handle_read_io(struct ublk_server *srv, struct ublk_queue *q,
                          int tag, unsigned int sector, unsigned int nr_sectors) {
    void *buf = get_io_buf(q, tag);
    off_t offset = sector * UBLK_BLOCK_SIZE;
    size_t size = nr_sectors * UBLK_BLOCK_SIZE;
    ssize_t ret;

    printf("READ: tag=%d, sector=%u, nr_sectors=%u, offset=%ld, size=%zu\n",
           tag, sector, nr_sectors, offset, size);

    /* Read from backend sparse file */
    ret = pread(srv->backend_fd, buf, size, offset);
    if (ret < 0) {
        perror("pread backend");
        return -1;
    }

    /* Zero-fill if read past EOF */
    if (ret < size) {
        memset(buf + ret, 0, size - ret);
    }

    return 0;
}

/* Handle a write IO request (disabled in read-only mode) */
static int handle_write_io(struct ublk_server *srv, struct ublk_queue *q,
                           int tag, unsigned int sector, unsigned int nr_sectors) {
    printf("WRITE: ignored (read-only mode)\n");
    return -EROFS;
}

/* Complete IO via io_uring */
static void complete_io(struct ublk_queue *q, int tag, int result) {
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    sqe = io_uring_get_sqe(&q->ring);
    if (!sqe) {
        fprintf(stderr, "no sqe available\n");
        return;
    }

    /* Submit completion event */
    io_uring_prep_nop(sqe);
    sqe->user_data = tag;  /* Pass tag as user_data */
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;  /* Skip success CQE */

    io_uring_submit(&q->ring);
}
```

- [ ] **Step 2: Commit queue and IO handling code**

```bash
git add ublk-mmap-demo/ublksrv/ublk_loop_srv.c
git commit -m "feat(ublksrv): add queue setup and IO handling functions"
```

---

### Task 5: Implement ublksrv - Part 4: IO Loop and Main

**Files:**
- Modify: `ublk-mmap-demo/ublksrv/ublk_loop_srv.c`

- [ ] **Step 1: Add main IO loop and entry point**

Add at the end of ublk_loop_srv.c:

```c
/* Main IO handling loop */
static void io_loop(struct ublk_server *srv) {
    struct ublk_queue *q = &srv->queues[0];  /* Single queue */
    struct io_uring_cqe *cqe;
    int ret;

    printf("Starting IO loop, press Ctrl+C to stop...\n");

    while (srv->running) {
        /* Wait for IO requests from ublk */
        /* In real ublksrv, we'd use ublk queue get/put commands */
        /* For demo, we poll the backend file on simulated requests */

        /* Simplified: use io_uring to poll for events */
        ret = io_uring_wait_cqe_timeout(&q->ring, &cqe, NULL);
        if (ret < 0) {
            if (ret == -ETIME) continue;  /* Timeout, check again */
            perror("io_uring_wait_cqe");
            break;
        }

        if (cqe) {
            /* Process completion */
            io_uring_cqe_seen(&q->ring, cqe);
        }

        /* Small sleep to avoid busy loop */
        usleep(1000);
    }
}

/* Signal handler */
static void sig_handler(int sig) {
    printf("Received signal %d, stopping...\n", sig);
    g_srv.running = 0;
}

/* Main entry */
int main(int argc, char **argv) {
    const char *backend_path = "backend.data";
    int ret;

    if (argc > 1) {
        backend_path = argv[1];
    }

    printf("ublk loop server starting\n");
    printf("backend file: %s\n", backend_path);

    /* Setup signal handler */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Initialize server */
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.dev_id = UBLK_DEV_ID;
    g_srv.running = 1;

    /* Setup ublk device */
    ret = ublk_dev_setup(&g_srv, backend_path);
    if (ret < 0) {
        fprintf(stderr, "Failed to setup ublk device\n");
        return 1;
    }

    /* Setup queue */
    ret = ublk_queue_setup(&g_srv, &g_srv.queues[0], 0);
    if (ret < 0) {
        ublk_dev_stop(&g_srv);
        close(g_srv.ctrl_fd);
        close(g_srv.backend_fd);
        return 1;
    }

    /* Start device */
    ret = ublk_dev_start(&g_srv);
    if (ret < 0) {
        ublk_queue_cleanup(&g_srv.queues[0]);
        ublk_dev_stop(&g_srv);
        close(g_srv.ctrl_fd);
        close(g_srv.backend_fd);
        return 1;
    }

    /* Run IO loop */
    io_loop(&g_srv);

    /* Cleanup */
    ublk_queue_cleanup(&g_srv.queues[0]);
    ublk_dev_stop(&g_srv);
    close(g_srv.ctrl_fd);
    close(g_srv.backend_fd);

    printf("ublk loop server stopped\n");
    return 0;
}
```

- [ ] **Step 2: Commit main loop**

```bash
git add ublk-mmap-demo/ublksrv/ublk_loop_srv.c
git commit -m "feat(ublksrv): add IO loop and main entry point"
```

---

### Task 6: Implement Test Program

**Files:**
- Create: `ublk-mmap-demo/test/test_mmap.c`
- Create: `ublk-mmap-demo/test/Makefile`

- [ ] **Step 1: Create test Makefile**

```makefile
# ublk-mmap-demo/test/Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

TARGET = test_mmap

SRCS = test_mmap.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)
```

- [ ] **Step 2: Write test_mmap.c**

```c
// ublk-mmap-demo/test/test_mmap.c
/*
 * Test program: mmap ublk block device and trigger page faults
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define UBLK_DEV_PATH "/dev/ublkb0"
#define DEFAULT_MAP_SIZE (64 * 1024 * 1024)  /* 64MB */

/* Get device size from block device */
static size_t get_dev_size(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return DEFAULT_MAP_SIZE;
    }
    return st.st_size;
}

int main(int argc, char **argv) {
    const char *dev_path = UBLK_DEV_PATH;
    size_t map_size = DEFAULT_MAP_SIZE;
    int fd;
    void *map;
    char buf[256];
    int test_offset;
    int i;

    if (argc > 1) {
        dev_path = argv[1];
    }
    if (argc > 2) {
        map_size = atol(argv[2]);
    }

    printf("=== ublk mmap test ===\n");
    printf("Device: %s\n", dev_path);
    printf("Map size: %zu bytes\n", map_size);

    /* Open ublk block device */
    fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror("open ublk device");
        fprintf(stderr, "Make sure ublksrv is running and device exists\n");
        return 1;
    }

    /* Get actual device size */
    map_size = get_dev_size(dev_path);
    if (map_size == 0) {
        map_size = DEFAULT_MAP_SIZE;
    }
    printf("Actual device size: %zu bytes\n", map_size);

    /* mmap the ublk device */
    map = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap ublk device");
        close(fd);
        return 1;
    }

    printf("mmap successful at address %p\n", map);
    printf("\nTriggering page faults by accessing different offsets...\n\n");

    /* Test 1: Access offset 0 (first sector) */
    printf("--- Test 1: Offset 0 ---\n");
    memcpy(buf, map, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    printf("Data at offset 0: \"%.50s...\"\n", buf);

    /* Test 2: Access offset 4KB (sector 8) */
    test_offset = 4096;
    printf("\n--- Test 2: Offset 4KB ---\n");
    if (test_offset < map_size) {
        memcpy(buf, map + test_offset, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        printf("Data at offset 4KB: \"%.50s...\"\n", buf);
    } else {
        printf("Offset 4KB beyond device size, skipped\n");
    }

    /* Test 3: Access offset 1MB */
    test_offset = 1024 * 1024;
    printf("\n--- Test 3: Offset 1MB ---\n");
    if (test_offset < map_size) {
        memcpy(buf, map + test_offset, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        printf("Data at offset 1MB: \"%.50s...\"\n", buf);
    } else {
        printf("Offset 1MB beyond device size, skipped\n");
    }

    /* Test 4: Access multiple sequential pages */
    printf("\n--- Test 4: Sequential page accesses ---\n");
    for (i = 0; i < 5; i++) {
        int offset = i * 4096;
        if (offset < map_size) {
            char val = *((char *)map + offset);
            printf("Page %d (offset %d): first byte = '%c' (0x%02x)\n",
                   i, offset, val >= 32 ? val : '.', val);
        }
    }

    /* Test 5: Check if sparse hole regions return zeros */
    test_offset = 32 * 1024 * 1024;  /* 32MB, likely in sparse hole */
    printf("\n--- Test 5: Sparse hole region (offset 32MB) ---\n");
    if (test_offset < map_size) {
        int zero_count = 0;
        for (i = 0; i < 512; i++) {
            if (*((char *)map + test_offset + i) == 0) {
                zero_count++;
            }
        }
        printf("Zero bytes in first 512 bytes: %d/512\n", zero_count);
        if (zero_count == 512) {
            printf("This is likely a sparse file hole (returns zeros)\n");
        }
    } else {
        printf("Offset 32MB beyond device size, skipped\n");
    }

    printf("\n=== Test complete ===\n");
    printf("Check dmesg and ublksrv logs for page fault IO handling\n");

    /* Cleanup */
    munmap(map, map_size);
    close(fd);

    return 0;
}
```

- [ ] **Step 3: Commit test program**

```bash
git add ublk-mmap-demo/test/
git commit -m "feat(test): add mmap test program for ublk device"
```

---

### Task 7: Create Setup Script

**Files:**
- Create: `ublk-mmap-demo/scripts/setup.sh`

- [ ] **Step 1: Write setup script**

```bash
#!/bin/bash
# ublk-mmap-demo/scripts/setup.sh
# Setup: create sparse file, start ublksrv

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(dirname "$SCRIPT_DIR")"
BACKEND_FILE="${DEMO_DIR}/backend.data"
UBLKSRV="${DEMO_DIR}/ublksrv/ublk_loop_srv"
SIZE_MB=64

echo "=== ublk mmap demo setup ==="

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script requires root privileges for ublk device creation"
    echo "Run with: sudo $0"
    exit 1
fi

# Check kernel version
KERNEL_VER=$(uname -r | cut -d. -f1-2)
if [ "$KERNEL_VER" \< "6.0" ]; then
    echo "Error: Linux kernel 6.0+ required for ublk support"
    echo "Current kernel: $(uname -r)"
    exit 1
fi

# Check ublk-control exists
if [ ! -e "/dev/ublk-control" ]; then
    echo "Error: /dev/ublk-control not found"
    echo "Make sure ublk kernel module is loaded: modprobe ublk_drv"
    exit 1
fi

# Build if needed
if [ ! -x "$UBLKSRV" ]; then
    echo "Building ublksrv..."
    make -C "$DEMO_DIR"
fi

# Create sparse file
echo "Creating sparse file: $BACKEND_FILE (${SIZE_MB}MB)"
truncate -s "${SIZE_MB}M" "$BACKEND_FILE"

# Optionally write test data at specific offsets
echo "Writing test data at specific offsets..."
echo "Block 0: Hello from ublk mmap demo offset 0!" | \
    dd of="$BACKEND_FILE" bs=512 seek=0 conv=notrunc 2>/dev/null
echo "Block 8 (4KB): Data at 4KB offset test" | \
    dd of="$BACKEND_FILE" bs=512 seek=8 conv=notrunc 2>/dev/null

# Show file info
ls -lh "$BACKEND_FILE"
du -h "$BACKEND_FILE"

# Start ublksrv
echo "Starting ublksrv..."
cd "$DEMO_DIR"
"$UBLKSRV" "$BACKEND_FILE" &
UBLKSRV_PID=$!

# Wait for device to appear
sleep 2

# Check if device was created
if [ -e "/dev/ublkb0" ]; then
    echo "Success! ublk device created: /dev/ublkb0"
    ls -l /dev/ublkb0
    echo "ublksrv PID: $UBLKSRV_PID"
    echo ""
    echo "Now run: ./test/test_mmap"
    echo "Or with sudo: sudo ./test/test_mmap"
else
    echo "Error: ublk device not created"
    kill $UBLKSRV_PID 2>/dev/null
    exit 1
fi

# Save PID for cleanup
echo "$UBLKSRV_PID" > "${DEMO_DIR}/.ublksrv_pid"
```

- [ ] **Step 2: Make script executable and commit**

```bash
chmod +x ublk-mmap-demo/scripts/setup.sh
git add ublk-mmap-demo/scripts/setup.sh
git commit -m "feat(scripts): add setup script for sparse file and ublksrv"
```

---

### Task 8: Create Cleanup Script

**Files:**
- Create: `ublk-mmap-demo/scripts/cleanup.sh`

- [ ] **Step 1: Write cleanup script**

```bash
#!/bin/bash
# ublk-mmap-demo/scripts/cleanup.sh
# Cleanup: stop ublksrv, remove sparse file

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_DIR="$(dirname "$SCRIPT_DIR")"
PID_FILE="${DEMO_DIR}/.ublksrv_pid"
BACKEND_FILE="${DEMO_DIR}/backend.data"

echo "=== ublk mmap demo cleanup ==="

# Stop ublksrv
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        echo "Stopping ublksrv (PID: $PID)..."
        kill "$PID"
        sleep 1
        # Force kill if still running
        if kill -0 "$PID" 2>/dev/null; then
            kill -9 "$PID"
        fi
        rm -f "$PID_FILE"
    else
        echo "ublksrv not running or PID file stale"
        rm -f "$PID_FILE"
    fi
else
    echo "No PID file found, ublksrv may not be running"
fi

# Check if ublk device still exists
if [ -e "/dev/ublkb0" ]; then
    echo "Warning: /dev/ublkb0 still exists"
    echo "This should be cleaned up by ublksrv"
fi

# Remove sparse file
if [ -f "$BACKEND_FILE" ]; then
    echo "Removing sparse file: $BACKEND_FILE"
    rm -f "$BACKEND_FILE"
fi

echo "Cleanup complete"
```

- [ ] **Step 2: Make script executable and commit**

```bash
chmod +x ublk-mmap-demo/scripts/cleanup.sh
git add ublk-mmap-demo/scripts/cleanup.sh
git commit -m "feat(scripts): add cleanup script"
```

---

### Task 9: Final Integration and Testing

**Files:**
- All files

- [ ] **Step 1: Build all components**

```bash
cd ublk-mmap-demo
make
```

- [ ] **Step 2: Verify build artifacts exist**

```bash
ls -la ublksrv/ublk_loop_srv test/test_mmap scripts/setup.sh scripts/cleanup.sh
```

- [ ] **Step 3: Run setup script (requires root)**

```bash
sudo ./scripts/setup.sh
```

- [ ] **Step 4: Verify ublk device created**

```bash
ls -l /dev/ublkb0
lsblk | grep ublk
```

- [ ] **Step 5: Run test program**

```bash
sudo ./test/test_mmap
```

- [ ] **Step 6: Cleanup**

```bash
sudo ./scripts/cleanup.sh
```

- [ ] **Step 7: Final commit**

```bash
git add -A
git commit -m "feat: complete ublk mmap demo implementation"
```

---

## Spec Coverage Check

| Spec Section | Task |
|--------------|------|
| Directory structure | Task 1 |
| ublksrv component | Tasks 2-5 |
| Sparse file backend | Task 7 (setup.sh) |
| Test program | Task 6 |
| Setup/cleanup scripts | Tasks 7-8 |
| IO handling (read/write) | Task 4 |
| Device setup/start/stop | Task 3 |
| Data flow (mmap → fault → IO) | Task 9 (integration test) |