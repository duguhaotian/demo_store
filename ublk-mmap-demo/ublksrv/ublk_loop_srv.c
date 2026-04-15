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