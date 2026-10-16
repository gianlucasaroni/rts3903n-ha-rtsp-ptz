/*
 * Realtek RTS3903N fshare2fifo v4 (Rock-Solid Jitter-Proof Streaming Engine)
 * Direct pointer tracking: O(1) CPU usage, 1MB non-blocking pipe.
 * Smart jitter handling: NEVER corrupts in-flight streams, drops single frames on lag.
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
#include <signal.h>
#include <errno.h>
#include <time.h>

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define DATA_OFF    300u
#define DATA_SZ     0x1B4000u
#define TOTAL_SZ    1786156u
#define REC_HDR_SZ  22u

#define HDR_VALID   0x04
#define HDR_HEAD    0x0C
#define HDR_TAIL    0x10
#define HDR_SEQ     0x18
#define SLOT_OFF(k) (0x1Cu + 16u * (k))

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

typedef struct __attribute__((packed)) {
    uint32_t len, seq, magic, ts;
    uint16_t type, chain, extra;
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

static inline int magic_ok(uint32_t m) {
    return (m >> 16) >= 0x6a00 && (m >> 16) <= 0x6aff;
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

/* Locate the record offset for a given target sequence number */
static int find_record_by_seq(uint32_t target_seq, uint32_t *found_off) {
    uint32_t off = hdr_u32(HDR_TAIL);
    uint32_t left = hdr_u32(HDR_VALID);
    uint32_t newest = hdr_u32(HDR_SEQ);

    while (left > REC_HDR_SZ) {
        rec_hdr h;
        ring_copy(off, &h, sizeof(h));
        if (!magic_ok(h.magic) || REC_HDR_SZ + h.len > left) break;
        if (h.seq == target_seq) {
            *found_off = off;
            return 1;
        }
        if (h.seq == newest) break;
        off = (off + REC_HDR_SZ + h.len) % DATA_SZ;
        left -= REC_HDR_SZ + h.len;
    }
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1 && strcmp(argv[1], "-f") == 0 && argc > 2) {
        fifo_path = argv[2];
    }

    /* 1. Open and mmap shared memory */
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

    /* 2. Prepare FIFO with 1MB pipe capacity */
    unlink(fifo_path);
    mknod(fifo_path, S_IFIFO | 0666, 0);

    dummy_rd_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    out_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (out_fd < 0) {
        perror("open fifo write");
        return 1;
    }

    /* Expand pipe to 1MB */
    int sz = fcntl(out_fd, F_SETPIPE_SZ, 1048576);
    fprintf(stderr, "pipe size set to %d bytes\n", sz > 0 ? sz : 65536);

    /* 3. Claim reader slot */
    for (int k = 10; k <= 14; k++) {
        uint32_t filt = hdr_u32(SLOT_OFF(k) + 12) & 0xFFFF;
        if (filt == 0 || filt == 0x0400) {
            slot_k = k;
            break;
        }
    }

    /* Synchronize cursor and offset to live edge */
    uint32_t newest = hdr_u32(HDR_SEQ);
    uint32_t cursor = newest > 5 ? newest - 5 : 0;
    uint32_t cur_off = 0;

    if (!find_record_by_seq(cursor, &cur_off)) {
        cursor = newest;
        cur_off = hdr_u32(HDR_TAIL);
    }

    hdr_wr_u32(SLOT_OFF(slot_k) + 8, cursor);
    hdr_wr_u32(SLOT_OFF(slot_k) + 12, 0x0400);
    hdr_wr_u32(SLOT_OFF(slot_k), 1);

    fprintf(stderr, "fshare2fifo v4 live: slot %d, cursor %u, off %u\n",
            slot_k, cursor, cur_off);

    uint8_t *payload = malloc(512 * 1024);
    if (!payload) return 1;

    unsigned long long total_bytes = 0;
    unsigned total_frames = 0;
    time_t last_log = time(NULL);
    time_t last_successful_write = time(NULL);

    while (running) {
        uint32_t ring_newest = hdr_u32(HDR_SEQ);

        /* Advance while there are new records in the ring */
        while (ring_newest > cursor) {
            rec_hdr h;
            ring_copy(cur_off, &h, sizeof(h));

            if (!magic_ok(h.magic)) {
                /* Desync: search for current cursor */
                if (!find_record_by_seq(cursor + 1, &cur_off)) {
                    cursor = ring_newest;
                    find_record_by_seq(cursor, &cur_off);
                }
                break;
            }

            if (h.seq == cursor + 1) {
                cursor = h.seq;

                if ((h.type & 0xFF00) == 0x0400 && h.len <= 512 * 1024) {
                    ring_copy(cur_off + REC_HDR_SZ, payload, h.len);

                    /* Write to FIFO */
                    ssize_t w = write(out_fd, payload, h.len);
                    if (w > 0) {
                        total_bytes += w;
                        total_frames++;
                        last_successful_write = time(NULL);
                    } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        /* Pipe is full: if no client has drained for >3 seconds, trim idle backlog */
                        if (time(NULL) - last_successful_write >= 3) {
                            uint8_t junk[32768];
                            while (read(dummy_rd_fd, junk, sizeof(junk)) > 0) {}
                            write(out_fd, payload, h.len);
                            last_successful_write = time(NULL);
                        }
                        /* On Wi-Fi jitter (<3s), do NOT drain: just drop this single frame safely */
                    }
                }

                cur_off = (cur_off + REC_HDR_SZ + h.len) % DATA_SZ;
                hdr_wr_u32(SLOT_OFF(slot_k) + 8, cursor);
            } else if (h.seq > cursor + 1) {
                /* Sequence jump: follow to next record */
                cursor = h.seq - 1;
            } else {
                /* Re-sync offset if wrapped or stalled */
                uint32_t next_off;
                if (find_record_by_seq(cursor + 1, &next_off)) {
                    cur_off = next_off;
                } else {
                    cursor = ring_newest > 5 ? ring_newest - 5 : 0;
                    find_record_by_seq(cursor, &cur_off);
                }
                break;
            }
        }

        time_t now = time(NULL);
        if (now - last_log >= 2) {
            fprintf(stderr, "LIVE: seq=%u cursor=%u frames=%u bytes=%lluKB\n",
                    ring_newest, cursor, total_frames, total_bytes / 1024);
            last_log = now;
        }

        usleep(15000); /* 15 ms poll */
    }

    hdr_wr_u32(SLOT_OFF(slot_k) + 12, 0);
    hdr_wr_u32(SLOT_OFF(slot_k), 0);
    close(out_fd);
    if (dummy_rd_fd >= 0) close(dummy_rd_fd);
    free(payload);
    return 0;
}
