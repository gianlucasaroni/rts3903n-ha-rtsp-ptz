/*
 * Realtek RTS3903N Speaker / TTS Server
 * Lightweight HTTP REST server on port 8082
 * Receives AAC audio over HTTP POST and plays it via native /ipc_rmm queue
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <mqueue.h>
#include <signal.h>
#include <errno.h>

#define HTTP_PORT   8082
#define AUDIO_DIR   "/tmp/audio"
#define AUDIO_FILE  "/tmp/audio/success.aac"
#define MQ_NAME     "/ipc_rmm"
#define MSG_SPEAK   4096  /* 0x1000 = RMM_SPEAK_SUCCESS -> /tmp/audio/success.aac */

static volatile int running = 1;

static void sig_handler(int s) {
    (void)s;
    running = 0;
}

/* Ensure /tmp/audio is a real writable directory in tmpfs (RAM) */
static void ensure_audio_dir(void) {
    struct stat st;
    if (lstat(AUDIO_DIR, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            unlink(AUDIO_DIR);
        }
    }
    mkdir(AUDIO_DIR, 0755);
}

/* Send playback command to rmm via POSIX message queue */
static int trigger_playback(void) {
    mqd_t mq = mq_open(MQ_NAME, O_WRONLY);
    if (mq == (mqd_t)-1) {
        perror("mq_open /ipc_rmm");
        return -1;
    }

    char msg[512];
    memset(msg, 0, sizeof(msg));
    /* mainOperation at offset 8 (uint16_t little-endian) */
    msg[8] = (char)(MSG_SPEAK & 0xFF);
    msg[9] = (char)((MSG_SPEAK >> 8) & 0xFF);

    int ret = 0;
    if (mq_send(mq, msg, sizeof(msg), 0) < 0) {
        perror("mq_send /ipc_rmm");
        ret = -1;
    }
    mq_close(mq);
    return ret;
}

int main(void) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    ensure_audio_dir();

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
    sin.sin_port = htons(HTTP_PORT);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (bind(sfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind 8082");
        return 1;
    }

    if (listen(sfd, 5) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "=== RTS3903N Speaker / TTS Server listening on port %d ===\n", HTTP_PORT);

    while (running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) break;
            usleep(50000);
            continue;
        }

        /* Read HTTP request headers */
        char req_buf[4096];
        int req_len = 0;
        int header_end = -1;

        while (req_len < (int)sizeof(req_buf) - 1) {
            int n = recv(cfd, req_buf + req_len, sizeof(req_buf) - 1 - req_len, 0);
            if (n <= 0) break;
            req_len += n;
            req_buf[req_len] = '\0';

            char *delim = strstr(req_buf, "\r\n\r\n");
            if (delim) {
                header_end = (delim - req_buf) + 4;
                break;
            }
        }

        if (header_end < 0) {
            close(cfd);
            continue;
        }

        /* Parse request method and path */
        char method[16], path[256];
        sscanf(req_buf, "%15s %255s", method, path);

        /* Extract Content-Length */
        int content_len = 0;
        char *cl_ptr = strcasestr(req_buf, "Content-Length:");
        if (cl_ptr) {
            content_len = atoi(cl_ptr + 15);
        }

        if (strcmp(path, "/health") == 0 || (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0)) {
            const char *res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Connection: close\r\n\r\n"
                              "{\"status\":\"ready\"}\n";
            send(cfd, res, strlen(res), 0);
        } else if (strcmp(path, "/test") == 0) {
            /* Play default poweron sound */
            ensure_audio_dir();
            unlink(AUDIO_FILE);
            system("cp /home/app/audio_file/common/poweron.aac " AUDIO_FILE);
            int ok = trigger_playback();

            const char *res = ok == 0
                ? "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"played_test\"}\n"
                : "HTTP/1.1 500 Internal Error\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"error\"}\n";
            send(cfd, res, strlen(res), 0);
        } else if (strncmp(path, "/play", 5) == 0 && strcmp(method, "POST") == 0) {
            ensure_audio_dir();

            /* Write incoming audio directly to AUDIO_FILE */
            int out_fd = open(AUDIO_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd < 0) {
                const char *res = "HTTP/1.1 500 Error Opening File\r\nConnection: close\r\n\r\n";
                send(cfd, res, strlen(res), 0);
            } else {
                /* Write any body bytes already read with headers */
                int initial_body = req_len - header_end;
                int total_written = 0;
                if (initial_body > 0) {
                    write(out_fd, req_buf + header_end, initial_body);
                    total_written += initial_body;
                }

                /* Read remaining body */
                char chunk[4096];
                while (total_written < content_len || content_len == 0) {
                    int to_read = sizeof(chunk);
                    if (content_len > 0 && (content_len - total_written) < to_read) {
                        to_read = content_len - total_written;
                    }
                    int n = recv(cfd, chunk, to_read, 0);
                    if (n <= 0) break;
                    write(out_fd, chunk, n);
                    total_written += n;
                    if (content_len > 0 && total_written >= content_len) break;
                }
                close(out_fd);

                fprintf(stderr, "Received TTS audio: %d bytes. Triggering playback...\n", total_written);
                int ok = trigger_playback();

                char res[256];
                if (ok == 0) {
                    snprintf(res, sizeof(res),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"status\":\"playing\",\"bytes\":%d}\n", total_written);
                } else {
                    snprintf(res, sizeof(res),
                        "HTTP/1.1 500 Error Triggering Playback\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"status\":\"error\"}\n");
                }
                send(cfd, res, strlen(res), 0);
            }
        } else {
            const char *res = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            send(cfd, res, strlen(res), 0);
        }

        close(cfd);
    }

    close(sfd);
    return 0;
}
