# TCP Pub/Sub System

A complete publish–subscribe messaging system in C using POSIX TCP sockets
on Linux. No threads — all concurrency is handled with `select()`.

## Build

```bash
make
# or manually:
gcc -Wall -Wextra -O2 server.c -o server
gcc -Wall -Wextra -O2 client.c -o client
```

## Network Setup

This system runs across **3 separate machines** connected in the same network:

| Machine | Role | IP |
|---|---|---|
| Machine 1 | Server | `<IP_Machine_1>` |
| Machine 2 | Client A (publisher) | `<IP_Machine_2>` |
| Machine 3 | Client B (subscriber) | `<IP_Machine_3>` |

> Copy the compiled `client` binary to Machine 2 and Machine 3,
> or compile `client.c` directly on each machine.

## Run

```bash
# Machine 1 — start the server (port 9000 is hardcoded)
./server

# Machine 2 — start Client A (connect to server's IP)
./client <IP_Machine_1>

# Machine 3 — start Client B (connect to server's IP)
./client <IP_Machine_1>
```

## Commands

| Command | Description |
|---|---|
| `SUBSCRIBE <topic>` | Subscribe to a topic (wildcards supported, e.g. `sport.*`) |
| `UNSUBSCRIBE <topic>` | Remove a subscription |
| `PUBLISH <topic> <message>` | Send a message to all subscribers of `<topic>` |
| `TOPICS` | List known topics with subscriber counts |
| `QUIT` / `EXIT` | Disconnect from the server |

## Example Interaction

```
=== Machine 1: Server ===
$ ./server
=== Pub/Sub Server listening on port 9000 ===
[server] Client 1 connected from <IP_Machine_2>:54320 (fd 4)
[server] Client 2 connected from <IP_Machine_3>:54321 (fd 5)
[server] Client 2 subscribed to 'news'
[server] Client 2 subscribed to 'sport.*'
[server] Client 1 published to 'news' -> 1 subscribers
[server] Client 1 published to 'sport.football' -> 1 subscribers
[server] Client 1 published to 'weather' -> 0 subscribers

=== Machine 2: Client A (publisher) ===
$ ./client <IP_Machine_1>
Connected to <IP_Machine_1>:9000
WELCOME 1
> PUBLISH news Breaking: system works!
Delivered to 1 subscribers
> PUBLISH sport.football Goal scored!
Delivered to 1 subscribers
> PUBLISH weather Sunny
Delivered to 0 subscribers
> TOPICS
Topics:
  news (1 subscribers)
  sport.football (1 subscribers)
  weather (0 subscribers)

=== Machine 3: Client B (subscriber) ===
$ ./client <IP_Machine_1>
Connected to <IP_Machine_1>:9000
WELCOME 2
> SUBSCRIBE news
OK Subscribed to news
> SUBSCRIBE sport.*
OK Subscribed to sport.*
> [news] Breaking: system works!
> [sport.football] Goal scored!
```

### Reconnection Demo

```
# Client B on Machine 3 disconnects (Ctrl+C or QUIT)
# Server output: [server] Client 2 (fd 5) disconnected

# Client A on Machine 2 publishes while B is offline:
> PUBLISH news Late-breaking update
Delivered to 0 subscribers

# Within 60 seconds, Client B reconnects from Machine 3 with their old session ID:
$ ./client <IP_Machine_1> 2
Connected to <IP_Machine_1>:9000
WELCOME 3
OK Reconnected as 2 (2 subscriptions, 1 pending messages)
[news] Late-breaking update
```

## Design

### Architecture

```
   Machine 2                    Machine 1                    Machine 3
┌──────────┐  TCP (LAN)  ┌──────────────────────────────┐  TCP (LAN)  ┌──────────┐
│ Client A │◄────────────►│         Server               │◄────────────►│ Client B │
│ (stdin + │  port 9000  │                              │  port 9000  │ (stdin + │
│  socket) │             │  select() event loop         │             │  socket) │
└──────────┘             │  ┌─────────────────────────┐ │             └──────────┘
<IP_Machine_2>           │  │ Client table (128 slots) │ │           <IP_Machine_3>
                         │  │ Topic registry           │ │
                         │  │ Disconnected session list │ │
                         │  └─────────────────────────┘ │
                         └──────────────────────────────┘
                                 <IP_Machine_1>
```

The server is single-threaded. It uses `select()` to multiplex:
- The **listen socket** (for new connections)
- All **connected client sockets** (for incoming commands)

A 1-second `select()` timeout drives periodic expiration of stale
disconnected sessions.

The client also uses `select()` to multiplex:
- **stdin** (for user input)
- The **server socket** (for incoming messages)

This lets the client print messages from the server asynchronously
while the user types commands.

### Data Structures

**`Client`** — Fixed array of 128 slots. Each active slot holds:
- `fd` — socket file descriptor
- `id` — unique server-assigned session ID
- `rbuf` / `rlen` / `rcap` — dynamically grown receive buffer for
  partial-frame assembly
- `subs[64]` — array of subscription pattern strings (may contain
  `fnmatch` wildcards like `*`, `?`, `[...]`)

**`DiscClient`** — Singly-linked list of disconnected sessions. Each node
saves the client's ID, subscription patterns, disconnect timestamp, and a
linked list of `PendingMsg` nodes (messages published while offline).
Sessions expire after 60 seconds.

**`Server`** — Top-level struct aggregating the listen fd, client array,
monotonic ID counter, disconnected-session list, and a topic-name
registry (simple string array for the `TOPICS` command).

### How `select()` Is Used

Each iteration of the server's main loop:
1. Build an `fd_set` containing the listen socket + all active client fds.
2. Call `select()` with a 1-second timeout.
3. If the listen socket is readable → `accept()` a new connection.
4. For each client socket that is readable → `recv()` into its buffer,
   then extract and dispatch complete frames.
5. After `select()` returns (including on timeout), run `disc_expire()`
   to prune stale saved sessions.

The client does the same with two fds (stdin + server socket) and no
timeout (blocks until either is readable).

### Message Framing

All messages on the wire use a length-prefixed binary protocol:

```
┌────────────────────┬──────────────────────────────┐
│  4 bytes (BE u32)  │      payload (N bytes)       │
│  = payload length  │                              │
└────────────────────┴──────────────────────────────┘
```

- The 4-byte length is in **network byte order** (big-endian).
- Both sender and receiver handle **partial transfers**: `send_all()`
  loops until all bytes are written; the receiver accumulates bytes in
  a dynamic buffer and only processes a frame once `rlen >= 4 + payload_len`.
- This prevents message-boundary corruption that would occur with raw
  `recv()` on a stream socket.

### Wildcard Subscriptions

Subscription patterns are matched using POSIX `fnmatch(3)`. For example,
`SUBSCRIBE sport.*` matches `sport.football`, `sport.tennis`, etc.
Standard glob characters `*`, `?`, and `[...]` are supported.

### Persistent Queue

When a subscribed client disconnects, the server saves their subscription
patterns and session ID. Any messages published to matching topics during
the next 60 seconds are queued. If the client reconnects with
`./client <IP_Machine_1> <port> <old_id>`, the session is restored and all
pending messages are delivered immediately.

## Cleanup

`make clean` removes the compiled binaries.
