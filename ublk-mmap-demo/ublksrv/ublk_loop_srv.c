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
    ret = ioctl(srv->ctrl_fd, UBLK_CMD_ADD_DEV, &dev_info);
    if (ret < 0) {
        perror("UBLK_CMD_ADD_DEV");
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

    ret = ioctl(srv->ctrl_fd, UBLK_CMD_SET_PARAMS, &params);
    if (ret < 0) {
        perror("UBLK_CMD_SET_PARAMS");
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

/* Setup queue: mmap IO buffer region and init io_uring */
static int ublk_queue_setup(struct ublk_server *srv, struct ublk_queue *q, int q_id) {
    size_t io_buf_size;
    int ret;

    q->q_id = q_id;
    q->depth = UBLK_QUEUE_DEPTH;

    /* IO buffer: one page per tag (4KB max IO size) */
    io_buf_size = q->depth * 4096;
    q->io_buf_size = io_buf_size;

    /* mmap anonymous memory for IO buffers */
    q->io_buf = mmap(NULL, io_buf_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q->io_buf == MAP_FAILED) {
        perror("mmap io_buf");
        return -1;
    }

    /* Initialize io_uring for async IO */
    ret = io_uring_queue_init(q->depth * 2, &q->ring, 0);
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
        return -1;
    }

    /* Zero-fill if read past EOF or into sparse hole */
    if (ret < size) {
        memset(buf + ret, 0, size - ret);
    }

    return 0;
}

/* Handle write IO request (disabled - read-only demo) */
static int handle_write_io(struct ublk_server *srv, struct ublk_queue *q,
                           int tag, unsigned int sector, unsigned int nr_sectors) {
    printf("WRITE: ignored (read-only mode)\n");
    return -EROFS;  /* Return read-only filesystem error */
}

/* Main IO handling loop - process ublk IO requests */
static void io_loop(struct ublk_server *srv) {
    struct ublk_queue *q = &srv->queues[0];  /* Single queue demo */
    struct io_uring_cqe *cqe;
    int ret;

    printf("IO loop started, waiting for requests...\n");
    printf("Device /dev/ublkb%d ready for mmap access\n", srv->dev_id);
    printf("Press Ctrl+C to stop\n\n");

    while (srv->running) {
        /* Poll for ublk IO events */
        ret = io_uring_wait_cqe(&q->ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;  /* Interrupted, check running */
            perror("io_uring_wait_cqe");
            break;
        }

        /* Process completion event */
        if (cqe) {
            printf("IO completion: tag=%d, res=%d\n",
                   (int)cqe->user_data, cqe->res);
            io_uring_cqe_seen(&q->ring, cqe);
        }

        /* Sleep briefly to avoid busy-wait in demo */
        usleep(10000);  /* 10ms */
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

    /* Setup queue */
    ret = ublk_queue_setup(&g_srv, &g_srv.queues[0], 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to setup queue\n");
        ublk_dev_stop(&g_srv);
        close(g_srv.ctrl_fd);
        close(g_srv.backend_fd);
        return 1;
    }

    /* Start device (make visible) */
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