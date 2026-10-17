/*
 * Realtek RTS3903N fshare2fifo v6 (Ultra-Low-Latency Real-Time Streaming Engine)
 * Direct pointer tracking: O(1) CPU usage, 128KB non-blocking pipe.
 * Zero-Lag Keyframe Flushing: Purges accumulated pipe backlog on every IDR frame
 * and on buffer-full events, eliminating 4-8s delay and locking stream to real-time live edge.
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

/* Set pipe buffer size to 128KB:
 * Big enough to fit any 1080p keyframe (40-70KB) without fragmentation.
 * Small enough that maximum buffer delay is capped at ~500ms instead of 4.2s.
 */
#define TARGET_PIPE_SZ (128 * 1024)

/* Backlog threshold (bytes) in pipe before triggering keyframe live-edge flush */
#define PURGE_THRESHOLD (32 * 1024)

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

/* Check if H.264 frame contains an SPS (7) or IDR Keyframe (5) */
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

/* Drain all unread bytes from pipe to instantly restore live edge */
static inline void drain_pipe(void) {
    if (dummy_rd_fd < 0) return;
    uint8_t junk[32768];
    while (read(dummy_rd_fd, junk, sizeof(junk)) > 0) {}
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

    /* 2. Prepare FIFO with 128KB low-latency pipe capacity */
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
    uint32_t cursor = newest > 2 ? newest - 2 : newest;
    uint32_t cur_off = 0;

    if (!find_record_by_seq(cursor, &cur_off)) {
        cursor = newest;
        cur_off = hdr_u32(HDR_TAIL);
    }

    hdr_wr_u32(SLOT_OFF(slot_k) + 8, cursor);
    hdr_wr_u32(SLOT_OFF(slot_k) + 12, 0x0400);
    hdr_wr_u32(SLOT_OFF(slot_k), 1);

    fprintf(stderr, "fshare2fifo v6 live: slot %d, cursor %u, off %u\n",
            slot_k, cursor, cur_off);

    uint8_t *payload = malloc(512 * 1024);
    if (!payload) return 1;

    unsigned long long total_bytes = 0;
    unsigned total_frames = 0;
    time_t last_log = time(NULL);

    while (running) {
        uint32_t ring_newest = hdr_u32(HDR_SEQ);

        /* Auto-recover from any desync or cursor falling behind by more than 8 frames (400ms) */
        if (cursor > ring_newest || (ring_newest - cursor > 8)) {
            cursor = ring_newest > 2 ? ring_newest - 2 : ring_newest;
            if (!find_record_by_seq(cursor, &cur_off)) {
                cur_off = hdr_u32(HDR_TAIL);
            }
        }

        /* Advance while there are new records in the ring */
        while (ring_newest > cursor) {
            rec_hdr h;
            ring_copy(cur_off, &h, sizeof(h));

            if (!magic_ok(h.magic) || h.seq > ring_newest) {
                /* Desync: search for current cursor */
                if (!find_record_by_seq(cursor + 1, &cur_off)) {
                    cursor = ring_newest > 2 ? ring_newest - 2 : ring_newest;
                    find_record_by_seq(cursor, &cur_off);
                }
                break;
            }

            if (h.seq == cursor + 1) {
                cursor = h.seq;

                if ((h.type & 0xFF00) == 0x0400 && h.len <= 512 * 1024) {
                    ring_copy(cur_off + REC_HDR_SZ, payload, h.len);

                    int keyframe = is_keyframe(payload, h.len);

                    /* If this is a keyframe, check if any backlog exists in pipe */
                    if (keyframe) {
                        int unread = 0;
                        if (ioctl(dummy_rd_fd, FIONREAD, &unread) == 0 && unread > PURGE_THRESHOLD) {
                            /* Backlog accumulated: purge old frames so player instantly jumps to live keyframe */
                            drain_pipe();
                        }
                    }

                    /* Write to FIFO */
                    ssize_t w = write(out_fd, payload, h.len);
                    if (w > 0) {
                        total_bytes += w;
                        total_frames++;
                    } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        /* Pipe is full (client paused or disconnected): drain and retry once */
                        drain_pipe();
                        w = write(out_fd, payload, h.len);
                        if (w > 0) {
                            total_bytes += w;
                            total_frames++;
                        }
                    }
                }

                cur_off = (cur_off + REC_HDR_SZ + h.len) % DATA_SZ;
                hdr_wr_u32(SLOT_OFF(slot_k) + 8, cursor);
            } else if (h.seq > cursor + 1 && h.seq <= ring_newest) {
                /* Valid sequence jump: follow to next record */
                cursor = h.seq - 1;
            } else {
                /* Re-sync offset if wrapped or stalled */
                uint32_t next_off;
                if (find_record_by_seq(cursor + 1, &next_off)) {
                    cur_off = next_off;
                } else {
                    cursor = ring_newest > 2 ? ring_newest - 2 : ring_newest;
                    find_record_by_seq(cursor, &cur_off);
                }
                break;
            }
        }

        time_t now = time(NULL);
        if (now - last_log >= 2) {
            int unread = 0;
            ioctl(dummy_rd_fd, FIONREAD, &unread);
            fprintf(stderr, "LIVE: seq=%u cursor=%u frames=%u bytes=%lluKB pipe_unread=%dB\n",
                    ring_newest, cursor, total_frames, total_bytes / 1024, unread);
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
