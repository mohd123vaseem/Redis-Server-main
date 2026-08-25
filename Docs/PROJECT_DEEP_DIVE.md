# Redis-Like Server in C++ — Deep Technical Study Guide

> A structured, diagram-rich breakdown of every layer of this project.

---

## Table of Contents

1. [High-Level Overview](#1-high-level-overview)
2. [Architecture Diagram](#2-architecture-diagram)
3. [Architecture Breakdown](#3-architecture-breakdown)
4. [Request Flow](#4-request-flow)
5. [Networking Layer](#5-networking-layer)
6. [Command Parsing](#6-command-parsing)
7. [Data Storage Layer](#7-data-storage-layer)
8. [Concurrency Model](#8-concurrency-model)
9. [Persistence Mechanism](#9-persistence-mechanism)
10. [Limitations & Design Trade-offs](#10-limitations--design-trade-offs)
11. [Improvement Roadmap](#11-improvement-roadmap)
12. [Key Learning Takeaways](#12-key-learning-takeaways)

---

## 1. High-Level Overview

### What This Project Does

This project is a simplified, from-scratch implementation of a Redis-compatible server written in C++. It:

- Listens for TCP connections on port **6379** (the same default port as real Redis).
- Speaks a subset of the **RESP protocol** (Redis Serialization Protocol), making it compatible with standard `redis-cli`.
- Stores data in three types: **strings** (key-value), **lists**, and **hashes**.
- Handles **multiple concurrent clients** using one OS thread per client.
- Persists data to a text-based file (`dump.my_rdb`) and reloads it on startup.

### How It Compares to Real Redis

| Feature                   | This Project                          | Real Redis                              |
|---------------------------|---------------------------------------|-----------------------------------------|
| Protocol                  | RESP (partial)                        | Full RESP2 / RESP3                      |
| Concurrency               | Thread-per-client (blocking I/O)      | Single-threaded event loop (epoll/kqueue)|
| Data types                | string, list, hash                    | string, list, hash, set, sorted set, stream, etc. |
| Persistence               | Custom text file (dump.my_rdb)        | RDB snapshots + AOF (append-only file)  |
| Expiry                    | Lazy purge on access                  | Active + lazy expiry                    |
| Pub/Sub                   | Not implemented                       | Supported                               |
| Replication               | Not implemented                       | Master-replica supported                |
| Memory management         | STL containers (heap)                 | Custom allocator (jemalloc/tcmalloc)    |
| Atomic operations         | Coarse-grained mutex                  | Single-thread = naturally atomic        |

### Main System Components

```
┌─────────────────────────────────────────────┐
│                  main.cpp                   │
│  - Starts persistence background thread     │
│  - Loads dump.my_rdb on boot                │
│  - Creates RedisServer and calls run()      │
└─────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────┐
│               RedisServer                   │
│  - TCP socket creation, bind, listen        │
│  - Accepts clients in a loop                │
│  - Spawns a new std::thread per client      │
└─────────────────────────────────────────────┘
              │  (per-client thread)
              ▼
┌─────────────────────────────────────────────┐
│           RedisCommandHandler               │
│  - Parses raw bytes as RESP or plain text   │
│  - Routes to the correct handler function   │
│  - Returns a RESP-formatted response string │
└─────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────┐
│             RedisDatabase (Singleton)       │
│  - Stores kv_store, list_store, hash_store  │
│  - Single mutex guards all three stores     │
│  - dump() / load() for persistence         │
└─────────────────────────────────────────────┘
```

---

## 2. Architecture Diagram

```
                        ┌─────────────────────────┐
                        │      redis-cli / client  │
                        │  (any RESP-speaking tool)│
                        └────────────┬────────────┘
                                     │  TCP :6379
                                     │  RESP bytes
                                     ▼
┌────────────────────────────────────────────────────────────────────────┐
│                           RedisServer                                  │
│                                                                        │
│   server_socket (int)    ◄── bind(6379), listen(backlog=10)           │
│                                                                        │
│   accept() loop ─────────────────────────────────────────────────┐   │
│                                                                   │   │
│   ┌────────────────────────────────────────────────────────────┐  │   │
│   │                   std::thread (per client)                 │  │   │
│   │                                                            │  │   │
│   │   recv(client_socket, buffer, 1024)                        │  │   │
│   │          │                                                 │  │   │
│   │          ▼                                                 │  │   │
│   │   ┌─────────────────────────────────────────────────┐     │  │   │
│   │   │         RedisCommandHandler::processCommand()   │     │  │   │
│   │   │                                                 │     │  │   │
│   │   │   parseRespCommand(raw_bytes)                   │     │  │   │
│   │   │          │                                      │     │  │   │
│   │   │          ▼  tokens = ["SET","key","val"]        │     │  │   │
│   │   │   if-else dispatch chain                        │     │  │   │
│   │   │          │                                      │     │  │   │
│   │   │          ▼                                      │     │  │   │
│   │   │   handleSet() / handleGet() / ...               │     │  │   │
│   │   │          │                                      │     │  │   │
│   │   │          ▼                                      │     │  │   │
│   │   │   ┌─────────────────────────────────────────┐  │     │  │   │
│   │   │   │   RedisDatabase (Singleton)              │  │     │  │   │
│   │   │   │                                         │  │     │  │   │
│   │   │   │  db_mutex (std::mutex)                  │  │     │  │   │
│   │   │   │  kv_store   : unordered_map<str,str>    │  │     │  │   │
│   │   │   │  list_store : unordered_map<str,vec>    │  │     │  │   │
│   │   │   │  hash_store : unordered_map<str,map>    │  │     │  │   │
│   │   │   │  expiry_map : unordered_map<str,tp>     │  │     │  │   │
│   │   │   └─────────────────────────────────────────┘  │     │  │   │
│   │   │                                                 │     │  │   │
│   │   │   Returns: "+OK\r\n" (RESP string)              │     │  │   │
│   │   └─────────────────────────────────────────────────┘     │  │   │
│   │          │                                                 │  │   │
│   │          ▼                                                 │  │   │
│   │   send(client_socket, response)                           │  │   │
│   └────────────────────────────────────────────────────────────┘  │   │
│                                                                   │   │
│   ◄─────────────────────────── repeat per new client ────────────┘   │
└────────────────────────────────────────────────────────────────────────┘
                                     │
                              ┌──────┴──────┐
                              │ Background  │
                              │  Thread     │
                              │ every 300s: │
                              │ dump()      │
                              │ → dump.my_rdb│
                              └─────────────┘
```

---

## 3. Architecture Breakdown

### `main.cpp` — Entry Point and Bootstrap

```
src/main.cpp
```

`main.cpp` is the program's bootstrap layer. It performs three jobs in strict order:

**1. Database loading** (before any client can connect):
```cpp
if (RedisDatabase::getInstance().load("dump.my_rdb"))
    std::cout << "Database Loaded From dump.my_rdb\n";
```
It calls `RedisDatabase::getInstance()` — the singleton accessor — and attempts to deserialize
`dump.my_rdb` into the three in-memory stores. If the file doesn't exist, the server starts fresh.

**2. Background persistence thread** (detached, runs forever):
```cpp
std::thread persistanceThread([](){
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(300));
        RedisDatabase::getInstance().dump("dump.my_rdb");
    }
});
persistanceThread.detach();
```
This thread wakes up every 5 minutes and serializes memory to disk. Because it is `detach()`-ed, it
runs independently of the main thread and is never explicitly joined.

**3. Server startup**:
```cpp
RedisServer server(port);
server.run(); // blocks forever (accept loop)
```
`server.run()` never returns during normal operation — it blocks in the `accept()` loop.

---

### `RedisServer` — Networking and Client Lifecycle

```
include/RedisServer.h
src/RedisServer.cpp
```

`RedisServer` owns the TCP lifecycle from socket creation through per-client threading.

**Key fields:**
```cpp
int port;                   // TCP port (default 6379)
int server_socket;          // the listening file descriptor
std::atomic<bool> running;  // set to false on SIGINT
```

**Startup sequence in `run()`:**
```
socket(AF_INET, SOCK_STREAM, 0)   → creates TCP socket
setsockopt(..., SO_REUSEADDR)     → allows port reuse after restart
bind(server_socket, :port)        → binds to 0.0.0.0:6379
listen(server_socket, backlog=10) → marks socket as passive; kernel queue = 10
```

**Client loop:**
```cpp
while (running) {
    int client_socket = accept(server_socket, nullptr, nullptr);
    threads.emplace_back([client_socket, &cmdHandler](){
        // read → process → write loop for this client
    });
}
```

Each accepted client gets its own `std::thread`. The thread captures `client_socket` by value (safe)
and `cmdHandler` by reference (shared — see concurrency section).

**Signal handling** (`SIGINT` / Ctrl+C):
```cpp
static RedisServer* globalServer = nullptr;  // global pointer
void signalHandler(int signum) {
    globalServer->shutdown();   // sets running=false, dumps DB, closes socket
    exit(signum);
}
```
This is a classic C-style signal handler pattern — a global pointer to the object is set in the
constructor so the free function `signalHandler` can reach it.

---

### `RedisCommandHandler` — Parsing and Dispatch

```
include/RedisCommandHandler.h
src/RedisCommandHandler.cpp
```

`RedisCommandHandler` has a single public method:
```cpp
std::string processCommand(const std::string& commandLine);
```

It has **no mutable state** — it is stateless. The instance is created once in `RedisServer::run()`
and shared across all client threads via reference capture.

Internally, `processCommand` does:
1. Calls `parseRespCommand()` to tokenize the raw bytes.
2. Normalizes the first token to uppercase (`std::transform + ::toupper`).
3. Dispatches via a large `if-else` chain to one of ~25 static handler functions.
4. Each handler function calls `RedisDatabase::getInstance()` to operate on data.

---

### `RedisDatabase` — Data Store (Singleton)

```
include/RedisDatabase.h
src/RedisDatabase.cpp
```

`RedisDatabase` is a **Meyers Singleton** — thread-safe by the C++11 standard:
```cpp
static RedisDatabase& getInstance() {
    static RedisDatabase instance;  // initialized once, on first call
    return instance;
}
```

It holds **four maps** all guarded by a single `std::mutex db_mutex`:

```cpp
std::mutex db_mutex;

// String data
std::unordered_map<std::string, std::string> kv_store;

// List data (std::vector used as the list body)
std::unordered_map<std::string, std::vector<std::string>> list_store;

// Hash data (nested maps)
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hash_store;

// TTL expiry timestamps (steady_clock)
std::unordered_map<std::string, std::chrono::steady_clock::time_point> expiry_map;
```

Every public method acquires `lock_guard<mutex>` at entry, making all operations mutually exclusive.

---

## 4. Request Flow

### Step-by-Step: `SET mykey hello`

```
Client types:  SET mykey hello
redis-cli encodes it as RESP:
    *3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$5\r\nhello\r\n
```

#### Sequence Diagram

```
  redis-cli         RedisServer        RedisCommandHandler      RedisDatabase
      │                  │                     │                     │
      │──TCP connect────►│                     │                     │
      │                  │ accept()            │                     │
      │                  │ spawn thread        │                     │
      │                  │                     │                     │
      │──RESP bytes─────►│                     │                     │
      │  *3\r\n          │                     │                     │
      │  $3\r\nSET\r\n   │ recv(buf, 1024)     │                     │
      │  $5\r\nmykey\r\n │                     │                     │
      │  $5\r\nhello\r\n │                     │                     │
      │                  │                     │                     │
      │                  │──processCommand()──►│                     │
      │                  │   (raw string)      │                     │
      │                  │                     │                     │
      │                  │                     │ parseRespCommand()  │
      │                  │                     │ → ["SET","mykey",   │
      │                  │                     │    "hello"]         │
      │                  │                     │                     │
      │                  │                     │ cmd = "SET"         │
      │                  │                     │ toupper()           │
      │                  │                     │                     │
      │                  │                     │──handleSet()───────►│
      │                  │                     │  tokens[1]="mykey"  │
      │                  │                     │  tokens[2]="hello"  │
      │                  │                     │                     │
      │                  │                     │                     │ lock_guard(db_mutex)
      │                  │                     │                     │ kv_store["mykey"]="hello"
      │                  │                     │                     │ mutex released
      │                  │                     │                     │
      │                  │                     │◄── return ──────────│
      │                  │                     │                     │
      │                  │                     │ builds "+OK\r\n"    │
      │                  │◄──"+OK\r\n"─────────│                     │
      │                  │                     │                     │
      │                  │ send(client_socket, │                     │
      │                  │   "+OK\r\n")        │                     │
      │                  │                     │                     │
      │◄──"+OK\r\n"──────│                     │                     │
      │                  │                     │                     │
   redis-cli
   displays: OK
```

#### Detailed Breakdown Per Layer

| Layer | Code Location | Action |
|-------|--------------|--------|
| **Client** | redis-cli | User types `SET mykey hello`; redis-cli encodes as RESP array |
| **Socket read** | `RedisServer.cpp:89` | `recv(client_socket, buffer, 1023, 0)` fills buffer with raw bytes |
| **Dispatch** | `RedisServer.cpp:92` | `cmdHandler.processCommand(request)` called |
| **RESP parse** | `RedisCommandHandler.cpp:17` | `parseRespCommand()` splits into `["SET", "mykey", "hello"]` |
| **Command route** | `RedisCommandHandler.cpp:346` | `if (cmd == "SET") return handleSet(tokens, db)` |
| **DB write** | `RedisDatabase.cpp:24` | `lock_guard` acquired; `kv_store["mykey"] = "hello"` |
| **Response build** | `RedisCommandHandler.cpp:84` | Returns `"+OK\r\n"` |
| **Socket write** | `RedisServer.cpp:93` | `send(client_socket, response.c_str(), ...)` |
| **Client display** | redis-cli | Decodes `+OK\r\n` and prints `OK` |

---

## 5. Networking Layer

### Socket Creation and Server Setup

The server uses **POSIX TCP sockets** (Berkeley sockets API), which is the standard low-level
networking API on Linux/macOS.

```
socket(AF_INET, SOCK_STREAM, 0)
  │
  │  AF_INET    = IPv4 address family
  │  SOCK_STREAM = TCP (reliable, ordered, byte-stream)
  │  0          = auto-select protocol (TCP)
  │
  ▼ Returns a file descriptor (int)

setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))
  │
  │  Allows the port to be reused immediately after the server crashes/restarts
  │  Without this: "Address already in use" error for ~60 seconds (TIME_WAIT)
  │
  ▼

bind(fd, {AF_INET, htons(6379), INADDR_ANY}, sizeof(addr))
  │
  │  INADDR_ANY = bind to all network interfaces (0.0.0.0)
  │  htons()    = host-to-network byte order (big-endian) conversion
  │
  ▼

listen(fd, backlog=10)
  │
  │  backlog=10: The OS kernel queues up to 10 connection requests
  │  before accept() is called. Beyond this, new connections are refused.
  │
  ▼

accept(fd, nullptr, nullptr)  ← BLOCKING call
  │
  │  Blocks until a new TCP connection arrives
  │  Returns a new fd dedicated to that one client
  │
  ▼ client_socket (int) — per-client file descriptor
```

### Data Read/Write Per Client

```cpp
// READ (blocking)
int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
// buffer is 1024 bytes — a hard limit (see limitations section)

// WRITE
send(client_socket, response.c_str(), response.size(), 0);
```

**Important:** `recv()` is called with a fixed 1024-byte buffer. This means commands longer than
1023 bytes will be **silently truncated** — a significant limitation for large values.

### Connection Lifecycle

```
Client connects
     │
     ▼
accept() returns client_fd
     │
     ▼
std::thread spawned
     │
     ├──── recv() loop ────────────────────────────────────────┐
     │         │                                               │
     │         ▼ (bytes > 0)                                  │
     │     processCommand() → send()                          │
     │         │                                               │
     │         └──────────────────────────────────────────────┘
     │
     ▼ (bytes <= 0: client disconnected or error)
close(client_socket)
thread exits
```

---

## 6. Command Parsing

### RESP Protocol

The server implements a subset of **RESP (REdis Serialization Protocol)**. RESP uses simple
prefix characters to encode types:

```
+  Simple String     e.g.  +OK\r\n
-  Error             e.g.  -Error: unknown command\r\n
:  Integer           e.g.  :42\r\n
$  Bulk String       e.g.  $5\r\nhello\r\n
*  Array             e.g.  *2\r\n$4\r\nPING\r\n$4\r\nTEST\r\n
```

### How `parseRespCommand()` Works

```
src/RedisCommandHandler.cpp:17
```

```
Input: "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$5\r\nhello\r\n"

Step 1: Check input[0] == '*'  → YES, it's RESP format
Step 2: Read count after '*'   → "3" → numElements = 3
Step 3: Advance pos past "\r\n"
Step 4: Loop 3 times:
    Iteration 1:
        input[pos] == '$' → skip '$'
        Read length: "3"  → len = 3
        Read 3 bytes:     → "SET"
        tokens.push_back("SET")
        pos += 3 + 2  (token + CRLF)
    Iteration 2:
        len = 5, token = "mykey"
        tokens.push_back("mykey")
    Iteration 3:
        len = 5, token = "hello"
        tokens.push_back("hello")

Output: ["SET", "mykey", "hello"]
```

**Fallback for plain text** (non-RESP input):
```cpp
if (input[0] != '*') {
    std::istringstream iss(input);
    std::string token;
    while (iss >> token)
        tokens.push_back(token);
    return tokens;
}
```
This fallback allows testing with `telnet` or `nc` without needing RESP encoding.

### Command Dispatch

```
src/RedisCommandHandler.cpp:329–399
```

After parsing, the first token is normalized to uppercase:
```cpp
std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
```

Then dispatched via an `if-else` chain:

```
                     ┌──────────────────────────┐
  tokens[0] (cmd) ──►│  if-else dispatch chain  │
                     └──────────────────────────┘
                           │         │         │
                  ┌────────┘    ┌────┘    ┌────┘
                  ▼             ▼          ▼
            handlePing()  handleSet()  handleLpush()
            handleGet()   handleDel()  handleHset()
            handleFlushAll() ...       ...
                  │             │          │
                  └─────────────┴──────────┘
                                │
                     RedisDatabase::getInstance()
                          (all handlers call this)
```

**All 25 supported commands:**

| Category   | Commands |
|------------|----------|
| General    | `PING`, `ECHO`, `FLUSHALL` |
| String     | `SET`, `GET`, `KEYS`, `TYPE`, `DEL`/`UNLINK`, `EXPIRE`, `RENAME` |
| List       | `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LGET`, `LREM`, `LINDEX`, `LSET` |
| Hash       | `HSET`, `HGET`, `HEXISTS`, `HDEL`, `HGETALL`, `HKEYS`, `HVALS`, `HLEN`, `HMSET` |

---

## 7. Data Storage Layer

### Three Separate In-Memory Stores

Unlike real Redis (which uses a single keyspace with type tagging), this project uses **three
physically separate hash maps**, one per data type:

```cpp
// include/RedisDatabase.h:61-63
std::unordered_map<std::string, std::string>
    kv_store;   // SET/GET/DEL/EXPIRE keys live here

std::unordered_map<std::string, std::vector<std::string>>
    list_store; // LPUSH/RPUSH/LPOP/RPOP keys live here

std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
    hash_store; // HSET/HGET/HGETALL keys live here
```

```
                 ┌─────────────────────────────────────────┐
                 │         RedisDatabase (in memory)        │
                 │                                          │
                 │  kv_store                                │
                 │  ┌─────────────────────────────────┐    │
                 │  │ "name"  → "Alice"               │    │
                 │  │ "city"  → "Berlin"              │    │
                 │  │ "count" → "42"                  │    │
                 │  └─────────────────────────────────┘    │
                 │                                          │
                 │  list_store                              │
                 │  ┌─────────────────────────────────┐    │
                 │  │ "fruits" → ["apple","banana"]   │    │
                 │  │ "queue"  → ["job1","job2","job3"]│    │
                 │  └─────────────────────────────────┘    │
                 │                                          │
                 │  hash_store                              │
                 │  ┌─────────────────────────────────┐    │
                 │  │ "user:1" → { "name":"Bob",      │    │
                 │  │              "age":"30"  }       │    │
                 │  │ "user:2" → { "name":"Eve",      │    │
                 │  │              "age":"25"  }       │    │
                 │  └─────────────────────────────────┘    │
                 │                                          │
                 │  expiry_map                              │
                 │  ┌─────────────────────────────────┐    │
                 │  │ "session" → time_point(+3600s)  │    │
                 │  └─────────────────────────────────┘    │
                 └─────────────────────────────────────────┘
```

### Why `std::unordered_map`?

- **O(1) average** lookup, insert, and delete — the right choice for a key-value store.
- Keys in Redis are strings, and `std::unordered_map<std::string, ...>` uses `std::hash<std::string>`.
- Worst case is O(n) (hash collision), but unlikely with typical Redis usage patterns.

### Why `std::vector<std::string>` for Lists?

Pros:
- Simple implementation.
- Random access by index (`lindex`, `lset`) is O(1).
- `rpush` is amortized O(1) (`push_back`).

Cons:
- `lpush` calls `insert(begin(), value)` which is **O(n)** — shifts all elements right.
- `lpop` calls `erase(begin())` which is also **O(n)**.
- Real Redis uses a **doubly-linked list** (or `listpack`/`quicklist` hybrid) for O(1) head/tail ops.

### Key Collision Across Stores

Because the three stores are separate maps, the same key name can exist in multiple stores
simultaneously (e.g., `kv_store["foo"]` and `list_store["foo"]` can both exist). The `type()`
command checks them in priority order (`kv_store` first), which can lead to surprising behavior.
Real Redis enforces that each key has exactly one type.

### TTL / Expiry

```cpp
// include/RedisDatabase.h:65
std::unordered_map<std::string, std::chrono::steady_clock::time_point> expiry_map;
```

**Expiry uses a lazy-purge strategy.** No background timer fires. Instead, `purgeExpired()` is
called at the top of operations that read data (`get`, `keys`, `type`, `del`, `expire`, `rename`):

```cpp
void RedisDatabase::purgeExpired() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = expiry_map.begin(); it != expiry_map.end(); ) {
        if (now > it->second) {
            kv_store.erase(it->first);
            list_store.erase(it->first);
            hash_store.erase(it->first);
            it = expiry_map.erase(it);
        } else { ++it; }
    }
}
```

`std::chrono::steady_clock` is used (not `system_clock`) — it is monotonic and unaffected by
system clock changes (NTP, daylight saving, etc.).

**Bug:** `purgeExpired()` is called while already holding `db_mutex` (since all callers already hold
it). This is correct — there is no re-locking attempt. However, `set()` does NOT call `purgeExpired`,
so setting a key does not remove a stale expired version first.

---

## 8. Concurrency Model

### Thread-per-Client Model

```
main thread
    │
    ├── persistenceThread (detached)
    │       sleeps 300s, dumps DB, repeat
    │
    └── server.run() ──► accept() loop
              │
              ├── thread_1 (client A)  ──► recv → process → send (loop)
              │
              ├── thread_2 (client B)  ──► recv → process → send (loop)
              │
              ├── thread_3 (client C)  ──► recv → process → send (loop)
              │
              └── thread_N (client N)  ──► ...
```

### How Threads Are Managed

```cpp
// src/RedisServer.cpp:74
std::vector<std::thread> threads;
```

Threads are pushed into a `vector<thread>`. When the server shuts down:
```cpp
for (auto& t : threads) {
    if (t.joinable()) t.join();
}
```

**Problem:** The vector grows forever — threads are never removed from it after they exit.
If 10,000 clients connect and disconnect, the vector holds 10,000 finished thread handles.

### Mutex Usage

```
                  Thread 1              Thread 2         Persistence Thread
                 (Client A)            (Client B)
                     │                     │                    │
                     │ SET key val         │ GET key            │ dump()
                     │                     │                    │
                     ▼                     ▼                    ▼
               lock(db_mutex)       BLOCKS waiting...    BLOCKS waiting...
                     │
               kv_store["key"] = "val"
                     │
               unlock(db_mutex)
                                          │
                                    lock(db_mutex)
                                          │
                                    kv_store.find("key")
                                          │
                                    unlock(db_mutex)
```

**One mutex protects all three stores.** This means:
- A `HSET` on a hash key blocks a `GET` on a completely unrelated string key.
- High contention under load — the entire database serializes through one lock.

### `RedisCommandHandler` Thread Safety

`RedisCommandHandler` has no member variables — it is a pure function dispatcher. It is safe to
share one instance across all client threads (which is what the code does via `&cmdHandler` capture
in the lambda at `RedisServer.cpp:85`).

---

## 9. Persistence Mechanism

### Overview

The project uses a **custom text-based snapshot format** stored in `dump.my_rdb`. It is conceptually
similar to Redis RDB (point-in-time snapshot), but much simpler — it is a human-readable text file.

### File Format

```
K name Alice
K city Berlin
L fruits apple banana orange
L queue job1 job2
H user:1 name:Bob age:30 email:bob@example.com
H user:2 name:Eve age:25
```

Each line starts with a type prefix:
- `K` = key-value string
- `L` = list (space-separated items after the key)
- `H` = hash (space-separated `field:value` pairs after the key)

### Dump (Memory → File)

```
src/RedisDatabase.cpp:353
```

```cpp
bool RedisDatabase::dump(const std::string& filename) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::ofstream ofs(filename, std::ios::binary);

    for (const auto& kv: kv_store)
        ofs << "K " << kv.first << " " << kv.second << "\n";

    for (const auto& kv : list_store) {
        ofs << "L " << kv.first;
        for (const auto& item : kv.second)
            ofs << " " << item;
        ofs << "\n";
    }

    for (const auto& kv : hash_store) {
        ofs << "H " << kv.first;
        for (const auto& fv : kv.second)
            ofs << " " << fv.first << ":" << fv.second;
        ofs << "\n";
    }
    return true;
}
```

### Load (File → Memory)

```
src/RedisDatabase.cpp:398
```

Reads the file line by line. Each line is parsed with `std::istringstream`:
```
Line: "H user:1 name:Bob age:30"
      │    │      │         │
      type key   field:val  field:val
```
For hash fields, it splits on `:` using `pair.find(':')`.

### When Persistence Happens

```
┌─────────────────────────────────────────────────────────────────┐
│                     Persistence Triggers                        │
│                                                                 │
│  1. Startup:    load("dump.my_rdb")     ← main.cpp:11          │
│                                                                 │
│  2. Background: dump("dump.my_rdb")     ← main.cpp:22          │
│                 every 300 seconds                               │
│                                                                 │
│  3. Shutdown:   dump("dump.my_rdb")     ← RedisServer.cpp:38   │
│                 triggered by SIGINT                             │
│                                                                 │
│  4. After run() loop exits:             ← RedisServer.cpp:104  │
│                 dump("dump.my_rdb")                             │
└─────────────────────────────────────────────────────────────────┘
```

### Persistence Limitations

- **Space characters** in keys or values break parsing. `"K my key my value"` would be parsed as
  key=`"my"`, value=`"key"`, dropping `"my value"` entirely.
- **Colon characters** in hash values break field-value splitting: `"name:Bob:Smith"` → field=`"name"`, value=`"Bob:Smith"` (works only because `find(':')` gets the first colon).
- Expiry information is **not persisted** — all TTLs are lost on restart.
- No atomic write (no temp file + rename) — a crash during `dump()` corrupts the file.

---

## 10. Limitations & Design Trade-offs

### Summary Table

| Area | Limitation | Impact |
|------|------------|--------|
| **Buffer size** | Fixed 1024-byte `recv()` buffer | Commands/values > 1023 bytes silently truncated |
| **Thread model** | One thread per client | OS thread limit (~1000–10000 on Linux); context-switch overhead at scale |
| **Thread list** | `vector<thread>` never pruned | Memory leak: dead thread handles accumulate |
| **Global mutex** | Single `db_mutex` for all ops | All DB operations serialize; no read parallelism |
| **lpush/lpop** | O(n) due to `vector::insert(begin())` | Slow for large lists with frequent head operations |
| **Key collision** | Three separate stores; same key can exist in multiple types | Undefined behavior for `TYPE`, `DEL`, etc. |
| **Persistence format** | Space-delimited text | Breaks with spaces in keys/values |
| **Expiry loss** | `expiry_map` not serialized | All TTLs lost on restart |
| **No AOF** | Only snapshots | Data written since last snapshot is lost on crash |
| **Lazy expiry only** | `purgeExpired()` on read operations only | Expired keys occupy memory until accessed |
| **RESP parsing** | No pipeline support | Can't batch multiple commands in one `send()` |
| **No authentication** | No `AUTH` command | Any client can connect |
| **DEL bug** | `del()` returns `false` always | `RedisDatabase.cpp:75` — always returns `false` instead of `erased` |

### The DEL Bug (Notable)

```cpp
// src/RedisDatabase.cpp:68-76
bool RedisDatabase::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    bool erased = false;
    erased |= kv_store.erase(key) > 0;
    erased |= list_store.erase(key) > 0;
    erased |= hash_store.erase(key) > 0;
    return false;   // ← BUG: should be `return erased;`
}
```
The key IS deleted correctly from the stores, but the function always signals failure. The
`handleDel()` response `":0\r\n"` is always returned to the client even on a successful delete.

---

## 11. Improvement Roadmap

### 1. Fix the Buffer Size Limit

**Current:**
```cpp
char buffer[1024];
recv(client_socket, buffer, sizeof(buffer) - 1, 0);
```
**Fix:** Use a dynamic buffer that reads until a complete RESP message is assembled:
```cpp
std::string readBuffer;
// Accumulate until RESP message is complete, then process
```
This also fixes pipelining (multiple commands in one TCP segment).

### 2. Replace Thread-per-Client with I/O Multiplexing

**Current model:**
```
accept → spawn thread → blocking recv()
```
**Better model (epoll on Linux):**
```
epoll_create1()
epoll_ctl(ADD, server_fd, EPOLLIN)

loop:
    events = epoll_wait(epollfd, events, MAX_EVENTS, -1)
    for each event:
        if event.fd == server_fd: accept() new client, epoll_ctl(ADD)
        else: read from client fd, process, respond
```
This handles thousands of clients with a single thread (or a small thread pool).

### 3. Replace Global Mutex with Sharded Locking

**Current:** One `db_mutex` for all keys.

**Better:** Shard the keyspace into N buckets, each with its own mutex:
```cpp
static const int SHARDS = 16;
std::mutex shard_mutex[SHARDS];
std::unordered_map<std::string, std::string> kv_shards[SHARDS];

int shard(const std::string& key) {
    return std::hash<std::string>{}(key) % SHARDS;
}
```
Operations on different keys can now proceed in parallel.

### 4. Replace `std::vector` List with `std::deque`

**Current:** `vector` → O(n) `lpush`/`lpop`.

**Fix:** Use `std::deque<std::string>` → O(1) front and back insertions/deletions:
```cpp
std::unordered_map<std::string, std::deque<std::string>> list_store;
```

### 5. Fix Persistence Format

Use a length-prefixed binary format to handle arbitrary bytes in keys/values:
```
K <key_len> <key_bytes> <val_len> <val_bytes>
```
Or adopt a proper format like MessagePack or a simple TLV encoding.

### 6. Persist Expiry Map

```cpp
// In dump(): also write expiry entries
for (const auto& kv : expiry_map) {
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        kv.second - std::chrono::steady_clock::now()).count();
    if (remaining > 0)
        ofs << "E " << kv.first << " " << remaining << "\n";
}
```

### 7. Active Expiry Background Thread

```cpp
std::thread expiryThread([](){
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        RedisDatabase::getInstance().purgeExpired();
    }
});
```

### 8. Atomic Persistence (Crash-Safe)

```cpp
// Write to temp file, then atomic rename
std::string tmpFile = filename + ".tmp";
// ... write to tmpFile ...
std::rename(tmpFile.c_str(), filename.c_str()); // atomic on POSIX
```

### 9. Thread Pool Instead of Unbounded Thread Spawning

```
┌─────────────────────────────────────────────────────────┐
│                     Thread Pool                          │
│                                                          │
│   accept() ──► push(client_fd) ──► work queue           │
│                                         │               │
│   ┌──────────┐  ┌──────────┐  ┌────────┴─┐             │
│   │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  ...         │
│   │ pop fd   │  │ pop fd   │  │ pop fd   │             │
│   │ process  │  │ process  │  │ process  │             │
│   └──────────┘  └──────────┘  └──────────┘             │
└─────────────────────────────────────────────────────────┘
```
Cap the number of OS threads; reuse them across connections.

---

## 12. Key Learning Takeaways

### System Design Concepts This Project Teaches

| Concept | Where It Appears in This Code |
|---------|-------------------------------|
| **TCP server lifecycle** | `RedisServer::run()` — socket, bind, listen, accept loop |
| **POSIX socket API** | `socket()`, `setsockopt()`, `bind()`, `listen()`, `accept()`, `recv()`, `send()`, `close()` |
| **Custom binary protocol parsing** | `parseRespCommand()` — byte-level RESP parsing with CRLF markers |
| **Singleton pattern** | `RedisDatabase::getInstance()` — Meyers singleton, thread-safe since C++11 |
| **RAII for mutex** | `std::lock_guard<std::mutex>` — automatic unlock on scope exit |
| **Thread-per-client model** | `std::thread` spawned per `accept()` |
| **Atomic flag for shutdown** | `std::atomic<bool> running` — safe cross-thread flag |
| **Signal handling in C++** | `signal(SIGINT, handler)` + global pointer pattern |
| **Lazy expiry / lazy deletion** | `purgeExpired()` called on read, not on a timer |
| **Monotonic clocks** | `std::chrono::steady_clock` for TTL tracking |
| **File I/O and serialization** | `std::ofstream`/`std::ifstream` for persistence |
| **Detached background threads** | `persistenceThread.detach()` for periodic saves |
| **Hash map performance** | `unordered_map` O(1) average operations |
| **Protocol compatibility** | Speaking RESP makes the server work with existing Redis tooling |
| **Graceful shutdown** | SIGINT → `shutdown()` → dump DB → close socket |

### The Bigger Picture

This project is a microcosm of how networked data servers work at a fundamental level:

```
  Every production database server follows the same skeleton:
  
  1. LISTEN  — bind a port, wait for connections
  2. ACCEPT  — establish a session per client
  3. PARSE   — deserialize bytes into structured commands  
  4. EXECUTE — apply commands to in-memory state (with locking)
  5. RESPOND — serialize results back to bytes
  6. PERSIST — write durable state to storage asynchronously

  Redis, PostgreSQL, MongoDB, and Memcached all follow this pattern.
  This project strips each layer down to its bare minimum — making it
  an ideal foundation for understanding what real servers do under the hood.
```

---

*Generated for study purposes. All code references are to the actual source files in this repository.*
