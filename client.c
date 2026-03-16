/*
 * client.c — TCP Pub/Sub Client
 *
 * Usage: ./client <server_ip> <port> [reconnect_id]
 *
 * Interactive commands:
 *   SUBSCRIBE <topic>       Subscribe (wildcards supported, e.g. sport.*)
 *   UNSUBSCRIBE <topic>     Unsubscribe from a topic
 *   PUBLISH <topic> <msg>   Publish a message to a topic
 *   TOPICS                  List all topics with subscriber counts
 *   QUIT / EXIT             Disconnect
 *
 * If a previous session ID is given as the third argument, the client
 * automatically sends RECONNECT to restore that session.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_PAYLOAD 60000
#define BUF_INIT    4096
#define PORT        9000

/* ================================================================
 * Receive buffer state
 * ================================================================ */
static char  *rbuf = NULL;
static size_t rlen = 0;
static size_t rcap = 0;

/* Session ID assigned by the server (printed on disconnect for reconnection) */
static uint32_t my_id = 0;

/* ================================================================
 * Framing helpers — identical wire format to the server:
 *   [4-byte big-endian payload length][payload bytes]
 * ================================================================ */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p  += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_framed(int fd, const char *data, size_t len)
{
    uint32_t net_len = htonl((uint32_t)len);
    if (send_all(fd, &net_len, 4) < 0) return -1;
    if (len > 0 && send_all(fd, data, len) < 0) return -1;
    return 0;
}

static int rbuf_grow(size_t need)
{
    size_t required = rlen + need;
    if (required <= rcap) return 0;
    size_t newcap = rcap ? rcap : BUF_INIT;
    while (newcap < required) newcap *= 2;
    char *tmp = realloc(rbuf, newcap);
    if (!tmp) return -1;
    rbuf = tmp;
    rcap = newcap;
    return 0;
}

/* ================================================================
 * Frame extraction — pull complete messages from the receive buffer,
 * print them, and shift the buffer forward.
 * ================================================================ */

static int process_recv(void)
{
    while (rlen >= 4) {
        uint32_t plen;
        memcpy(&plen, rbuf, 4);
        plen = ntohl(plen);
        if (plen > MAX_PAYLOAD) return -1;   /* protocol error */
        if (rlen < 4 + plen)   break;        /* incomplete frame */

        /* Print the payload */
        printf("%.*s\n", (int)plen, rbuf + 4);
        fflush(stdout);

        /* Extract session ID from WELCOME message */
        if (plen > 8 && memcmp(rbuf + 4, "WELCOME ", 8) == 0) {
            my_id = (uint32_t)strtoul(rbuf + 4 + 8, NULL, 10);
        }

        /* Advance past this frame */
        size_t consumed = 4 + plen;
        rlen -= consumed;
        if (rlen > 0)
            memmove(rbuf, rbuf + consumed, rlen);
    }
    return 0;
}

/* ================================================================
 * Main — connect and run the select() event loop
 * ================================================================ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip> [reconnect_id]\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = PORT;
    uint32_t reconnect_id = 0;
    if (argc >= 3)
        reconnect_id = (uint32_t)strtoul(argv[2], NULL, 10);

    signal(SIGPIPE, SIG_IGN);

    /* Create and connect socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", ip);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    printf("Connected to %s:%d\n", ip, port);
    printf("Commands: SUBSCRIBE <topic> | UNSUBSCRIBE <topic> | "
           "PUBLISH <topic> <msg> | TOPICS | QUIT\n");

    int need_reconnect = (reconnect_id > 0);

    printf("> ");
    fflush(stdout);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sockfd, &rfds);
        int maxfd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* ---- data from server ---- */
        if (FD_ISSET(sockfd, &rfds)) {
            if (rbuf_grow(4096) < 0) break;
            ssize_t n = recv(sockfd, rbuf + rlen, rcap - rlen, 0);
            if (n <= 0) {
                printf("\nServer disconnected.\n");
                if (my_id > 0)
                    printf("Your session ID was %u. "
                           "Reconnect within 60 s:  ./client %s %d %u\n",
                           my_id, ip, port, my_id);
                break;
            }
            rlen += (size_t)n;

            if (process_recv() < 0) {
                fprintf(stderr, "Protocol error\n");
                break;
            }

            /* After receiving WELCOME, send RECONNECT if the user
             * supplied a previous session ID on the command line. */
            if (need_reconnect && my_id > 0) {
                char cmd[64];
                int clen = snprintf(cmd, sizeof(cmd), "RECONNECT %u", reconnect_id);
                send_framed(sockfd, cmd, (size_t)clen);
                my_id = reconnect_id;   /* we'll be known by the old ID */
                need_reconnect = 0;
            }

            printf("> ");
            fflush(stdout);
        }

        /* ---- input from user ---- */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char line[MAX_PAYLOAD];
            if (!fgets(line, sizeof(line), stdin)) break;

            /* Strip trailing newline / carriage return */
            size_t llen = strlen(line);
            while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r'))
                line[--llen] = '\0';

            if (llen == 0) {
                printf("> ");
                fflush(stdout);
                continue;
            }

            /* Local commands */
            if (strcasecmp(line, "QUIT") == 0 || strcasecmp(line, "EXIT") == 0)
                break;

            /* Send the command as a framed message */
            if (send_framed(sockfd, line, llen) < 0) {
                fprintf(stderr, "Send failed\n");
                break;
            }
            /* Prompt will be reprinted after server response arrives */
        }
    }

    close(sockfd);
    free(rbuf);
    return 0;
}
