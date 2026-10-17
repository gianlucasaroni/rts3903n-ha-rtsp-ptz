/*
 * Realtek RTS3903N fshare2fifo v9 (Ultra-Low-Latency Real-Time Streaming Engine)
 * Sequential Ring Tracker: Follows ring buffer directly via head/tail pointers.
 * No sequence desync, zero memory allocation in loop, O(1) CPU.
 * Zero-Lag Keyframe Flushing: Purges pipe backlog on IDR keyframe and buffer-full.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define DATA_OFF    300u
#define DATA_SZ     0x1B4000u
#define TOTAL_SZ    1786156u
#define REC_HDR_SZ  22u

#define HDR_MAGIC   0x00u
#define HDR_VALID   0x04u
#define HDR_HEAD    0x0Cu
#define HDR_TAIL    0x10u
#define HDR_SEQ     0x18u
#define SLOT_OFF(k) (0x1Cu + 16u * (k))

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

#define TARGET_PIPE_SZ (128 * 1024)
#define PURGE_THRESHOLD (32 * 1024)

typedef struct __attribute__((packed)) {
    uint32_t len;      /* Total record length including 22-byte header */
    uint32_t seq;      /* Sequence number */
    uint32_t ts_sec;   /* Timestamp sec */
    uint32_t ts_usec;  /* Timestamp usec */
    uint16_t type;     /* Record type: 0x0402, 0x0404 (H.264 video) */
    uint16_t chain;    /* Chain index */
    uint16_t extra;    /* Extra flags */
} rec_hdr;

static volatile uint8_t *buf;
static int out_fd = -1;
static int dummy_rd_fd = -1;
static const char *fifo_path = "/tmp/h264_high_fifo";
static int slot_k = 10;
static volatile int running = 1;

static void ring_copy(uint32_t off, void *dst, size_t n) {
    size_t first;
    off %= DATA_SZ;
    first = DATA_SZ - off;
    if (first > n) first = n;
    memcpy(dst, (const void *)(buf + DATA_OFF + off), first);
    if (first < n)
        memcpy((uint8_t *)dst + first, (const void *)(buf + DATA_OFF), n - first);
}

static inline uint32_t hdr_u32(unsigned off) {
    uint32_t v;
    memcpy(&v, (const void *)(buf + off), 4);
    return v;
}

static inline void hdr_wr_u32(unsigned off, uint32_t v) {
    memcpy((void *)(buf + off), &v, 4);
}

static void sig_handler(int s) {
    (void)s;
    running = 0;
}

static inline int is_keyframe(const uint8_t *p, size_t len) {
    if (!p || len < 5) return 0;
    size_t limit = len < 64 ? len - 4 : 60;
    for (size_t i = 0; i < limit; i++) {
        if (p[i] == 0x00 && p[i+1] == 0x00) {
            if (p[i+2] == 0x01) {
                uint8_t t = p[i+3] & 0x1F;
                if (t == 5 || t == 7) return 1;
                i += 3;
            } else if (i + 3 < limit && p[i+2] == 0x00 && p[i+3] == 0x01) {
                uint8_t t = p[i+4] & 0x1F;
                if (t == 5 || t == 7) return 1;
                i += 4;
            }
        }
    }
    return 0;
}

static inline void drain_pipe(void) {
    if (dummy_rd_fd < 0) return;
    uint8_t junk[32768];
    while (read(dummy_rd_fd, junk, sizeof(junk)) > 0) {}
}

int main(int argc, char **argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1 && strcmp(argv[1], "-f") == 0 && argc > 2) {
        fifo_path = argv[2];
    }

    int shm_fd = open(BUFFER_FILE, O_RDWR);
    if (shm_fd < 0) {
        shm_fd = open(BUFFER_FILE, O_RDONLY);
        if (shm_fd < 0) {
            perror("open shm");
            return 1;
        }
        buf = mmap(NULL, TOTAL_SZ, PROT_READ, MAP_SHARED, shm_fd, 0);
    } else {
        buf = mmap(NULL, TOTAL_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    }
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    unlink(fifo_path);
    mknod(fifo_path, S_IFIFO | 0666, 0);

    dummy_rd_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    out_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (out_fd < 0) {
        perror("open fifo write");
        return 1;
    }

    int sz = fcntl(out_fd, F_SETPIPE_SZ, TARGET_PIPE_SZ);
    fprintf(stderr, "pipe size set to %d bytes (ultra-low-latency mode)\n",
            sz > 0 ? sz : 65536);

    for (int k = 10; k <= 14; k++) {
        uint32_t filt = hdr_u32(SLOT_OFF(k) + 12) & 0xFFFF;
        if (filt == 0 || filt == 0x0400) {
            slot_k = k;
            break;
        }
    }

    uint8_t *payload = malloc(512 * 1024);
    if (!payload) return 1;

    /* Start directly at HDR_HEAD (live edge) */
    uint32_t cur_off = hdr_u32(HDR_HEAD);
    uint32_t last_seq = hdr_u32(HDR_SEQ);

    /* Claim reader slot with active PID so rmm recognizes the consumer */
    hdr_wr_u32(SLOT_OFF(slot_k) + 4, (uint32_t)getpid());
    hdr_wr_u32(SLOT_OFF(slot_k) + 8, last_seq);
    hdr_wr_u32(SLOT_OFF(slot_k) + 12, 0x0d00);
    hdr_wr_u32(SLOT_OFF(slot_k), 1);

    fprintf(stderr, "fshare2fifo v9 live edge started: slot %d, pid %u, off %u, seq %u\n",
            slot_k, (unsigned)getpid(), cur_off, last_seq);

    unsigned long long total_bytes = 0;
    unsigned total_frames = 0;
    time_t last_log = time(NULL);

    while (running) {
        uint32_t head = hdr_u32(HDR_HEAD);

        /* Read all available records up to head */
        while (cur_off != head && running) {
            rec_hdr h;
            ring_copy(cur_off, &h, sizeof(h));

            if (h.len < REC_HDR_SZ || h.len > 512 * 1024) {
                /* Buffer wrapped or corrupted: resync to head */
                cur_off = head;
                break;
            }

            uint32_t payload_len = h.len - REC_HDR_SZ;

            /* Video record (type 0x0400 = 1080p stream) */
            if ((h.type & 0xFF00) == 0x0400 && payload_len > 0) {
                ring_copy(cur_off + REC_HDR_SZ, payload, payload_len);

                int keyframe = is_keyframe(payload, payload_len);
                if (keyframe) {
                    int unread = 0;
                    if (ioctl(dummy_rd_fd, FIONREAD, &unread) == 0 && unread > PURGE_THRESHOLD) {
                        drain_pipe();
                    }
                }

                ssize_t w = write(out_fd, payload, payload_len);
                if (w > 0) {
                    total_bytes += w;
                    total_frames++;
                } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    drain_pipe();
                    w = write(out_fd, payload, payload_len);
                    if (w > 0) {
                        total_bytes += w;
                        total_frames++;
                    }
                }
            }

            last_seq = h.seq;
            cur_off = (cur_off + h.len) % DATA_SZ;
            hdr_wr_u32(SLOT_OFF(slot_k) + 8, last_seq);

            /* Re-read head in case producer advanced */
            head = hdr_u32(HDR_HEAD);
        }

        time_t now = time(NULL);
        if (now - last_log >= 2) {
            int unread = 0;
            ioctl(dummy_rd_fd, FIONREAD, &unread);
            fprintf(stderr, "LIVE: head=%u cur_off=%u seq=%u frames=%u bytes=%lluKB pipe_unread=%dB\n",
                    head, cur_off, last_seq, total_frames, total_bytes / 1024, unread);
            last_log = now;
        }

        usleep(3000); /* 3 ms polling */
    }

    hdr_wr_u32(SLOT_OFF(slot_k) + 4, 0);
    hdr_wr_u32(SLOT_OFF(slot_k) + 12, 0);
    hdr_wr_u32(SLOT_OFF(slot_k), 0);
    close(out_fd);
    if (dummy_rd_fd >= 0) close(dummy_rd_fd);
    free(payload);
    return 0;
}
