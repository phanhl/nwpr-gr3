# Pub/Sub Notification Server

A simple TCP-based publish-subscribe notification server written in C.

## Overview

This project uses **line-based text commands** over TCP. Each command must end with a newline (`\n`).

It supports:
- client identification with `ID`
- topic subscription / unsubscription
- publishing messages to topics
- wildcard subscriptions such as `sport.*`
- temporary offline message queue (TTL 60 seconds)
- topic listing with subscriber counts

---

## Source Files

- `server.c` - server implementation
- `client.c` - TCP client for sending commands and receiving notifications

---

## Supported Commands

### 1. `HELP`
Show the list of supported commands.

**Request:**
```txt
HELP
```

---

### 2. `ID <client_id>`
Identify the client with a logical ID.

- Required before using `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`, or `TOPICS`
- `client_id` must be 1 to 32 characters
- Allowed characters: `a-z A-Z 0-9 _ . -`

**Request:**
```txt
ID client1
```

**Example response:**
```txt
OK Identified as client1
```

If reconnecting with the same ID while offline messages are queued:
```txt
OK Reconnected as client1. Delivered 2 queued messages
```

---

### 3. `SUBSCRIBE <topic>`
Subscribe to an exact topic.

**Request:**
```txt
SUBSCRIBE news
```

**Example response:**
```txt
OK Subscribed to 'news'. Subscribers: 1
```

---

### 4. `SUBSCRIBE <prefix*>`
Subscribe using wildcard prefix matching.

Example: `sport.*` matches:
- `sport.football`
- `sport.basketball`
- `sport.tennis`

**Request:**
```txt
SUBSCRIBE sport.*
```

**Example response:**
```txt
OK Subscribed (wildcard) to 'sport.*'
```

**Note:** only suffix `*` is supported.

---

### 5. `UNSUBSCRIBE <topic>`
Unsubscribe from an exact topic.

**Request:**
```txt
UNSUBSCRIBE news
```

**Example response:**
```txt
OK Unsubscribed from 'news'
```

---

### 6. `UNSUBSCRIBE <prefix*>`
Unsubscribe from a wildcard prefix subscription.

**Request:**
```txt
UNSUBSCRIBE sport.*
```

**Example response:**
```txt
OK Unsubscribed (wildcard) from 'sport.*'
```

---

### 7. `PUBLISH <topic> <message>`
Publish a message to a topic.

- Publisher does **not** receive its own message
- Matching online subscribers receive the message immediately
- Matching offline subscribers have the message queued for up to **60 seconds**

**Request:**
```txt
PUBLISH news Hello everyone
```

**Publisher response:**
```txt
Delivered to 1 subscribers
```

**Subscriber receives:**
```txt
[news] Hello everyone
```

---

### 8. `TOPICS`
List known topics and current subscriber counts.

**Request:**
```txt
TOPICS
```

**Example response:**
```txt
TOPICS 3
weather 0
sport.football 1
news 1
END
```

---

## Protocol Notes

- Communication uses **TCP**
- Message framing is **delimiter-based**
- Each command must be sent as **one line ending with `\n`**
- Commands are text-based and space-separated

---

## Build Instructions

### Build the server
```bash
gcc -O2 -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L server.c -o server
```

### Build the client
```bash
gcc -O2 -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L client.c -o client
```

### Minimal build commands
If you want shorter commands, these also work:

```bash
gcc server.c -o server
gcc client.c -o client
```

---

## Run Instructions

### 1. Start the server
```bash
./server 9000
```

Example server output:
```txt
=== Pub/Sub Server listening on port 9000 ===
```

---

### 2. Start the client
Open one or more terminals and run:

```bash
./client <host> <port> [client_id]
```

Examples:

Without automatic ID:
```bash
./client 127.0.0.1 9000
```

With automatic ID:
```bash
./client 127.0.0.1 9000 client1
```

If using multiple machines, replace `127.0.0.1` with the server machine IP.

---

## Example Session

### Subscriber terminal
```bash
./client 127.0.0.1 9000 client2
```

Then type:
```txt
SUBSCRIBE news
SUBSCRIBE sport.*
```

### Publisher terminal
```bash
./client 127.0.0.1 9000 client1
```

Then type:
```txt
PUBLISH news Breaking update
PUBLISH sport.football Match tonight at 8PM
PUBLISH weather Rain tomorrow
```

### Example server log output
```txt
=== Pub/Sub Server listening on port 9000 ===
[server] Client 1 connected from 192.168.1.10:54320 (fd 4)
[server] Client 2 connected from 192.168.1.11:54321 (fd 5)
[server] Client 2 subscribed to 'news'
[server] Client 2 subscribed to 'sport.*'
[server] Client 1 published to 'news' -> 1 subscribers
[server] Client 1 published to 'sport.football' -> 1 subscribers
[server] Client 1 published to 'weather' -> 0 subscribers
```

---

## Client Usage Notes

When the client starts, it can optionally send the ID automatically if `[client_id]` is provided.

The client also allows typing commands manually from standard input, for example:

```txt
ID alice
SUBSCRIBE news
SUBSCRIBE sport.*
PUBLISH news Hello
TOPICS
```

The client prints all complete lines received from the server.

---

## Error Cases

Examples of invalid usage:

### Missing ID before other commands
```txt
ERR Please set ID first: ID <client_id>
```

### Invalid ID
```txt
ERR Usage: ID <client_id> (1..32 chars: a-zA-Z0-9_.-)
```

### Invalid wildcard
```txt
ERR Invalid wildcard. Only suffix '*' is supported, e.g. sport.*
```

### Invalid publish format
```txt
ERR Usage: PUBLISH <topic> <message>
```

### Unknown command
```txt
ERR Unknown command. Type HELP
```

---

## Notes

- Topics are tracked when subscribed to or published to
- Wildcard subscriptions use prefix matching only
- Offline queue lifetime is **60 seconds**
- This is a simple educational pub/sub server, not a full message broker like Apache Kafka
