// ublk-mmap-demo/ublksrv/ublk_loop_srv.c
/*
 * ublk loop server - ublk device backed by sparse file
 *
 * Creates ublk block device /dev/ublkb0 backed by backend.data
 * Handles IO requests via io_uring passthrough commands (URING_CMD)
 *
 * Key components:
 * 1. Open /dev/ublkc{dev_id} and mmap io_cmd_buf for reading IO descriptors
 * 2. Issue FETCH_REQ commands via io_uring URING_CMD before START_DEV
 * 3. Handle IO by reading io_cmd_buf[tag] and submitting COMMIT_AND_FETCH_REQ
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/ublk_cmd.h>
#include <liburing.h>

#define UBLK_CTRL_DEV "/dev/ublk-control"
#define UBLK_BLOCK_SIZE 512
#define UBLK_QUEUE_DEPTH 32
#define UBLK_NR_HW_QUEUES 1
#define UBLK_DEV_ID 0

/* IO command buffer offset for mmap (from ublk_cmd.h) */
#define UBLKSRV_CMD_BUF_OFFSET 0
#define UBLKSRV_IO_BUF_OFFSET   0x80000000

/* Per-queue data */
struct ublk_queue {
    int q_id;
    int depth;
    int cdev_fd;            /* /dev/ublkcN fd for this queue */
    void *io_cmd_buf;       /* mmap'd IO command descriptor buffer */
    size_t io_cmd_buf_size;
    void *io_buf;           /* mmap'd IO data buffer region */
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

/* Helper: get IO data buffer address for a tag */
static void *get_io_buf(struct ublk_queue *q, int tag) {
    return q->io_buf + tag * q->io_buf_size / q->depth;
}

/* Helper: get IO descriptor for a tag from io_cmd_buf */
static struct ublksrv_io_desc *get_io_desc(struct ublk_queue *q, int tag) {
    return (struct ublksrv_io_desc *)(q->io_cmd_buf + tag * sizeof(struct ublksrv_io_desc));
}

/* Submit FETCH_REQ command via io_uring URING_CMD */
static int submit_fetch_req(struct ublk_queue *q, int tag) {
    struct io_uring_sqe *sqe;
    struct ublksrv_io_cmd cmd = {
        .q_id = q->q_id,
        .tag = tag,
        .result = 0,
        .addr = (__u64)get_io_buf(q, tag),  /* IO buffer address for kernel to fill */
    };

    sqe = io_uring_get_sqe(&q->ring);
    if (!sqe) {
        fprintf(stderr, "Failed to get sqe for FETCH_REQ tag=%d\n", tag);
        return -1;
    }

    /* Use URING_CMD for passthrough command */
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->fd = q->cdev_fd;
    sqe->cmd_op = UBLK_U_IO_FETCH_REQ;
    sqe->user_data = tag;  /* Store tag in user_data for CQE identification */
    memcpy(&sqe->cmd, &cmd, sizeof(cmd));

    io_uring_submit(&q->ring);
    return 0;
}

/* Submit COMMIT_AND_FETCH_REQ command via io_uring URING_CMD */
static int submit_commit_and_fetch_req(struct ublk_queue *q, int tag, int result) {
    struct io_uring_sqe *sqe;
    struct ublksrv_io_cmd cmd = {
        .q_id = q->q_id,
        .tag = tag,
        .result = result,
        .addr = (__u64)get_io_buf(q, tag),
    };

    sqe = io_uring_get_sqe(&q->ring);
    if (!sqe) {
        fprintf(stderr, "Failed to get sqe for COMMIT_AND_FETCH_REQ tag=%d\n", tag);
        return -1;
    }

    sqe->opcode = IORING_OP_URING_CMD;
    sqe->fd = q->cdev_fd;
    sqe->cmd_op = UBLK_U_IO_COMMIT_AND_FETCH_REQ;
    sqe->user_data = tag;
    memcpy(&sqe->cmd, &cmd, sizeof(cmd));

    io_uring_submit(&q->ring);
    return 0;
}

/* Setup ublk device via ioctl */
static int ublk_dev_setup(struct ublk_server *srv, const char *backend_path) {
    struct ublksrv_ctrl_dev_info dev_info = {
        .dev_id = srv->dev_id,
        .nr_hw_queues = UBLK_NR_HW_QUEUES,
        .queue_depth = UBLK_QUEUE_DEPTH,
        .max_io_buf_bytes = 4096,    /* 4KB max IO buffer per request */
        .flags = 0,
    };

    struct ublk_param_basic basic = {
        .attrs = UBLK_ATTR_READ_ONLY,  /* Read-only for safety in demo */
        .logical_bs_shift = 9,         /* 2^9 = 512 bytes */
        .physical_bs_shift = 9,        /* same as logical for simplicity */
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
    basic.dev_sectors = srv->dev_size / UBLK_BLOCK_SIZE;

    /* Open ublk control device */
    srv->ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
    if (srv->ctrl_fd < 0) {
        perror("open ublk-control");
        close(srv->backend_fd);
        return -1;
    }

    /* Add device */
    struct ublksrv_ctrl_cmd ctrl_cmd = {
        .dev_id = srv->dev_id,
        .queue_id = -1,
        .len = sizeof(dev_info),
        .addr = (__u64)&dev_info,
    };

    ret = ioctl(srv->ctrl_fd, UBLK_U_CMD_ADD_DEV, &ctrl_cmd);
    if (ret < 0) {
        perror("UBLK_U_CMD_ADD_DEV");
        close(srv->ctrl_fd);
        close(srv->backend_fd);
        return -1;
    }

    /* Set device parameters */
    struct ublk_params params = {
        .len = sizeof(struct ublk_params),
        .types = UBLK_PARAM_TYPE_BASIC,
        .basic = basic,
    };

    ctrl_cmd = (struct ublksrv_ctrl_cmd) {
        .dev_id = srv->dev_id,
        .queue_id = -1,
        .len = sizeof(params),
        .addr = (__u64)&params,
    };

    ret = ioctl(srv->ctrl_fd, UBLK_U_CMD_SET_PARAMS, &ctrl_cmd);
    if (ret < 0) {
        perror("UBLK_U_CMD_SET_PARAMS");
        close(srv->ctrl_fd);
        close(srv->backend_fd);
        return -1;
    }

    printf("ublk device %d created: size=%lu bytes (%llu sectors)\n",
           srv->dev_id, srv->dev_size, (unsigned long long)basic.dev_sectors);

    return 0;
}

/* Start device (make it visible) - must be called AFTER all FETCH_REQs are submitted */
static int ublk_dev_start(struct ublk_server *srv) {
    struct ublksrv_ctrl_cmd ctrl_cmd = {
        .dev_id = srv->dev_id,
        .queue_id = -1,
        .data[0] = getpid(),  /* ublksrv_pid */
    };
    int ret;

    ret = ioctl(srv->ctrl_fd, UBLK_U_CMD_START_DEV, &ctrl_cmd);
    if (ret < 0) {
        perror("UBLK_U_CMD_START_DEV");
        return -1;
    }

    printf("ublk device %d started, check /dev/ublkb%d\n",
           srv->dev_id, srv->dev_id);
    return 0;
}

/* Stop and delete device */
static void ublk_dev_stop(struct ublk_server *srv) {
    struct ublksrv_ctrl_cmd ctrl_cmd = {
        .dev_id = srv->dev_id,
        .queue_id = -1,
    };

    ioctl(srv->ctrl_fd, UBLK_U_CMD_STOP_DEV, &ctrl_cmd);
    ioctl(srv->ctrl_fd, UBLK_U_CMD_DEL_DEV, &ctrl_cmd);

    printf("ublk device %d stopped and deleted\n", srv->dev_id);
}

/* Setup queue: open char device, mmap io_cmd_buf and io_buf, init io_uring */
static int ublk_queue_setup(struct ublk_server *srv, struct ublk_queue *q, int q_id) {
    char cdev_path[32];
    size_t io_cmd_buf_size;
    size_t io_buf_size;
    int ret;

    q->q_id = q_id;
    q->depth = UBLK_QUEUE_DEPTH;

    /* Open char device /dev/ublkcN */
    snprintf(cdev_path, sizeof(cdev_path), "/dev/ublkc%d", srv->dev_id);
    q->cdev_fd = open(cdev_path, O_RDWR);
    if (q->cdev_fd < 0) {
        perror("open char device");
        return -1;
    }

    /* IO command descriptor buffer: queue_depth * sizeof(ublksrv_io_desc) */
    io_cmd_buf_size = q->depth * sizeof(struct ublksrv_io_desc);
    q->io_cmd_buf_size = io_cmd_buf_size;

    /* mmap io_cmd_buf at UBLKSRV_CMD_BUF_OFFSET */
    q->io_cmd_buf = mmap(NULL, io_cmd_buf_size, PROT_READ,
                         MAP_SHARED | MAP_POPULATE, q->cdev_fd,
                         UBLKSRV_CMD_BUF_OFFSET);
    if (q->io_cmd_buf == MAP_FAILED) {
        perror("mmap io_cmd_buf");
        close(q->cdev_fd);
        return -1;
    }

    /* IO data buffer: queue_depth * max_io_buf_bytes (4KB per tag) */
    io_buf_size = q->depth * 4096;
    q->io_buf_size = io_buf_size;

    /* mmap io_buf at UBLKSRV_IO_BUF_OFFSET */
    q->io_buf = mmap(NULL, io_buf_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, q->cdev_fd,
                     UBLKSRV_IO_BUF_OFFSET);
    if (q->io_buf == MAP_FAILED) {
        perror("mmap io_buf");
        munmap(q->io_cmd_buf, io_cmd_buf_size);
        close(q->cdev_fd);
        return -1;
    }

    /* Initialize io_uring for async IO commands */
    ret = io_uring_queue_init(q->depth * 2, &q->ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret));
        munmap(q->io_buf, io_buf_size);
        munmap(q->io_cmd_buf, io_cmd_buf_size);
        close(q->cdev_fd);
        return -1;
    }

    printf("queue %d setup: depth=%d, io_cmd_buf=%zu, io_buf=%zu\n",
           q_id, q->depth, io_cmd_buf_size, io_buf_size);
    return 0;
}

/* Cleanup queue */
static void ublk_queue_cleanup(struct ublk_queue *q) {
    io_uring_queue_exit(&q->ring);
    munmap(q->io_buf, q->io_buf_size);
    munmap(q->io_cmd_buf, q->io_cmd_buf_size);
    close(q->cdev_fd);
}

/* Issue all FETCH_REQ commands before starting device */
static int issue_all_fetch_reqs(struct ublk_queue *q) {
    int i, ret;

    printf("Issuing FETCH_REQ for all %d tags...\n", q->depth);

    for (i = 0; i < q->depth; i++) {
        ret = submit_fetch_req(q, i);
        if (ret < 0) {
            fprintf(stderr, "Failed to submit FETCH_REQ for tag %d\n", i);
            return -1;
        }
    }

    /* Ensure all SQEs are submitted */
    io_uring_submit(&q->ring);

    printf("All FETCH_REQ commands submitted\n");
    return 0;
}

/* Handle read IO request from ublk */
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
        return -EIO;
    }

    /* Zero-fill if read past EOF or into sparse hole */
    if ((size_t)ret < size) {
        memset(buf + ret, 0, size - ret);
    }

    return 0;  /* Success */
}

/* Handle write IO request (disabled - read-only demo) */
static int handle_write_io(struct ublk_server *srv, struct ublk_queue *q,
                           int tag, unsigned int sector, unsigned int nr_sectors) {
    (void)srv; (void)q; (void)tag; (void)sector; (void)nr_sectors;  /* Unused in read-only mode */
    printf("WRITE: ignored (read-only mode)\n");
    return -EROFS;  /* Return read-only filesystem error */
}

/* Process a single IO request from io_cmd_buf */
static int process_io_request(struct ublk_server *srv, struct ublk_queue *q, int tag) {
    struct ublksrv_io_desc *iod = get_io_desc(q, tag);
    __u8 op = ublksrv_get_op(iod);
    unsigned int sector = iod->start_sector;
    unsigned int nr_sectors = iod->nr_sectors;
    int result;

    printf("Processing IO: tag=%d, op=%d, sector=%llu, nr_sectors=%u\n",
           tag, op, (unsigned long long)sector, nr_sectors);

    switch (op) {
    case UBLK_IO_OP_READ:
        result = handle_read_io(srv, q, tag, sector, nr_sectors);
        break;
    case UBLK_IO_OP_WRITE:
        result = handle_write_io(srv, q, tag, sector, nr_sectors);
        break;
    case UBLK_IO_OP_FLUSH:
        result = 0;  /* No flush needed for read-only */
        break;
    case UBLK_IO_OP_DISCARD:
        result = 0;  /* Discard is no-op for demo */
        break;
    case UBLK_IO_OP_WRITE_ZEROES:
        result = 0;  /* Write zeroes is no-op (already zero) */
        break;
    default:
        fprintf(stderr, "Unknown IO op: %d\n", op);
        result = -EINVAL;
        break;
    }

    return result;
}

/* Demo: simulate an IO request to show the data path */
static void simulate_test_io(struct ublk_server *srv, struct ublk_queue *q) {
    /* Simulate reading sector 0 */
    printf("\n=== Simulated IO Test ===\n");
    int ret = handle_read_io(srv, q, 0, 0, 8);  /* tag=0, sector=0, 8 sectors = 4KB */
    if (ret == 0) {
        printf("Simulated read successful\n");
        printf("Data preview: %.50s...\n", (char *)get_io_buf(q, 0));
    } else {
        printf("Simulated read failed: %d\n", ret);
    }
    printf("=== End Simulated IO Test ===\n\n");
}

/* Main IO handling loop - process ublk IO requests */
static void io_loop(struct ublk_server *srv) {
    struct ublk_queue *q = &srv->queues[0];  /* Single queue demo */
    struct io_uring_cqe *cqe;
    int tag, result, ret;

    printf("IO loop started, waiting for requests...\n");
    printf("Device /dev/ublkb%d ready for mmap access\n", srv->dev_id);
    printf("Press Ctrl+C to stop\n\n");

    /* Demo: simulate one IO to show the data path works */
    simulate_test_io(srv, q);

    while (srv->running) {
        /* Wait for io_uring completion (FETCH_REQ or COMMIT_AND_FETCH_REQ) */
        ret = io_uring_wait_cqe(&q->ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;  /* Interrupted, check running */
            perror("io_uring_wait_cqe");
            break;
        }

        if (!cqe) continue;

        tag = (int)cqe->user_data;

        /* CQE completion means a request is ready for processing */
        if (cqe->res == UBLK_IO_RES_OK) {
            /* Process the IO request from io_cmd_buf */
            result = process_io_request(srv, q, tag);

            /* Submit COMMIT_AND_FETCH_REQ with result */
            ret = submit_commit_and_fetch_req(q, tag, result);
            if (ret < 0) {
                fprintf(stderr, "Failed to submit COMMIT_AND_FETCH_REQ for tag %d\n", tag);
            }
        } else if (cqe->res == UBLK_IO_RES_ABORT) {
            fprintf(stderr, "IO abort received for tag %d, device may be stopping\n", tag);
            /* Don't re-fetch on abort */
        } else if (cqe->res == UBLK_IO_RES_NEED_GET_DATA) {
            /* For write requests with UBLK_F_NEED_GET_DATA flag */
            fprintf(stderr, "NEED_GET_DATA for tag %d (not supported in this demo)\n", tag);
        } else {
            fprintf(stderr, "Unexpected CQE result: %d for tag %d\n", cqe->res, tag);
        }

        io_uring_cqe_seen(&q->ring, cqe);
    }
}

/* Signal handler */
static void sig_handler(int sig) {
    printf("\nReceived signal %d, stopping...\n", sig);
    g_srv.running = 0;
}

/* Main entry point */
int main(int argc, char **argv) {
    const char *backend_path = "backend.data";
    int ret;

    if (argc > 1) {
        backend_path = argv[1];
    }

    printf("=== ublk loop server ===\n");
    printf("Backend: %s\n", backend_path);

    /* Setup signal handlers */
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

    /* Setup queue (mmap char device) */
    ret = ublk_queue_setup(&g_srv, &g_srv.queues[0], 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to setup queue\n");
        ublk_dev_stop(&g_srv);
        close(g_srv.ctrl_fd);
        close(g_srv.backend_fd);
        return 1;
    }

    /* IMPORTANT: Issue all FETCH_REQ commands BEFORE START_DEV */
    ret = issue_all_fetch_reqs(&g_srv.queues[0]);
    if (ret < 0) {
        fprintf(stderr, "Failed to issue FETCH_REQ commands\n");
        ublk_queue_cleanup(&g_srv.queues[0]);
        ublk_dev_stop(&g_srv);
        close(g_srv.ctrl_fd);
        close(g_srv.backend_fd);
        return 1;
    }

    /* Start device (make visible) - AFTER all FETCH_REQs are submitted */
    ret = ublk_dev_start(&g_srv);
    if (ret < 0) {
        fprintf(stderr, "Failed to start device\n");
        ublk_queue_cleanup(&g_srv.queues[0]);
        ublk_dev_stop(&g_srv);
        close(g_srv.ctrl_fd);
        close(g_srv.backend_fd);
        return 1;
    }

    /* Run IO loop */
    io_loop(&g_srv);

    /* Cleanup */
    printf("\nCleaning up...\n");
    ublk_queue_cleanup(&g_srv.queues[0]);
    ublk_dev_stop(&g_srv);
    close(g_srv.ctrl_fd);
    close(g_srv.backend_fd);

    printf("Server stopped cleanly\n");
    return 0;
}