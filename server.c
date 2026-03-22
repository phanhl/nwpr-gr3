// server.c
// C2: Publish-Subscribe Notification Server (TCP)
// Message framing: delimiter-based (each message is one line terminated by \n)
//
// Core:
//   SUBSCRIBE <topic>
//   UNSUBSCRIBE <topic>
//   PUBLISH <topic> <message>
// Bonus:
//   Wildcard subscribe (prefix match): SUBSCRIBE sport.*
//   Persistent queue for offline subscribers (max 60s)
//   TOPICS (list topics + subscriber count)
//
// Build:
// gcc -O2 -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L pubsub_server_logged.c -o server

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE 4096
#define BACKLOG 64
#define OFFLINE_TTL_SEC 60

// ----------------- Data structures -----------------

typedef struct StrNode {
    char *s;
    struct StrNode *next;
} StrNode;

typedef struct Msg {
    char *line;              // already framed with \n
    time_t expire_at;        // message expires at this time
    struct Msg *next;
} Msg;

typedef struct Client {
    char *id;                // logical identity (for reconnect/offline queue). must be set by ID command
    int fd;                  // >=0 if online; -1 if offline
    int client_no;           // connection sequence number for logging: Client 1, Client 2, ...

    // Input buffer only used while online
    char inbuf[MAX_LINE];
    size_t inlen;

    // Subscriptions
    StrNode *exact_subs;     // exact topics: e.g. "sport"
    StrNode *wild_subs;      // wildcard prefixes (store prefix without '*'): e.g. "sport." for "sport.*"

    // Offline queue
    Msg *q_head;
    Msg *q_tail;

    struct Client *next;
} Client;

typedef struct Topic {
    char *name;
    time_t last_seen;
    struct Topic *next;
} Topic;

static Client *g_clients = NULL;      // list of all known clients (online + offline)
static Topic *g_topics = NULL;        // list of known topic names (created on subscribe/publish)
static int g_next_client_no = 1;

// ----------------- Utilities -----------------

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) die("malloc");
    memcpy(p, s, n + 1);
    return p;
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

static int send_line(int fd, const char *line) {
    return send_all(fd, line, strlen(line));
}

static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int strlist_contains(StrNode *n, const char *s) {
    for (; n; n = n->next) {
        if (strcmp(n->s, s) == 0) return 1;
    }
    return 0;
}

static void strlist_add_unique(StrNode **head, const char *s) {
    if (strlist_contains(*head, s)) return;
    StrNode *n = (StrNode *)calloc(1, sizeof(StrNode));
    if (!n) die("calloc");
    n->s = xstrdup(s);
    n->next = *head;
    *head = n;
}

static void strlist_remove(StrNode **head, const char *s) {
    StrNode **pp = head;
    while (*pp) {
        if (strcmp((*pp)->s, s) == 0) {
            StrNode *victim = *pp;
            *pp = victim->next;
            free(victim->s);
            free(victim);
            return;
        }
        pp = &((*pp)->next);
    }
}

static void strlist_free(StrNode *n) {
    while (n) {
        StrNode *next = n->next;
        free(n->s);
        free(n);
        n = next;
    }
}

static void queue_free(Msg *m) {
    while (m) {
        Msg *next = m->next;
        free(m->line);
        free(m);
        m = next;
    }
}

static void queue_push(Client *c, const char *line, time_t expire_at) {
    Msg *m = (Msg *)calloc(1, sizeof(Msg));
    if (!m) die("calloc");
    m->line = xstrdup(line);
    m->expire_at = expire_at;

    if (!c->q_tail) {
        c->q_head = c->q_tail = m;
    } else {
        c->q_tail->next = m;
        c->q_tail = m;
    }
}

static int queue_prune_expired(Client *c, time_t now) {
    int removed = 0;

    while (c->q_head && c->q_head->expire_at <= now) {
        Msg *victim = c->q_head;
        c->q_head = victim->next;
        if (!c->q_head) c->q_tail = NULL;
        free(victim->line);
        free(victim);
        removed++;
    }

    Msg *prev = c->q_head;
    if (!prev) return removed;

    Msg *cur = prev->next;
    while (cur) {
        if (cur->expire_at <= now) {
            prev->next = cur->next;
            if (c->q_tail == cur) c->q_tail = prev;
            free(cur->line);
            free(cur);
            cur = prev->next;
            removed++;
            continue;
        }
        prev = cur;
        cur = cur->next;
    }

    return removed;
}

static void global_prune_expired_queues(void) {
    time_t now = time(NULL);
    for (Client *c = g_clients; c; c = c->next) {
        (void)queue_prune_expired(c, now);
    }
}

static Topic *topic_find(const char *name) {
    for (Topic *t = g_topics; t; t = t->next) {
        if (strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

static void topic_touch(const char *name) {
    Topic *t = topic_find(name);
    if (t) {
        t->last_seen = time(NULL);
        return;
    }

    t = (Topic *)calloc(1, sizeof(Topic));
    if (!t) die("calloc");
    t->name = xstrdup(name);
    t->last_seen = time(NULL);
    t->next = g_topics;
    g_topics = t;
}

static Client *client_find_by_fd(int fd) {
    for (Client *c = g_clients; c; c = c->next) {
        if (c->fd == fd) return c;
    }
    return NULL;
}

static Client *client_find_by_id(const char *id) {
    for (Client *c = g_clients; c; c = c->next) {
        if (c->id && strcmp(c->id, id) == 0) return c;
    }
    return NULL;
}

static int is_valid_id(const char *id) {
    if (!id) return 0;
    size_t n = strlen(id);
    if (n == 0 || n > 32) return 0;

    for (size_t i = 0; i < n; i++) {
        char ch = id[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '.') {
            continue;
        }
        return 0;
    }
    return 1;
}

static Client *client_create_unidentified(int fd) {
    Client *c = (Client *)calloc(1, sizeof(Client));
    if (!c) die("calloc");
    c->fd = fd;
    c->client_no = g_next_client_no++;
    c->next = g_clients;
    g_clients = c;
    return c;
}

static void client_mark_offline(Client *c) {
    if (!c) return;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->inlen = 0;
    c->inbuf[0] = '\0';
}

static void client_destroy(Client *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->id);
    strlist_free(c->exact_subs);
    strlist_free(c->wild_subs);
    queue_free(c->q_head);
    free(c);
}

static void client_remove_from_global(Client *victim) {
    Client **pp = &g_clients;
    while (*pp) {
        if (*pp == victim) {
            *pp = victim->next;
            client_destroy(victim);
            return;
        }
        pp = &((*pp)->next);
    }
}

static int wildcard_to_prefix(const char *topic_or_wild, char *out_prefix, size_t out_sz) {
    const char *star = strchr(topic_or_wild, '*');
    if (!star) return 0;
    if (star[1] != '\0') return -1;

    size_t prefix_len = (size_t)(star - topic_or_wild);
    if (prefix_len == 0) return -1;
    if (prefix_len + 1 > out_sz) return -1;

    memcpy(out_prefix, topic_or_wild, prefix_len);
    out_prefix[prefix_len] = '\0';
    return 1;
}

static int client_matches_topic(const Client *c, const char *topic) {
    if (strlist_contains(c->exact_subs, topic)) return 1;

    for (StrNode *n = c->wild_subs; n; n = n->next) {
        size_t plen = strlen(n->s);
        if (strncmp(topic, n->s, plen) == 0) return 1;
    }

    return 0;
}

static int topic_subscriber_count(const char *topic) {
    int cnt = 0;
    for (Client *c = g_clients; c; c = c->next) {
        if (!c->id) continue;
        if (client_matches_topic(c, topic)) cnt++;
    }
    return cnt;
}

// ----------------- Command handlers -----------------

static void cmd_help(int fd) {
    send_line(fd, "OK Commands:\n");
    send_line(fd, " ID <client_id>\n");
    send_line(fd, " SUBSCRIBE <topic>\n");
    send_line(fd, " SUBSCRIBE <prefix*> (example: sport.*)\n");
    send_line(fd, " UNSUBSCRIBE <topic|prefix*>\n");
    send_line(fd, " PUBLISH <topic> <message>\n");
    send_line(fd, " TOPICS\n");
}

static void cmd_id(int fd, const char *id) {
    if (!id || !is_valid_id(id)) {
        send_line(fd, "ERR Usage: ID <client_id> (1..32 chars: a-zA-Z0-9_.-)\n");
        return;
    }

    Client *conn = client_find_by_fd(fd);
    if (!conn) return;

    if (conn->id) {
        send_line(fd, "ERR ID already set\n");
        return;
    }

    Client *existing = client_find_by_id(id);
    if (existing) {
        if (existing->fd >= 0) {
            send_line(fd, "ERR This ID is already online\n");
            return;
        }

        existing->fd = fd;
        existing->inlen = 0;
        existing->inbuf[0] = '\0';

        client_remove_from_global(conn);

        global_prune_expired_queues();
        int delivered = 0;
        for (Msg *m = existing->q_head; m; m = m->next) {
            if (send_line(fd, m->line) == 0) delivered++;
        }
        queue_free(existing->q_head);
        existing->q_head = existing->q_tail = NULL;

        char out[MAX_LINE];
        snprintf(out, sizeof(out), "OK Reconnected as %s. Delivered %d queued messages\n", id, delivered);
        send_line(fd, out);
        return;
    }

    conn->id = xstrdup(id);
    char out[MAX_LINE];
    snprintf(out, sizeof(out), "OK Identified as %s\n", id);
    send_line(fd, out);
}

static void cmd_subscribe(int fd, const char *topic_or_wild) {
    if (!topic_or_wild || topic_or_wild[0] == '\0') {
        send_line(fd, "ERR Usage: SUBSCRIBE <topic>\n");
        return;
    }

    Client *c = client_find_by_fd(fd);
    if (!c) return;
    if (!c->id) {
        send_line(fd, "ERR Please set ID first: ID <client_id>\n");
        return;
    }

    char prefix[256];
    int w = wildcard_to_prefix(topic_or_wild, prefix, sizeof(prefix));
    if (w == -1) {
        send_line(fd, "ERR Invalid wildcard. Only suffix '*' is supported, e.g. sport.*\n");
        return;
    }
    if (w == 1) {
        strlist_add_unique(&c->wild_subs, prefix);

        char out[MAX_LINE];
        snprintf(out, sizeof(out), "OK Subscribed (wildcard) to '%s'\n", topic_or_wild);
        send_line(fd, out);

        printf("[server] Client %d subscribed to '%s'\n", c->client_no, topic_or_wild);
        fflush(stdout);
        return;
    }

    strlist_add_unique(&c->exact_subs, topic_or_wild);
    topic_touch(topic_or_wild);

    int cnt = topic_subscriber_count(topic_or_wild);
    char out[MAX_LINE];
    snprintf(out, sizeof(out), "OK Subscribed to '%s'. Subscribers: %d\n", topic_or_wild, cnt);
    send_line(fd, out);

    printf("[server] Client %d subscribed to '%s'\n", c->client_no, topic_or_wild);
    fflush(stdout);
}

static void cmd_unsubscribe(int fd, const char *topic_or_wild) {
    if (!topic_or_wild || topic_or_wild[0] == '\0') {
        send_line(fd, "ERR Usage: UNSUBSCRIBE <topic>\n");
        return;
    }

    Client *c = client_find_by_fd(fd);
    if (!c) return;
    if (!c->id) {
        send_line(fd, "ERR Please set ID first: ID <client_id>\n");
        return;
    }

    char prefix[256];
    int w = wildcard_to_prefix(topic_or_wild, prefix, sizeof(prefix));
    if (w == -1) {
        send_line(fd, "ERR Invalid wildcard. Only suffix '*' is supported, e.g. sport.*\n");
        return;
    }
    if (w == 1) {
        strlist_remove(&c->wild_subs, prefix);
        char out[MAX_LINE];
        snprintf(out, sizeof(out), "OK Unsubscribed (wildcard) from '%s'\n", topic_or_wild);
        send_line(fd, out);
        return;
    }

    strlist_remove(&c->exact_subs, topic_or_wild);
    char out[MAX_LINE];
    snprintf(out, sizeof(out), "OK Unsubscribed from '%s'\n", topic_or_wild);
    send_line(fd, out);
}

static void cmd_publish(int fd, const char *topic, const char *message) {
    if (!topic || topic[0] == '\0' || !message || message[0] == '\0') {
        send_line(fd, "ERR Usage: PUBLISH <topic> <message>\n");
        return;
    }

    Client *pub = client_find_by_fd(fd);
    if (!pub) return;
    if (!pub->id) {
        send_line(fd, "ERR Please set ID first: ID <client_id>\n");
        return;
    }

    global_prune_expired_queues();
    topic_touch(topic);

    char payload[MAX_LINE];
    snprintf(payload, sizeof(payload), "[%s] %s\n", topic, message);

    int delivered = 0;
    time_t now = time(NULL);

    for (Client *c = g_clients; c; c = c->next) {
        if (!c->id) continue;
        if (strcmp(c->id, pub->id) == 0) continue;
        if (!client_matches_topic(c, topic)) continue;

        if (c->fd >= 0) {
            if (send_line(c->fd, payload) == 0) {
                delivered++;
            } else {
                client_mark_offline(c);
                queue_push(c, payload, now + OFFLINE_TTL_SEC);
                delivered++;
            }
        } else {
            queue_push(c, payload, now + OFFLINE_TTL_SEC);
            delivered++;
        }
    }

    char out[MAX_LINE];
    snprintf(out, sizeof(out), "Delivered to %d subscribers\n", delivered);
    send_line(fd, out);

    printf("[server] Client %d published to '%s' -> %d subscribers\n",
           pub->client_no, topic, delivered);
    fflush(stdout);
}

static void cmd_topics(int fd) {
    Client *c = client_find_by_fd(fd);
    if (!c) return;
    if (!c->id) {
        send_line(fd, "ERR Please set ID first: ID <client_id>\n");
        return;
    }

    global_prune_expired_queues();

    int k = 0;
    for (Topic *t = g_topics; t; t = t->next) k++;

    char line[MAX_LINE];
    snprintf(line, sizeof(line), "TOPICS %d\n", k);
    send_line(fd, line);

    for (Topic *t = g_topics; t; t = t->next) {
        int cnt = topic_subscriber_count(t->name);
        snprintf(line, sizeof(line), "%s %d\n", t->name, cnt);
        send_line(fd, line);
    }
    send_line(fd, "END\n");
}

// ----------------- Parsing & server loop -----------------

static void handle_line(int fd, char *line) {
    trim_crlf(line);
    if (line[0] == '\0') return;

    char *cmd = strtok(line, " \t");
    if (!cmd) return;

    if (strcmp(cmd, "HELP") == 0) {
        cmd_help(fd);
        return;
    }

    if (strcmp(cmd, "ID") == 0) {
        char *id = strtok(NULL, " \t");
        cmd_id(fd, id);
        return;
    }

    if (strcmp(cmd, "SUBSCRIBE") == 0) {
        char *topic = strtok(NULL, " \t");
        cmd_subscribe(fd, topic);
        return;
    }

    if (strcmp(cmd, "UNSUBSCRIBE") == 0) {
        char *topic = strtok(NULL, " \t");
        cmd_unsubscribe(fd, topic);
        return;
    }

    if (strcmp(cmd, "PUBLISH") == 0) {
        char *topic = strtok(NULL, " \t");
        char *msg = strtok(NULL, "");
        if (msg) {
            while (*msg == ' ' || *msg == '\t') msg++;
        }
        cmd_publish(fd, topic, msg);
        return;
    }

    if (strcmp(cmd, "TOPICS") == 0) {
        cmd_topics(fd);
        return;
    }

    send_line(fd, "ERR Unknown command. Type HELP\n");
}

static int accept_client(int listen_fd) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int cfd = accept(listen_fd, (struct sockaddr *)&addr, &alen);
    if (cfd < 0) return -1;

    Client *c = client_create_unidentified(cfd);

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

    printf("[server] Client %d connected from %s:%d (fd %d)\n",
           c->client_no, ip, ntohs(addr.sin_port), cfd);
    fflush(stdout);

    send_line(cfd, "OK Connected. Please identify: ID <client_id>\n");
    return cfd;
}

static void on_disconnect_fd(int fd) {
    Client *c = client_find_by_fd(fd);
    if (!c) {
        close(fd);
        return;
    }

    printf("[server] Client %d disconnected (fd %d)\n", c->client_no, fd);
    fflush(stdout);

    if (!c->id) {
        client_remove_from_global(c);
        return;
    }

    client_mark_offline(c);
}

static void read_from_client(int fd) {
    Client *c = client_find_by_fd(fd);
    if (!c) return;

    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return;
        fprintf(stderr, "recv error fd=%d: %s\n", fd, strerror(errno));
        on_disconnect_fd(fd);
        return;
    }
    if (n == 0) {
        on_disconnect_fd(fd);
        return;
    }

    for (ssize_t i = 0; i < n; i++) {
        if (c->inlen + 1 >= sizeof(c->inbuf)) {
            c->inlen = 0;
            c->inbuf[0] = '\0';
            send_line(fd, "ERR Line too long\n");
            continue;
        }

        c->inbuf[c->inlen++] = buf[i];
        c->inbuf[c->inlen] = '\0';

        if (buf[i] == '\n') {
            char line[MAX_LINE];
            size_t linelen = c->inlen;
            if (linelen >= sizeof(line)) linelen = sizeof(line) - 1;
            memcpy(line, c->inbuf, linelen);
            line[linelen] = '\0';

            c->inlen = 0;
            c->inbuf[0] = '\0';

            handle_line(fd, line);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 2;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 2;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket");

    int yes = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        die("setsockopt");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(listen_fd, BACKLOG) < 0) die("listen");

    printf("=== Pub/Sub Server listening on port %d ===\n", port);
    fflush(stdout);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        for (Client *c = g_clients; c; c = c->next) {
            if (c->fd >= 0) {
                FD_SET(c->fd, &rfds);
                if (c->fd > maxfd) maxfd = c->fd;
            }
        }

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            die("select");
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            (void)accept_client(listen_fd);
        }

        int ready_fds[FD_SETSIZE];
        int ready_n = 0;
        for (Client *c = g_clients; c; c = c->next) {
            if (c->fd >= 0 && FD_ISSET(c->fd, &rfds)) {
                ready_fds[ready_n++] = c->fd;
            }
        }

        for (int i = 0; i < ready_n; i++) {
            read_from_client(ready_fds[i]);
        }
    }

    return 0;
}
