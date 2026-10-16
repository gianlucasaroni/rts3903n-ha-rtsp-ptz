#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>

#define DEFAULT_PORT 8080
#define DEV_SSP "/dev/ssp"

/*
 * MS41909 Stepper Motor Controller
 * ioctl cmd 1: Get status
 * ioctl cmd 2: Move motor
 * ioctl cmd 3: Stop motor
 *
 * Directions:
 *   1 = UP (Tilt)
 *   2 = DOWN (Tilt)
 *   3 = LEFT (Pan)
 *   4 = RIGHT (Pan)
 */

struct motor_arg {
    int f0;
    int f1;
    int dir;
    int speed;
    int steps;
    int f5;
    int f6;
    int f7;
};

static int ssp_fd = -1;

static int open_ssp(void) {
    if (ssp_fd < 0) {
        ssp_fd = open(DEV_SSP, O_RDWR);
        if (ssp_fd < 0) {
            perror("open " DEV_SSP);
            return -1;
        }
    }
    return ssp_fd;
}

static int do_move(int dir, int steps, int speed) {
    if (open_ssp() < 0) return -1;
    if (speed <= 0) speed = 10;
    if (steps <= 0) steps = 50;

    struct motor_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.dir = dir;
    arg.speed = speed;
    arg.steps = steps * 8; /* Hardware formula: steps * 8 for speed 10 */

    int ret = ioctl(ssp_fd, 2, &arg);
    if (ret < 0) {
        perror("ioctl move");
        return -1;
    }
    return 0;
}

static int do_stop(void) {
    if (open_ssp() < 0) return -1;
    return ioctl(ssp_fd, 3, 0);
}

static int do_status(int *out8) {
    if (open_ssp() < 0) return -1;
    return ioctl(ssp_fd, 1, out8);
}

static int parse_dir(const char *s) {
    if (!s) return 0;
    if (strcasecmp(s, "up") == 0 || strcmp(s, "1") == 0) return 1;
    if (strcasecmp(s, "down") == 0 || strcmp(s, "2") == 0) return 2;
    if (strcasecmp(s, "left") == 0 || strcmp(s, "3") == 0) return 3;
    if (strcasecmp(s, "right") == 0 || strcmp(s, "4") == 0) return 4;
    return 0;
}

static void handle_client(int cfd) {
    char req[1024];
    int n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(cfd);
        return;
    }
    req[n] = '\0';

    /* Parse HTTP GET line */
    char method[16], url[256];
    if (sscanf(req, "%15s %255s", method, url) < 2) {
        close(cfd);
        return;
    }

    int dir = 0;
    int steps = 50;
    int is_stop = 0;
    int is_status = 0;

    /* Check shorthand paths: /up, /down, /left, /right, /stop, /status */
    if (strncmp(url, "/up", 3) == 0) dir = 1;
    else if (strncmp(url, "/down", 5) == 0) dir = 2;
    else if (strncmp(url, "/left", 5) == 0) dir = 3;
    else if (strncmp(url, "/right", 6) == 0) dir = 4;
    else if (strncmp(url, "/stop", 5) == 0) is_stop = 1;
    else if (strncmp(url, "/status", 7) == 0) is_status = 1;

    /* Parse query string: ?dir=...&steps=... */
    char *q = strchr(url, '?');
    if (q) {
        *q++ = '\0';
        char *token = strtok(q, "&");
        while (token) {
            if (strncmp(token, "dir=", 4) == 0) {
                dir = parse_dir(token + 4);
                if (strcasecmp(token + 4, "stop") == 0) is_stop = 1;
            } else if (strncmp(token, "steps=", 6) == 0) {
                int s = atoi(token + 6);
                if (s > 0) steps = s;
            }
            token = strtok(NULL, "&");
        }
    }

    char body[512];
    int code = 200;

    if (is_status) {
        int st[8] = {0};
        do_status(st);
        snprintf(body, sizeof(body),
                 "{\"status\":\"ok\",\"state\":[%d,%d,%d,%d,%d,%d,%d,%d]}\n",
                 st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
    } else if (is_stop) {
        do_stop();
        snprintf(body, sizeof(body), "{\"status\":\"ok\",\"action\":\"stopped\"}\n");
    } else if (dir >= 1 && dir <= 4) {
        if (steps > 500) steps = 500; /* safety limit */
        do_move(dir, steps, 10);
        const char *name = (dir == 1) ? "up" : (dir == 2) ? "down" : (dir == 3) ? "left" : "right";
        snprintf(body, sizeof(body),
                 "{\"status\":\"ok\",\"dir\":\"%s\",\"dir_id\":%d,\"steps\":%d}\n",
                 name, dir, steps);
    } else {
        code = 400;
        snprintf(body, sizeof(body),
                 "{\"error\":\"unknown command\",\"usage\":\"/left, /right, /up, /down, /stop, /status or /ptz?dir=left&steps=50\"}\n");
    }

    char resp[1024];
    int len = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      code, (code == 200) ? "OK" : "Bad Request", strlen(body), body);

    send(cfd, resp, len, 0);
    close(cfd);
}

static void run_daemon(int port) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    if (bind(sfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(sfd, 10) < 0) {
        perror("listen");
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);
    printf("PTZ HTTP Daemon listening on port %d...\n", port);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_client(cfd);
    }
    close(sfd);
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && (strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "--daemon") == 0)) {
        int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;
        run_daemon(port);
        return 0;
    }

    if (argc < 2) {
        printf("Usage:\n");
        printf("  CLI:    %s <up|down|left|right|stop|status> [steps]\n", argv[0]);
        printf("  Daemon: %s -d [port] (default %d)\n\n", argv[0], DEFAULT_PORT);
        printf("Examples:\n");
        printf("  %s left 50\n", argv[0]);
        printf("  %s right 50\n", argv[0]);
        printf("  %s up 30\n", argv[0]);
        printf("  %s down 30\n", argv[0]);
        printf("  %s -d 8080 &\n", argv[0]);
        return 1;
    }

    if (strcasecmp(argv[1], "status") == 0) {
        int st[8] = {0};
        if (do_status(st) == 0) {
            printf("Current motor state: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d [5]=%d [6]=%d [7]=%d\n",
                   st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
        }
        return 0;
    }

    if (strcasecmp(argv[1], "stop") == 0) {
        do_stop();
        printf("Motor stopped.\n");
        return 0;
    }

    int dir = parse_dir(argv[1]);
    if (!dir) {
        fprintf(stderr, "Invalid direction '%s'. Use up, down, left, right.\n", argv[1]);
        return 1;
    }

    int steps = (argc >= 3) ? atoi(argv[2]) : 50;
    printf("Moving %s (%d steps)...\n", argv[1], steps);
    if (do_move(dir, steps, 10) == 0) {
        printf("OK!\n");
    }

    return 0;
}
