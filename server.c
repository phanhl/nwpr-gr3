/*
 * server.c — TCP Pub/Sub Server
 *
 * Usage: ./server <port>
 *
 * Features:
 *   - Length-prefixed message framing [4-byte BE length][payload]
 *   - select()-based I/O multiplexing (no threads)
 *   - Wildcard subscriptions via fnmatch (e.g. sport.*)
 *   - Persistent message queue for disconnected clients (60 s)
 *   - SUBSCRIBE / UNSUBSCRIBE / PUBLISH / TOPICS / RECONNECT commands
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fnmatch.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ================================================================
 * Tunables
 * ================================================================ */
#define MAX_CLIENTS   128   /* max concurrent connections           */
#define MAX_SUBS       64   /* max subscriptions per client         */
#define MAX_TOPICS   1024   /* max tracked topic names              */
#define BUF_INIT     4096   /* initial recv-buffer size             */
#define MAX_PAYLOAD 60000   /* max single message payload           */
#define PERSIST_SEC    60   /* seconds to keep disconnected session */
#define PORT         9000   /* fixed listening port                 */

/* ================================================================
 * Data structures
 * ================================================================ */

/* Queued message for a disconnected client */
typedef struct pending_msg {
    char               *data;   /* formatted "[topic] message" string */
    time_t              ts;     /* publish timestamp                  */
    struct pending_msg  *next;
} PendingMsg;

/* A connected client */
typedef struct {
    int       fd;               /* socket fd (-1 if slot unused) */
    int       active;           /* 1 = slot in use               */
    uint32_t  id;               /* unique server-assigned ID     */
    /* Receive buffer — accumulates bytes for frame extraction */
    char     *rbuf;
    size_t    rlen;             /* bytes currently buffered */
    size_t    rcap;             /* allocated capacity       */
    /* Subscription patterns (may contain fnmatch wildcards) */
    char     *subs[MAX_SUBS];
    int       nsubs;
} Client;

/* A disconnected client whose session is preserved temporarily */
typedef struct disc_client {
    uint32_t             id;
    char                *subs[MAX_SUBS];
    int                  nsubs;
    time_t               disc_time;
    PendingMsg          *pending;
    struct disc_client  *next;
} DiscClient;

/* Top-level server state */
typedef struct {
    int          listen_fd;
    Client       clients[MAX_CLIENTS];
    uint32_t     next_id;           /* monotonic ID counter        */
    DiscClient  *disc_list;         /* linked list of saved sessions */
    char        *topics[MAX_TOPICS]; /* known topic names           */
    int          ntopics;
} Server;

/* ================================================================
 * Framing helpers
 *
 * Wire format: [4-byte big-endian payload length][payload bytes]
 * All send/recv helpers handle partial transfers.
 * ================================================================ */

/* Send exactly `len` bytes.  Returns 0 on success, -1 on error. */
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

/* Send a single length-prefixed frame. */
static int send_framed(int fd, const char *data, size_t len)
{
    uint32_t net_len = htonl((uint32_t)len);
    if (send_all(fd, &net_len, 4) < 0) return -1;
    if (len > 0 && send_all(fd, data, len) < 0) return -1;
    return 0;
}

/* Convenience: send a printf-formatted string as a framed message. */
static int send_fmt(int fd, const char *fmt, ...)
{
    char buf[MAX_PAYLOAD];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    return send_framed(fd, buf, (size_t)n);
}

/* Grow the client's receive buffer so it can hold `need` more bytes. */
static int rbuf_grow(Client *c, size_t need)
{
    size_t required = c->rlen + need;
    if (required <= c->rcap) return 0;
    size_t newcap = c->rcap ? c->rcap : BUF_INIT;
    while (newcap < required) newcap *= 2;
    char *tmp = realloc(c->rbuf, newcap);
    if (!tmp) return -1;
    c->rbuf = tmp;
    c->rcap = newcap;
    return 0;
}

/* ================================================================
 * Topic tracking (for the TOPICS command)
 * ================================================================ */

/* Register a concrete topic name (idempotent). */
static void track_topic(Server *srv, const char *topic)
{
    for (int i = 0; i < srv->ntopics; i++)
        if (strcmp(srv->topics[i], topic) == 0) return;
    if (srv->ntopics < MAX_TOPICS)
        srv->topics[srv->ntopics++] = strdup(topic);
}

/* Count active subscribers whose patterns match `topic`.
 * `exclude_fd` is skipped (use -1 to count everyone). */
static int count_subscribers(Server *srv, const char *topic, int exclude_fd)
{
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &srv->clients[i];
        if (!c->active || c->fd == exclude_fd) continue;
        for (int j = 0; j < c->nsubs; j++) {
            if (fnmatch(c->subs[j], topic, 0) == 0) {
                count++;
                break;  /* count each client at most once */
            }
        }
    }
    return count;
}

/* ================================================================
 * Disconnected-client management (persistent queue)
 *
 * When a subscribed client disconnects we save their subscriptions
 * and buffer any messages published to matching topics for up to
 * PERSIST_SEC seconds.  A RECONNECT command restores the session.
 * ================================================================ */

static void free_pending_list(PendingMsg *p)
{
    while (p) {
        PendingMsg *next = p->next;
        free(p->data);
        free(p);
        p = next;
    }
}

static void free_disc(DiscClient *d)
{
    for (int i = 0; i < d->nsubs; i++) free(d->subs[i]);
    free_pending_list(d->pending);
    free(d);
}

/* Save a client's subscriptions for later reconnection. */
static void disc_save(Server *srv, Client *c)
{
    if (c->nsubs == 0) return;  /* nothing worth saving */

    DiscClient *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->id        = c->id;
    d->disc_time = time(NULL);
    d->nsubs     = c->nsubs;
    for (int i = 0; i < c->nsubs; i++)
        d->subs[i] = strdup(c->subs[i]);

    d->next       = srv->disc_list;
    srv->disc_list = d;
}

/* Queue a formatted message for every disconnected client that matches `topic`. */
static void disc_queue_msg(Server *srv, const char *topic, const char *formatted)
{
    for (DiscClient *d = srv->disc_list; d; d = d->next) {
        for (int i = 0; i < d->nsubs; i++) {
            if (fnmatch(d->subs[i], topic, 0) == 0) {
                PendingMsg *pm = malloc(sizeof(*pm));
                if (!pm) break;
                pm->data = strdup(formatted);
                pm->ts   = time(NULL);
                pm->next = NULL;
                /* append at tail */
                PendingMsg **tail = &d->pending;
                while (*tail) tail = &(*tail)->next;
                *tail = pm;
                break;  /* one copy per disconnected client */
            }
        }
    }
}

/* Expire sessions older than PERSIST_SEC. */
static void disc_expire(Server *srv)
{
    time_t now = time(NULL);
    DiscClient **pp = &srv->disc_list;
    while (*pp) {
        if (now - (*pp)->disc_time > PERSIST_SEC) {
            DiscClient *d = *pp;
            *pp = d->next;
            printf("[server] Expired saved session for client %u\n", d->id);
            free_disc(d);
        } else {
            pp = &(*pp)->next;
        }
    }
}

/* Remove and return a saved session by ID (caller must free). */
static DiscClient *disc_take(Server *srv, uint32_t id)
{
    DiscClient **pp = &srv->disc_list;
    while (*pp) {
        if ((*pp)->id == id) {
            DiscClient *d = *pp;
            *pp = d->next;
            d->next = NULL;
            return d;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* ================================================================
 * Client management
 * ================================================================ */

static Client *client_add(Server *srv, int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!srv->clients[i].active) {
            Client *c = &srv->clients[i];
            memset(c, 0, sizeof(*c));
            c->fd     = fd;
            c->active = 1;
            c->id     = srv->next_id++;
            return c;
        }
    }
    return NULL;  /* server full */
}

static void client_remove(Server *srv, Client *c)
{
    printf("[server] Client %u (fd %d) disconnected\n", c->id, c->fd);
    disc_save(srv, c);          /* preserve session for reconnection */
    close(c->fd);
    for (int i = 0; i < c->nsubs; i++) free(c->subs[i]);
    free(c->rbuf);
    memset(c, 0, sizeof(*c));   /* clears active flag, fd, etc. */
}

/* ================================================================
 * Command handlers
 * ================================================================ */

static void cmd_subscribe(Server *srv, Client *c, const char *pattern)
{
    /* Reject duplicates */
    for (int i = 0; i < c->nsubs; i++) {
        if (strcmp(c->subs[i], pattern) == 0) {
            send_fmt(c->fd, "Already subscribed to %s", pattern);
            return;
        }
    }
    if (c->nsubs >= MAX_SUBS) {
        send_fmt(c->fd, "ERROR: subscription limit reached (%d)", MAX_SUBS);
        return;
    }
    c->subs[c->nsubs++] = strdup(pattern);

    /* Track concrete (non-wildcard) topics for the TOPICS command */
    if (!strchr(pattern, '*') && !strchr(pattern, '?') && !strchr(pattern, '['))
        track_topic(srv, pattern);

    send_fmt(c->fd, "OK Subscribed to %s", pattern);
    printf("[server] Client %u subscribed to '%s'\n", c->id, pattern);
}

static void cmd_unsubscribe(Server *srv, Client *c, const char *pattern)
{
    (void)srv;
    for (int i = 0; i < c->nsubs; i++) {
        if (strcmp(c->subs[i], pattern) == 0) {
            free(c->subs[i]);
            c->subs[i] = c->subs[--c->nsubs];
            c->subs[c->nsubs] = NULL;
            send_fmt(c->fd, "OK Unsubscribed from %s", pattern);
            printf("[server] Client %u unsubscribed from '%s'\n", c->id, pattern);
            return;
        }
    }
    send_fmt(c->fd, "ERROR: not subscribed to %s", pattern);
}

static void cmd_publish(Server *srv, Client *c, const char *topic, const char *message)
{
    track_topic(srv, topic);

    /* Format the delivery payload: "[topic] message" */
    char delivery[MAX_PAYLOAD];
    int dlen = snprintf(delivery, sizeof(delivery), "[%s] %s", topic, message);
    if (dlen < 0 || (size_t)dlen >= sizeof(delivery)) {
        send_fmt(c->fd, "ERROR: message too long");
        return;
    }

    /* Deliver to every active subscriber whose pattern matches,
     * but skip the publisher so they don't receive their own message. */
    int delivered = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *sub = &srv->clients[i];
        if (!sub->active || sub->fd == c->fd) continue;
        for (int j = 0; j < sub->nsubs; j++) {
            if (fnmatch(sub->subs[j], topic, 0) == 0) {
                if (send_framed(sub->fd, delivery, (size_t)dlen) == 0)
                    delivered++;
                break;  /* deliver once per subscriber */
            }
        }
    }

    /* Also queue for disconnected clients (persistent queue) */
    disc_queue_msg(srv, topic, delivery);

    send_fmt(c->fd, "Delivered to %d subscribers", delivered);
    printf("[server] Client %u published to '%s' -> %d subscribers\n",
           c->id, topic, delivered);
}

static void cmd_topics(Server *srv, Client *c)
{
    if (srv->ntopics == 0) {
        send_fmt(c->fd, "No active topics");
        return;
    }
    char buf[MAX_PAYLOAD];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "Topics:");
    for (int i = 0; i < srv->ntopics && off < (int)sizeof(buf) - 128; i++) {
        int cnt = count_subscribers(srv, srv->topics[i], -1);
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "\n  %s (%d subscribers)", srv->topics[i], cnt);
    }
    send_framed(c->fd, buf, (size_t)off);
}

static void cmd_reconnect(Server *srv, Client *c, uint32_t old_id)
{
    DiscClient *d = disc_take(srv, old_id);
    if (!d) {
        send_fmt(c->fd, "ERROR: no saved session for ID %u", old_id);
        return;
    }

    /* Restore old ID and subscriptions */
    c->id = d->id;
    for (int i = 0; i < d->nsubs && c->nsubs < MAX_SUBS; i++)
        c->subs[c->nsubs++] = strdup(d->subs[i]);

    /* Count pending messages */
    int pcount = 0;
    for (PendingMsg *p = d->pending; p; p = p->next) pcount++;

    send_fmt(c->fd, "OK Reconnected as %u (%d subscriptions, %d pending messages)",
             c->id, c->nsubs, pcount);
    printf("[server] Client reconnected as %u, delivering %d pending messages\n",
           c->id, pcount);

    /* Deliver all pending messages */
    for (PendingMsg *p = d->pending; p; p = p->next)
        send_framed(c->fd, p->data, strlen(p->data));

    free_disc(d);
}

/* ================================================================
 * Message dispatch
 *
 * Called once per complete frame extracted from a client's buffer.
 * The `msg` buffer is null-terminated.
 * ================================================================ */

static void handle_message(Server *srv, Client *c, const char *msg)
{
    if (strncmp(msg, "SUBSCRIBE ", 10) == 0) {
        cmd_subscribe(srv, c, msg + 10);
    }
    else if (strncmp(msg, "UNSUBSCRIBE ", 12) == 0) {
        cmd_unsubscribe(srv, c, msg + 12);
    }
    else if (strncmp(msg, "PUBLISH ", 8) == 0) {
        /* Split "PUBLISH <topic> <message>" at the first space after PUBLISH */
        const char *rest = msg + 8;
        const char *sp   = strchr(rest, ' ');
        if (!sp || sp == rest) {
            send_fmt(c->fd, "ERROR: usage: PUBLISH <topic> <message>");
            return;
        }
        /* We need a mutable copy of topic (space -> NUL) */
        size_t tlen = (size_t)(sp - rest);
        char topic[MAX_PAYLOAD];
        if (tlen >= sizeof(topic)) {
            send_fmt(c->fd, "ERROR: topic too long");
            return;
        }
        memcpy(topic, rest, tlen);
        topic[tlen] = '\0';
        cmd_publish(srv, c, topic, sp + 1);
    }
    else if (strcmp(msg, "TOPICS") == 0) {
        cmd_topics(srv, c);
    }
    else if (strncmp(msg, "RECONNECT ", 10) == 0) {
        uint32_t old_id = (uint32_t)strtoul(msg + 10, NULL, 10);
        cmd_reconnect(srv, c, old_id);
    }
    else {
        send_fmt(c->fd, "ERROR: unknown command. "
                 "Use SUBSCRIBE, UNSUBSCRIBE, PUBLISH, TOPICS, or RECONNECT");
    }
}

/* ================================================================
 * Frame extraction
 *
 * Pulls complete [4-byte len][payload] frames out of a client's
 * receive buffer and dispatches them.  Returns -1 on protocol error.
 * ================================================================ */

static int process_frames(Server *srv, Client *c)
{
    while (c->rlen >= 4) {
        uint32_t payload_len;
        memcpy(&payload_len, c->rbuf, 4);
        payload_len = ntohl(payload_len);

        if (payload_len > MAX_PAYLOAD) return -1;           /* protocol violation */
        if (c->rlen < 4 + payload_len) break;               /* incomplete frame  */

        /* Copy payload so we can null-terminate safely */
        char *copy = malloc(payload_len + 1);
        if (!copy) return -1;
        memcpy(copy, c->rbuf + 4, payload_len);
        copy[payload_len] = '\0';

        handle_message(srv, c, copy);
        free(copy);

        /* Shift remaining bytes to front of buffer */
        size_t consumed = 4 + payload_len;
        c->rlen -= consumed;
        if (c->rlen > 0)
            memmove(c->rbuf, c->rbuf + consumed, c->rlen);
    }
    return 0;
}

/* ================================================================
 * Main — server setup and select() event loop
 * ================================================================ */

int main(void)
{
    int port = PORT;

    /* Ignore SIGPIPE so failed sends return -1 instead of killing us */
    signal(SIGPIPE, SIG_IGN);

    Server srv;
    memset(&srv, 0, sizeof(srv));
    srv.next_id = 1;

    /* Create listen socket */
    srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv.listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srv.listen_fd, 16) < 0) {
        perror("listen"); return 1;
    }

    printf("=== Pub/Sub Server listening on port %d ===\n", port);

    /* ---- main event loop ---- */
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv.listen_fd, &rfds);
        int maxfd = srv.listen_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (srv.clients[i].active) {
                FD_SET(srv.clients[i].fd, &rfds);
                if (srv.clients[i].fd > maxfd)
                    maxfd = srv.clients[i].fd;
            }
        }

        /* 1-second timeout so we can periodically expire saved sessions */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int nready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (nready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Periodic maintenance: expire stale disconnected sessions */
        disc_expire(&srv);

        /* ---- accept new connections ---- */
        if (FD_ISSET(srv.listen_fd, &rfds)) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int newfd = accept(srv.listen_fd, (struct sockaddr *)&peer, &plen);
            if (newfd >= 0) {
                Client *c = client_add(&srv, newfd);
                if (c) {
                    printf("[server] Client %u connected from %s:%d (fd %d)\n",
                           c->id, inet_ntoa(peer.sin_addr),
                           ntohs(peer.sin_port), newfd);
                    /* Tell the client its session ID (needed for RECONNECT) */
                    send_fmt(newfd, "WELCOME %u", c->id);
                } else {
                    fprintf(stderr, "[server] Connection rejected — server full\n");
                    close(newfd);
                }
            }
        }

        /* ---- read from connected clients ---- */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &srv.clients[i];
            if (!c->active || !FD_ISSET(c->fd, &rfds)) continue;

            /* Ensure we have room to receive */
            if (rbuf_grow(c, 4096) < 0) {
                client_remove(&srv, c);
                continue;
            }

            ssize_t n = recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen, 0);
            if (n <= 0) {
                /* Client closed connection or error */
                client_remove(&srv, c);
                continue;
            }
            c->rlen += (size_t)n;

            /* Extract and handle complete frames */
            if (process_frames(&srv, c) < 0) {
                printf("[server] Protocol error from client %u — disconnecting\n", c->id);
                client_remove(&srv, c);
            }
        }
    }

    /* ---- cleanup (reached only on select error) ---- */
    close(srv.listen_fd);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (srv.clients[i].active) client_remove(&srv, &srv.clients[i]);
    for (int i = 0; i < srv.ntopics; i++)
        free(srv.topics[i]);
    while (srv.disc_list) {
        DiscClient *d = srv.disc_list;
        srv.disc_list = d->next;
        free_disc(d);
    }

    return 0;
}
