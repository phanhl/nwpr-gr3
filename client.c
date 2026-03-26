// client.c
// C2: Publish-Subscribe Notification Server (TCP)
// Message framing: delimiter-based (each message is one line terminated by \n)
//
// Build:
//   gcc -O2 -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L client.c -o client

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LINE 4096

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

//Mục đích: Thiết lập kết nối TCP đến server.
static int connect_tcp(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

//Mục đích: Gửi lệnh ID đến server nếu có client_id được cung cấp.
static void maybe_send_id(int fd, const char *id) {
    if (!id) return;
    char line[MAX_LINE];
    snprintf(line, sizeof(line), "ID %s\n", id);
    (void)send_all(fd, line, strlen(line));
}

//Mục đích: Vòng lặp chính client — đồng thời đọc stdin và socket.
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <host> <port> [client_id]\n", argv[0]);
        return 2;
    }

    int fd = connect_tcp(argv[1], argv[2]);
    if (fd < 0) die("connect");

    fprintf(stderr, "Connected. Examples:\n");
    fprintf(stderr, "  ID alice\n");
    fprintf(stderr, "  SUBSCRIBE sport\n");
    fprintf(stderr, "  SUBSCRIBE sport.*\n");
    fprintf(stderr, "  PUBLISH sport.football Goal!!!\n");
    fprintf(stderr, "  TOPICS\n");

    if (argc == 4) {
        maybe_send_id(fd, argv[3]);
    }

    // Use raw read() for stdin to avoid stdio buffering vs select() deadlock.
    // stdio's fgets() may internally read() more bytes than one line and buffer
    // them, causing select() to never report stdin as readable again even though
    // data is available in stdio's internal buffer => terminal hangs.
    char stdinbuf[MAX_LINE];
    size_t stdinlen = 0;

    char sockbuf[1024];
    char outbuf[MAX_LINE];
    size_t outlen = 0;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (fd > STDIN_FILENO) ? fd : STDIN_FILENO;

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            die("select");
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char tmp[1024];
            ssize_t nr = read(STDIN_FILENO, tmp, sizeof(tmp));
            if (nr <= 0) {
                fprintf(stderr, "EOF on stdin, exiting.\n");
                break;
            }

            for (ssize_t i = 0; i < nr; i++) {
                if (stdinlen + 1 >= sizeof(stdinbuf)) {
                    // Line too long, discard
                    stdinlen = 0;
                    continue;
                }
                stdinbuf[stdinlen++] = tmp[i];

                if (tmp[i] == '\n') {
                    // Complete line ready, send it
                    if (send_all(fd, stdinbuf, stdinlen) < 0) {
                        fprintf(stderr, "Send failed.\n");
                        goto done;
                    }
                    stdinlen = 0;
                }
            }
        }

        if (FD_ISSET(fd, &rfds)) {
            ssize_t n = recv(fd, sockbuf, sizeof(sockbuf), 0);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                fprintf(stderr, "recv error: %s\n", strerror(errno));
                break;
            }
            if (n == 0) {
                fprintf(stderr, "Server closed connection.\n");
                break;
            }

            // Print complete lines
            for (ssize_t i = 0; i < n; i++) {
                if (outlen + 1 >= sizeof(outbuf)) {
                    fwrite(outbuf, 1, outlen, stdout);
                    outlen = 0;
                }

                outbuf[outlen++] = sockbuf[i];
                if (sockbuf[i] == '\n') {
                    fwrite(outbuf, 1, outlen, stdout);
                    fflush(stdout);
                    outlen = 0;
                }
            }
        }
    }

done:
    close(fd);
    return 0;
}
