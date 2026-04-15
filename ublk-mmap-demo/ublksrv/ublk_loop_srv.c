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