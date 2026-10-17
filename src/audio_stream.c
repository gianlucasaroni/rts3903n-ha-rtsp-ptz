/*
 * Realtek RTS3903N Audio Streamer (Microphone AAC over HTTP)
 * Reads AAC-LC (16kHz Mono ADTS) from /dev/shm/fshare_frame_buf
 * Serves live audio over HTTP on port 8081 for go2rtc / Home Assistant / Frigate
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"
#define DATA_OFF    300u
#define DATA_SZ     0x1B4000u
#define TOTAL_SZ    1786156u
#define REC_HDR_SZ  22u

#define HDR_TAIL    0x10u
#define HDR_SEQ     0x18u

#define AUDIO_PORT  8081

typedef struct __attribute__((packed)) {
    uint32_t len, seq, magic, ts;
    uint16_t type, chain, extra;
} rec_hdr;

static volatile uint8_t *buf;
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

static void sig_handler(int s) {
    (void)s;
    running = 0;
}

static int find_record_by_seq(uint32_t target_seq, uint32_t *found_off) {
    uint32_t off = hdr_u32(HDR_TAIL);
    uint32_t left = hdr_u32(0x04); /* valid bytes */
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
    (void)argc; (void)argv;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    int shm_fd = open(BUFFER_FILE, O_RDONLY);
    if (shm_fd < 0) {
        perror("open shm");
        return 1;
    }
    buf = mmap(NULL, TOTAL_SZ, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(AUDIO_PORT);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (bind(sfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind audio port");
        return 1;
    }

    if (listen(sfd, 2) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "=== RTS3903N Audio Streamer listening on port %d ===\n", AUDIO_PORT);

    uint8_t payload[4096];

    while (running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) break;
            usleep(50000);
            continue;
        }

        fprintf(stderr, "Audio client connected!\n");

        /* Read optional HTTP request */
        char req[1024];
        recv(cfd, req, sizeof(req) - 1, MSG_DONTWAIT);

        /* Send HTTP response headers */
        const char *hdr = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: audio/aac\r\n"
                          "Connection: close\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Access-Control-Allow-Origin: *\r\n\r\n";
        send(cfd, hdr, strlen(hdr), 0);

        /* Synchronize to live edge */
        uint32_t newest = hdr_u32(HDR_SEQ);
        uint32_t cursor = newest > 10 ? newest - 10 : newest;
        uint32_t cur_off = 0;
        if (!find_record_by_seq(cursor, &cur_off)) {
            cursor = newest;
            cur_off = hdr_u32(HDR_TAIL);
        }

        while (running) {
            uint32_t ring_newest = hdr_u32(HDR_SEQ);

            if (cursor > ring_newest || (ring_newest - cursor > 100)) {
                cursor = ring_newest > 5 ? ring_newest - 5 : ring_newest;
                if (!find_record_by_seq(cursor, &cur_off)) {
                    cur_off = hdr_u32(HDR_TAIL);
                }
            }

            int had_audio = 0;
            while (ring_newest > cursor) {
                rec_hdr h;
                ring_copy(cur_off, &h, sizeof(h));

                if (!magic_ok(h.magic) || h.seq > ring_newest) {
                    if (!find_record_by_seq(cursor + 1, &cur_off)) {
                        cursor = ring_newest;
                        find_record_by_seq(cursor, &cur_off);
                    }
                    break;
                }

                if (h.seq == cursor + 1) {
                    cursor = h.seq;

                    /* Type 0x0100 is AAC audio */
                    if ((h.type & 0xFF00) == 0x0100 && h.len <= sizeof(payload)) {
                        ring_copy(cur_off + REC_HDR_SZ, payload, h.len);
                        ssize_t s = send(cfd, payload, h.len, 0);
                        if (s < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                goto client_done;
                            }
                        }
                        had_audio = 1;
                    }

                    cur_off = (cur_off + REC_HDR_SZ + h.len) % DATA_SZ;
                } else if (h.seq > cursor + 1 && h.seq <= ring_newest) {
                    cursor = h.seq - 1;
                } else {
                    break;
                }
            }

            usleep(had_audio ? 10000 : 20000); /* 10-20ms poll */
        }

client_done:
        close(cfd);
        fprintf(stderr, "Audio client disconnected.\n");
    }

    close(sfd);
    return 0;
}
