#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <mqueue.h>
#include <errno.h>

struct ipc_msg {
    uint32_t sender;   /* 2 = DISPATCH */
    uint32_t dest;     /* 1 = RMM */
    uint16_t mainOp;   /* 0x1000 = RMM_SPEAK_SUCCESS */
    uint16_t subOp;    /* 0x1000 */
    uint32_t data;     /* 0 */
};

int main(int argc, char *argv[]) {
    printf("=== RTS3903N RMM Video Stream Activator ===\n");

    /* 1. Ensure /tmp/mmap.info has active flags */
    int fd = open("/tmp/mmap.info", O_RDWR);
    if (fd >= 0) {
        uint8_t *map = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            uint32_t *p50c = (uint32_t *)(map + 0x50c);
            uint32_t *p5a0 = (uint32_t *)(map + 0x5a0);
            uint32_t *p5bc = (uint32_t *)(map + 0x5bc);
            uint32_t *p600 = (uint32_t *)(map + 0x600);

            printf("mmap.info before: 0x50c=%u, 0x5a0=%u, 0x5bc=%u, 0x600=%u\n",
                   *p50c, *p5a0, *p5bc, *p600);

            *p50c = 1;
            *p5a0 = 1; /* wifi connected */
            *p5bc = 0; /* disable QR scanner mode */
            *p600 = 0;

            msync(map, 4096, MS_SYNC);
            munmap(map, 4096);
            printf("mmap.info updated successfully.\n");
        }
        close(fd);
    } else {
        printf("Notice: /tmp/mmap.info open failed: %s\n", strerror(errno));
    }

    /* 2. Send RMM_SPEAK_SUCCESS (0x1000) to /ipc_rmm */
    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.sender = 2; /* DISPATCH */
    msg.dest   = 1; /* RMM */
    msg.mainOp = 0x1000;
    msg.subOp  = 0x1000;
    msg.data   = 0;

    mqd_t q = mq_open("/ipc_rmm", O_WRONLY);
    if (q == (mqd_t)-1) {
        perror("mq_open /ipc_rmm");
        return 1;
    }

    if (mq_send(q, (const char *)&msg, sizeof(msg), 0) < 0) {
        perror("mq_send");
        mq_close(q);
        return 1;
    }

    printf("Sent send_success_msg (0x1000) to /ipc_rmm OK!\n");
    mq_close(q);
    return 0;
}
