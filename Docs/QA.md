# Redis Server - Questions & Answers

---

## Q1: What is the Background Persistence Thread?

**Question:**
> What is this background thread? Please explain simply.

**Answer:**

The background persistence thread is a **"save your data to disk every 5 minutes" worker**.

### How It Works

1. **Main server thread** — accepts client connections, handles commands, stores data in memory
2. **Background thread** — runs separately, every 300 seconds (5 minutes):
   - Saves the entire database to a file (`dump.my_rdb`) ,in the mean time the data lives on RAM
   - Goes back to sleep for 5 more minutes
   - Repeats forever

### Why You Need It

If your server crashes or the power goes out, all in-memory data is lost.

- **Without this thread:** Server crashes → all data gone ❌
- **With this thread:** Server crashes → restart → load last saved snapshot from file ✅

### The Code

**Location:** main.cpp:19-28

```cpp
std::thread persistanceThread([](){
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(300));  // wait 5 min
        RedisDatabase::getInstance().dump("dump.my_rdb");         // save to disk
    }
});
persistanceThread.detach();  // let it run in background
```

### You Also Save on Shutdown

When you press `Ctrl+C`, a signal handler immediately saves the data before the server closes, so you don't lose recent changes.

### Analogy

Like autosave in Google Docs — every 5 minutes the document is backed up to the server, so if your computer crashes, you restart and your work is still there.

### Timeline

```
Server Start
    ↓
Background thread created
    ↓
Thread: sleep 5 minutes
    ↓
Thread: save database to dump.my_rdb
    ↓
Thread: sleep 5 minutes
    ↓
(repeats forever)

If server crashes:
    ↓
Restart server
    ↓
Load dump.my_rdb on startup
    ↓
Data is restored!
```

---

## Q2: What Does `.detach()` Do?

**Question:**
> What does this line of code do? Explain its concept to me.

**Answer:**

`.detach()` means **"let this thread run on its own, I don't care when it finishes"**.

### The Concept

When you create a thread, you have two choices for what happens to it:

```cpp
std::thread myThread([](){
    // some work
});

// Option 1: DETACH
myThread.detach();  // "Go run in background, I'm not waiting for you"

// Option 2: JOIN
myThread.join();    // "Wait until you finish, then continue"
```

### `.detach()` vs `.join()`

| Aspect | `.detach()` | `.join()` |
|--------|-----------|---------|
| **Thread lifecycle** | Runs independently | Main thread waits for it |
| **Main code behavior** | Doesn't wait, continues immediately | Blocks until thread finishes |
| **Ownership** | Thread becomes "orphaned" | Ownership is clear |
| **Control** | You lose control over it | You control when it ends |

### In Your Code

**Location:** main.cpp:19-28

```cpp
std::thread persistanceThread([](){
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(300));
        RedisDatabase::getInstance().dump("dump.my_rdb");
    }
});
persistanceThread.detach();  // "Run forever in background, I'm starting the server now"
```

**What happens:**
1. Thread is created
2. `.detach()` is called → thread runs on its own
3. Main code immediately continues → `server.run()` starts accepting clients
4. Meanwhile, the persistence thread keeps saving data every 5 minutes (forever)

### Visual Timeline

```
Main Thread                    Persistence Thread
─────────────────────────────  ──────────────────────────
Create thread      ──────→    Waiting...
.detach()          ──────→    (now running independently)
server.run()                   Sleep 300s
(accepting clients)            Save database
                              Sleep 300s
                              Save database
                              ... (forever)
```

### Why `.detach()` Here?

You **don't want the server to wait** for the persistence thread. You want:
- ✅ The server to start immediately and accept client connections
- ✅ The persistence thread to quietly save data in the background

### What Would Happen with `.join()`?

```cpp
persistanceThread.join();  // BAD: waits forever!
server.run();               // Never reached! Waits 5 minutes first
```

The server would wait 5 minutes before accepting any connections. Users would see the server as "not responding."

### Analogy

Detach is like **sending your friend on an errand and continuing your day** instead of waiting for them to finish.

- With `.join()`: You wait at home until they return ⏳
- With `.detach()`: You start your day while they run the errand 🏃

### ⚠️ Trade-off

**Downside:** Once detached, you lose control. The thread keeps running even after `main()` ends, which can cause crashes if resources are freed while the thread is still accessing them.

**In your case:** The signal handler calls `exit()` cleanly when Ctrl+C is pressed, so it's okay. The thread is forcefully terminated along with the entire process.

---

## Q3: How Does Static Know the Object is Already Created?

**Question:**
> But how does the code know that object is already created and skips creating a new object?

**Answer:**

This is a **compiler trick**. The C++ compiler generates hidden code behind the scenes to track whether a static variable has been initialized or not.

### What Actually Happens (Behind the Scenes)

Your code:
```cpp
RedisDatabase& RedisDatabase::getInstance() {
    static RedisDatabase instance;
    return instance;
}
```

The compiler transforms it into something like this:

```cpp
RedisDatabase& RedisDatabase::getInstance() {
    // Compiler-generated initialization flag (hidden from you)
    static bool __initialized = false;
    
    if (!__initialized) {  // ← Only true on FIRST call
        // Create the object ONLY on first call
        instance = new RedisDatabase();
        __initialized = true;
    }
    
    // All calls (first and subsequent) return the same object
    return instance;
}
```

### Step-by-Step Timeline

```
First call to getInstance():
    ↓
Check: Is __initialized true?
    ↓
No, it's false
    ↓
Execute: static RedisDatabase instance;  ← Create object
    ↓
Set __initialized = true
    ↓
Return instance


Second call to getInstance():
    ↓
Check: Is __initialized true?
    ↓
Yes, it's true!
    ↓
Skip: static RedisDatabase instance;  ← Don't create again
    ↓
Return instance (the one from first call)


Third, Fourth, Fifth calls... (same as second call)
```

### Real Memory Illustration

```cpp
First call:
  Memory: [  instance object created here  ]
  __initialized flag: false → true

Second call:
  Memory: [  same instance object - not created again  ]
  __initialized flag: already true, skip creation

Third call:
  Memory: [  same instance object - still the same one  ]
  __initialized flag: already true, skip creation
```

### Why This Works

The **`static` keyword** tells the compiler: 
- "Create this variable ONCE and keep it for the lifetime of the program"
- "Add an initialization guard flag"
- "Only initialize if it hasn't been initialized before"

This is **automatic**. The compiler does all the tracking for you—you don't write the `__initialized` flag yourself; the compiler generates it.

### Visual Code Execution

```cpp
#include <iostream>
using namespace std;

class Database {
public:
    static Database& getInstance() {
        static Database instance;  // ← Compiler adds tracking here
        cout << "getInstance called\n";
        return instance;
    }
};

int main() {
    cout << "Call 1:\n";
    Database& db1 = Database::getInstance();
    
    cout << "\nCall 2:\n";
    Database& db2 = Database::getInstance();
    
    cout << "\nCall 3:\n";
    Database& db3 = Database::getInstance();
    
    cout << "\nAre they the same object?\n";
    cout << "db1 == db2? " << (&db1 == &db2 ? "YES" : "NO") << "\n";
    cout << "db2 == db3? " << (&db2 == &db3 ? "YES" : "NO") << "\n";
    
    return 0;
}
```

**Output:**
```
Call 1:
getInstance called

Call 2:
getInstance called

Call 3:
getInstance called

Are they the same object?
db1 == db2? YES
db2 == db3? YES
```

All three calls return the **same object address** (`&db1 == &db2 == &db3`).

### In Your Redis Code

**Location:** RedisDatabase.cpp:9-12

```cpp
RedisDatabase& RedisDatabase::getInstance() {
    static RedisDatabase instance;  // Created once, on first call only
    return instance;                 // All calls return same instance
}
```

When called from different threads:

```
Thread 1: getInstance() → Creates instance + initialization flag
Thread 2: getInstance() → Sees flag is already set, skips creation
Thread 3: getInstance() → Sees flag is already set, skips creation
Thread 4: getInstance() → Sees flag is already set, skips creation

Result: All threads get the same database instance
```

### Key Takeaway

The **`static` keyword** is magic because:
1. C++ compiler automatically adds a **hidden initialization guard**
2. Compiler checks guard on every call
3. Guard is set to `true` after first initialization
4. On future calls, creation is skipped

**You don't write this logic yourself**—the compiler does it for you. That's why it's called the **"Meyers Singleton"** pattern—it's elegant and requires minimal code from you.

---

## Q4: What Does the `load()` Function Do?

**Question:**
> What does this load function do?

**Answer:**

The `load()` function reads the `dump.my_rdb` file and **restores the database into memory**. It's the reverse of `dump()` — the "restore from backup" function.

### What It Does Step by Step

**1. Opens the file**
```cpp
std::ifstream ifs(filename, std::ios::binary);
if (!ifs) return false;  // File doesn't exist → return false
```

**2. Clears existing in-memory data**
```cpp
kv_store.clear();
list_store.clear();
hash_store.clear();
```
Wipes everything in memory first so there's no mixing of old and new data.

**3. Reads the file line by line** and based on the first letter of each line, it knows what type of data to restore:

| First Letter | Data Type | Example Line |
|---|---|---|
| `K` | Key-Value string | `K username alice` |
| `L` | List | `L fruits apple banana orange` |
| `H` | Hash | `H user:1 name:alice age:30` |

### How Each Type is Loaded

**K (Key-Value):**
```
Line: "K username alice"
Result: kv_store["username"] = "alice"
```

**L (List):**
```
Line: "L fruits apple banana orange"
Result: list_store["fruits"] = ["apple", "banana", "orange"]
```

**H (Hash):**
```
Line: "H user:1 name:alice age:30"
Splits on ':' → field="name", value="alice" | field="age", value="30"
Result: hash_store["user:1"] = { name→alice, age→30 }
```

### The Big Picture

```
Server starts
    ↓
load("dump.my_rdb") is called
    ↓
File exists? → Read it → Rebuild database in memory
File missing? → Return false → Start with empty database
    ↓
Server is ready to accept clients with all previous data restored
```

**Location:** RedisDatabase.cpp:398-441

---

## Q5: What is `globalServer` and Why Do We Need It?

**Question:**
> Explain this line of code, what is globalServer and why do we need it, tell me its significance.

**Answer:**

### What is `globalServer`?

```cpp
static RedisServer* globalServer = nullptr;  // RedisServer.cpp:15
```

A **global pointer** that holds the address of the `RedisServer` object. Starts as `nullptr` (pointing to nothing).

### What Does `globalServer = this` Do?

```cpp
RedisServer::RedisServer(int port) : ... {
    globalServer = this;  // "store MY address in the global pointer"
}
```

`this` = the current `RedisServer` object being created. So when `RedisServer server(6379)` is called in `main.cpp`, this line stores the address of that `server` object into `globalServer`.

### Why Do We Need It? — The Core Problem

When you press `Ctrl+C`, the OS sends a **SIGINT signal** and calls your signal handler:

```cpp
void signalHandler(int signum) {
    globalServer->shutdown();  // need to call shutdown on the server
}
```

**The problem:** `signalHandler` is a plain C-style function. It has **no access to any object** — it can't see your `RedisServer server` variable in `main()`.

```
main()
  └── RedisServer server(6379)   ← lives here, signal handler can't see it!

signalHandler()                  ← completely separate, isolated function
  └── ??? how do I reach `server`?
```

**The solution:** Store the server's address in a global variable so the signal handler can reach it.

```
main()
  └── RedisServer server(6379)
        └── globalServer = &server  ← store address globally

signalHandler()
  └── globalServer->shutdown()      ← now it can reach the server!
```

### Full Flow When You Press Ctrl+C

```
You press Ctrl+C
    ↓
OS sends SIGINT signal
    ↓
signalHandler(SIGINT) is called
    ↓
globalServer is not nullptr? → YES
    ↓
globalServer->shutdown()
    ↓
Server saves database to dump.my_rdb
    ↓
Server closes socket
    ↓
exit() — program ends cleanly
```

### What Happens Without `globalServer`?

```cpp
void signalHandler(int signum) {
    // No access to server object!
    // Can't call shutdown()
    // Can't save database
    exit(signum);  // abrupt exit — data loss!
}
```

Without it, pressing `Ctrl+C` would kill the server immediately without saving any data to disk.

### Summary Table

| | Detail |
|---|---|
| **What it is** | A global pointer to the `RedisServer` object |
| **Set in** | Constructor: `globalServer = this` |
| **Used in** | `signalHandler()` to call `shutdown()` |
| **Why needed** | Signal handlers are plain C functions with no access to C++ objects |
| **Without it** | Ctrl+C kills server abruptly, unsaved data is lost |

**Location:** RedisServer.cpp:15-31

---

## Q6: Why is `setupSignalHandler()` Called During Server Object Creation?

**Question:**
> Why is this function called during server object creation?

**Answer:**

### What It Does

```cpp
void RedisServer::setupSignalHandler() {
    signal(SIGINT, signalHandler);  // "when Ctrl+C happens, call signalHandler"
}
```

It tells the OS: *"if you receive a SIGINT signal, run `signalHandler` instead of the default behavior (abrupt kill)"*.

### Why in the Constructor Specifically?

Look at the order of events in `main.cpp`:

```cpp
RedisServer server(port);   // ← constructor runs here
                            //   1. globalServer = this
                            //   2. setupSignalHandler()  ← registered HERE

server.run();               // ← server starts accepting clients HERE
```

The signal handler must be registered **BEFORE `server.run()`** because:

- The moment `server.run()` starts, clients can connect and data can come in
- If user presses `Ctrl+C` during `run()` and the handler isn't registered yet → OS kills the process abruptly → **data loss**
- By registering in the constructor, you guarantee it's set up before a single client connects

### What Happens Without This?

```
Default OS behavior for Ctrl+C (SIGINT):
    → Immediately kill the process
    → No shutdown() called
    → No database saved
    → All data lost ❌
```

```
With setupSignalHandler():
    → Ctrl+C intercepted
    → signalHandler() called
    → shutdown() called
    → database saved ✅
    → clean exit ✅
```

### Analogy

It's like setting up a **fire alarm before opening a restaurant**. You don't open the doors to customers and then install the alarm — you install it first so it's ready the moment anyone walks in.

**Location:** RedisServer.cpp:25-31

---

## Q7: Is `signal()` an OS-Level Function That Registers Our Custom Handler?

**Question:**
> Is this the OS level function that makes the OS aware about our server and we also pass our specific handler function which it has to execute on Ctrl+C?

**Answer:**

Yes, exactly right! `signal()` is a **C standard library function** (provided by the OS) that registers your custom function with the OS.

### Breaking Down the Line

```cpp
signal(SIGINT, signalHandler);
//     ↑        ↑
//     |        your custom function to run
//     which signal to listen for
```

- **`signal()`** — OS-level function, tells the OS "I want to handle this signal myself"
- **`SIGINT`** — the signal code for `Ctrl+C` (Signal Interrupt)
- **`signalHandler`** — your function that the OS will call when it receives that signal

### The Contract Between Your Code and the OS

```
Your code says:
"Hey OS, when you receive SIGINT, don't kill me.
 Instead, call MY function: signalHandler()"

OS says:
"Got it. I'll remember that."

User presses Ctrl+C:
OS says:
"I got SIGINT. I promised to call signalHandler(). Calling it now..."
```

### Without `signal()`

```
User presses Ctrl+C
    ↓
OS receives SIGINT
    ↓
OS uses DEFAULT behavior → kills process immediately
    ↓
No chance to save data ❌
```

### With `signal()`

```
User presses Ctrl+C
    ↓
OS receives SIGINT
    ↓
OS remembers: "they registered a custom handler"
    ↓
OS calls signalHandler()
    ↓
Your code runs → saves database → clean shutdown ✅
```

So `signal()` is exactly the **bridge between your application and the OS**, and `signalHandler` is your callback that the OS executes when the event happens.

**Location:** RedisServer.cpp:26

---

## Q8: What is the `socket()` Function and What Does It Do?

**Question:**
> Explain this line of code, what is it and what does it do?

```cpp
extern int socket(int __domain, int __type, int __protocol) __THROW;
```

**Answer:**

This is a **declaration** in the OS header file telling your C++ code that this function exists inside the OS kernel.

```cpp
extern int socket(int __domain, int __type, int __protocol) __THROW;
//     ↑           ↑             ↑            ↑
//  returns        what          connection   protocol
//  socket ID      network       style        (usually 0)
```

- **`extern`** — "this function is defined elsewhere" (inside the OS, not your code)
- **`__THROW`** — compiler hint that this function won't throw C++ exceptions

### What It Actually Does

When called, it asks the OS to **create a communication endpoint** (a socket) — think of it as creating a phone jack that clients can plug into.

In your server ([RedisServer.cpp:48](src/RedisServer.cpp#L48)):
```cpp
server_socket = socket(AF_INET, SOCK_STREAM, 0);
//                     ↑        ↑             ↑
//                     IPv4     TCP           auto-select protocol
```

| Argument | Value | Meaning |
|---|---|---|
| `__domain` | `AF_INET` | Use IPv4 internet network |
| `__type` | `SOCK_STREAM` | TCP — reliable, ordered connection |
| `__protocol` | `0` | Let OS pick the right protocol automatically |

**Returns:** an integer (the socket file descriptor). If `-1`, creation failed.

### Big Picture

```
socket()
    ↓
OS creates a socket
    ↓
Returns an ID (e.g. 5)
    ↓
bind()   → attach it to port 6379
    ↓
listen() → start waiting for clients
    ↓
accept() → client connects!
```

`socket()` is always **step 1** — without it, none of the networking works. The header line is just the **declaration** that tells your compiler the function signature; the actual implementation lives deep inside the OS kernel.

**Location:** /usr/include/x86_64-linux-gnu/sys/socket.h:102

---

## Q9: What is `setsockopt()` and How Does It Work?

**Question:**
> Explain `setsockopt()` — what it is, why it exists, what its arguments mean, and how values like `timeval` work.

**Answer:**

### What It Is

`setsockopt()` is an **OS system call** that lets you customize a socket's behavior. When you create a socket with `socket()`, the OS gives it default settings that don't always work for servers. `setsockopt()` lets you override those defaults.

```cpp
extern int setsockopt(int __fd, int __level, int __optname,
                      const void *__optval, socklen_t __optlen) __THROW;
//                    ↑         ↑            ↑                ↑
//                  socket ID  which layer  which option     value to set
```

### How It's Used in Your Server

**Location:** [RedisServer.cpp:55](src/RedisServer.cpp#L55)
```cpp
int opt = 1;
setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

| Argument | Value | Meaning |
|---|---|---|
| `__fd` | `server_socket` | Which socket to configure |
| `__level` | `SOL_SOCKET` | Configure at the socket level |
| `__optname` | `SO_REUSEADDR` | WHICH option to change |
| `__optval` | `&opt` (= 1) | The VALUE for that option (1 = ON, 0 = OFF) |
| `__optlen` | `sizeof(opt)` | How many bytes to read from the pointer |

### Why Both `&opt` and `sizeof(opt)` Are Needed

`setsockopt()` is a **generic function** that handles ALL socket options. Different options take different value types:

```cpp
// Boolean option — value is just int (1 or 0)
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

// Timeout option — value is a struct
struct timeval timeout = {5, 0};
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
```

Since the OS accepts `void*` (a pointer to anything), it has no idea how many bytes to read without being told explicitly. That's why both are required:
- **`&opt`** = raw pointer to the value (the envelope)
- **`sizeof(opt)`** = how many bytes to read (how many pages are in the envelope)

### `opt` Does NOT Identify the Option

A common confusion — `opt` is just the value, not the identifier:
```cpp
// SO_REUSEADDR = identifies WHICH option to change
// &opt (= 1)   = the VALUE to set for that option
setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

You could name the variable anything:
```cpp
int on = 1;
setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));  // same thing
```

### Why These Options Exist in the First Place

Your code doesn't control the network — **the OS does**. `setsockopt()` is how you tell the OS to change its behavior for your specific socket without affecting others.

| Problem | Default OS Behavior | Fix with `setsockopt()` |
|---|---|---|
| Server crashes & restarts | Port locked for ~60s | `SO_REUSEADDR` → restart instantly |
| Client goes silent forever | Connection stays open forever | `SO_KEEPALIVE` → detect dead clients |
| Large data transfers slow | Small receive buffer | `SO_RCVBUF` → increase buffer size |
| Need response within 5s | Wait forever | `SO_RCVTIMEO` → set timeout |

**Analogy:** `socket()` = buy a car with factory defaults. `setsockopt()` = customize the seat, AC, and speed settings.

### What is `struct timeval`?

The OS defines `timeval` for representing time with sub-second precision:

```cpp
struct timeval {
    long tv_sec;   // seconds
    long tv_usec;  // microseconds (millionths of a second)
};

struct timeval timeout = {5, 0};   // 5 seconds, 0 microseconds = 5.0s exactly
struct timeval timeout = {5, 500000}; // 5 seconds + 500,000 microseconds = 5.5s
struct timeval timeout = {0, 100000}; // 0 seconds + 100,000 microseconds = 0.1s
```

Two fields are needed because a single `int seconds` can't express sub-second values like "wait 1.5 seconds" — you need `1 second + 500,000 microseconds`.

```
1 second = 1,000,000 microseconds
```

### Compared to `socket()`

| | `socket()` | `setsockopt()` |
|---|---|---|
| **Purpose** | Create the socket | Configure the socket |
| **When called** | Step 1 | Step 2, right after creation |
| **Analogy** | Buy a phone | Adjust its settings |

**Location:** RedisServer.cpp:54-55

---

## Summary

| # | Question | Key Takeaway |
|---|----------|--------------|
| 1 | What is the background persistence thread? | Saves database every 5 minutes so data isn't lost on server crash |
| 2 | What does `.detach()` do? | Lets the persistence thread run independently while server accepts clients immediately |
| 3 | How does static know object is created? | Compiler adds hidden initialization flag; guard is set to true after first creation |
| 4 | What does `load()` do? | Reads dump file on startup and rebuilds the entire database in memory |
| 5 | What is `globalServer`? | Global pointer that lets the signal handler reach the server object to shut down cleanly |
| 6 | Why is `setupSignalHandler()` in constructor? | Signal handler must be ready before server starts accepting clients to prevent data loss on Ctrl+C |
| 7 | Is `signal()` an OS-level function? | Yes — it's the bridge between your app and the OS; registers your callback so OS calls it instead of killing the process |
| 8 | What is the `socket()` function? | OS system call that creates a network communication endpoint; step 1 of all networking |
| 9 | What is `setsockopt()` and how does it work? | OS call to customize socket behavior; option name identifies what to change, value sets it, size tells OS how many bytes to read |
| 10 | What is `sockaddr_in` and what does it configure? | A struct that tells the OS where to listen — network type, port (in network byte order), and which interfaces to accept from |
| 11 | How do `bind()` and `listen()` work? | `bind()` claims the address, `listen()` opens the doors with a small backlog queue (10) for waiting clients while active connections stay unlimited |
| 12 | What does `accept()` do? | Blocks until a client connects, then creates a new dedicated socket for that client while `server_socket` keeps listening for others |
| 13 | How does the per-client thread work? Is the loop wasteful when idle? | Each client gets a dedicated thread running recv→process→send in a loop. Not wasteful — `accept()` blocks at ~0% CPU until a real client connects |
| 14 | How does `accept()`/OS know a client connected? Will early messages be lost? | TCP 3-way handshake done by OS kernel triggers `accept()`. Messages aren't lost — OS buffers incoming data until `recv()` reads it |
| 15 | How does a client connect? What if it sends nothing? ⚠️ **IMPORTANT** | Client uses `socket()` + `connect()` to trigger handshake. If client sends nothing, `recv()` blocks forever → **slow loris vulnerability** — fix with `SO_RCVTIMEO` or async I/O |
| 16 | How does the RESP parser work? ⚠️ **IMPORTANT** | Walks input byte-by-byte, reads `*N` (array size) and `$N` (string length) markers to slice raw bytes into clean strings; length prefixes avoid issues with spaces/special chars |
| 17 | Command dispatcher and RESP response patterns ⚠️ **IMPORTANT** | Dispatcher uppercases command + routes via if/else; 3 main response types: simple string (`+`), bulk string (`$len`/`$-1`), array (`*N`); all handlers follow validate→call→format template |
| 18 | How does `purgeExpired()` work? | Lazy eviction — walks `expiry_map`, erases past-deadline keys from all 3 stores; called inside get/keys/type/del/expire/rename; O(N) and missing from list/hash ops |
| 19 | `steady_clock` vs `system_clock` — why it matters for TTLs | `steady_clock` is monotonic (only forward), immune to NTP/DST/admin changes. `system_clock` is wall time — breaks TTLs when clock jumps |
| 20 | What is `std::lock_guard<std::mutex>`? | RAII wrapper that locks on construction, auto-unlocks on scope exit (even on exceptions). Standard safe way to use mutexes in modern C++ |
| 21 | How does `lrem()` work? (most complex function) | 3 modes: `count=0` uses erase-remove idiom, `count>0` forward iteration, `count<0` reverse iteration with `.base()-1` dance to convert reverse↔forward iterators |
| 22 | `lindex`/`lset` — negative indexing and why it matters | Negative index converted via `size + index`, two-stage bounds check; negative indexing is critical for atomicity (no race), single round-trip, cleaner code, natural mental model |
| 23 | How do `dump()` / `load()` work? (persistence) | Custom text format with K/L/H type tags; uses `ofstream`/`ifstream` + `istringstream`. ⚠️ Spaces/colons in values silently corrupt data — fix with length-prefixed encoding. Real Redis uses binary RDB + AOF |

---

## Q10: What is `sockaddr_in` and What Does It Do?

**Question:**
> What is this, what does it do and why?

```cpp
sockaddr_in serverAddr{};
serverAddr.sin_family = AF_INET;
serverAddr.sin_port = htons(port);
serverAddr.sin_addr.s_addr = INADDR_ANY;
```

**Answer:**

This block **tells the OS where your server should listen** — which network and which port. Think of it as filling out a form before handing it to `bind()`.

### Line by Line

**Line 1:**
```cpp
sockaddr_in serverAddr{};
```
Creates a struct that holds the server's address info. `{}` zero-initializes all fields (clears any garbage values from memory).

---

**Line 2:**
```cpp
serverAddr.sin_family = AF_INET;
```
"Use **IPv4** networking" (not IPv6 which would be `AF_INET6`).

---

**Line 3:**
```cpp
serverAddr.sin_port = htons(port);
```
Sets the port number. But why `htons()`?

Different CPUs store bytes in different orders:
- Your CPU (x86): stores `6379` as `[0x18, 0xEB]` → **little-endian**
- Network standard: expects `[0xEB, 0x18]` → **big-endian**

`htons()` = **H**ost **T**o **N**etwork **S**hort — converts your CPU's byte order to network byte order so all machines on the internet agree.

```
Without htons: port 6379 → sent as 14232 on network ❌
With htons:    port 6379 → sent as 6379 on network  ✅
```

---

**Line 4:**
```cpp
serverAddr.sin_addr.s_addr = INADDR_ANY;
```
"Listen on **all network interfaces**" — accept connections from anywhere (localhost, LAN, internet), not just one specific IP.

```
INADDR_ANY  = 0.0.0.0 = listen on ALL interfaces ✅
specific IP = 127.0.0.1 = only localhost
```

---

### The Full Picture

```cpp
sockaddr_in serverAddr{};                    // create address struct
serverAddr.sin_family = AF_INET;             // IPv4
serverAddr.sin_port = htons(port);           // port 6379 (network byte order)
serverAddr.sin_addr.s_addr = INADDR_ANY;     // accept from anywhere

bind(server_socket, &serverAddr, sizeof(serverAddr));
// ↑ hand this filled form to OS so it knows where to listen
```

Like **filling out a form** before submitting it to the OS via `bind()`:
```
Form: "Where should this server listen?"
  ✓ Network type: IPv4
  ✓ Port: 6379
  ✓ Accept from: everywhere
→ Submit form to OS via bind()
→ OS starts listening at that address
```

**Location:** RedisServer.cpp:57-60

---

## Q11: How Do `bind()` and `listen()` Work?

**Question:**
> Briefly explain `bind()` and `listen()` — and the questions that came up about the backlog limit.

**Answer:**

### Q11.1: What does `bind()` do?

```cpp
bind(server_socket, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
```

Tells the OS: *"attach my socket to this specific address (port 6379, all interfaces)"*. After this, the OS knows "socket ID 5 owns port 6379". Returns `< 0` if port is already taken.

The cast `(struct sockaddr*)` exists because `bind()` accepts a generic address type — IPv4, IPv6, Unix sockets all share it. So you cast your specific `sockaddr_in` (IPv4) to the generic `sockaddr*`.

---

### Q11.2: What does `listen()` do?

```cpp
listen(server_socket, 10);
```

Tells the OS: *"start accepting incoming connections; queue up to 10 waiting clients"*. Switches the socket from **idle** → **listening mode**. The `10` is the **backlog** — max waiting queue size.

---

### Q11.3: After 10 waiting clients, are new requests rejected?

Yes — but only the **waiting queue** is limited, not active connections.

```
Active connections (already accepted): UNLIMITED ✅
Pending in queue waiting:              max 10 → 11th gets rejected ❌
```

If queue fills, new clients get **"Connection refused"**.

---

### Q11.4: Active connections vs. backlog — what's the difference?

A client goes through 2 stages:

```
Stage 1: WAITING IN LINE         Stage 2: INSIDE
("not accepted yet")              ("accept() called for me")
        ↓                                  ↓
   Limited to 10                     UNLIMITED
   (backlog queue)                   (active connections)
```

**Restaurant analogy:**
- **Bench outside** = backlog queue (10 seats)
- **Tables inside** = active connections (unlimited)
- **Host** = `accept()` function — walks people from bench to table

People already eating don't care about the bench. There can be 1000 eating while 5 wait outside.

---

### Q11.5: Why set queue to only 10 when active connections are unlimited?

Because the queue has a **different purpose** than total capacity. It's a **shock absorber for sudden bursts** — for those microseconds when many clients arrive simultaneously and `accept()` hasn't picked them up yet.

Why NOT make the queue huge:

| Reason | Explanation |
|---|---|
| **Memory waste** | Each queued slot reserves OS state (~few KB) |
| **Hides real problems** | A full queue means `accept()` is too slow — fix that, don't grow the queue |
| **Lies to clients** | A million-slot queue makes clients wait 5 minutes instead of being rejected fast — fast rejection is more honest |

The real bottleneck is **active connections** (RAM, file descriptors, CPU), not the queue. The queue of 10 is fine because `accept()` drains it in milliseconds.

**Production note:** Real Redis uses backlog 511 for high-traffic moments. For your project, 10 is fine.

---

### Big Picture (Restaurant Analogy)

```
socket()    → Build the restaurant
setsockopt() → Configure rules (allow re-entry, etc.)
bind()      → Claim address: "123 Main Street, Suite 6379"
listen()    → Open doors, set up 10-person waiting line
accept()    → Greet each customer as they walk in
```

**Location:** RedisServer.cpp:62-70

---

## Q12: What Does `accept()` Do?

**Question:**
> Explain `accept(server_socket, nullptr, nullptr)`.

**Answer:**

This is the line where your server **actually starts talking to a client**.

### What It Does (3 things)

1. **Waits (blocks)** until a client tries to connect
2. **Pulls one client** from the backlog queue
3. **Creates a brand new socket** dedicated to that specific client

```
Before accept():
  server_socket (port 6379) ← listening, waiting

Client connects → enters backlog queue

After accept():
  server_socket (port 6379) ← still listening for OTHER clients
  client_socket             ← NEW socket, used only for this one client
```

### Two Sockets Now Exist

| Socket | Purpose |
|---|---|
| `server_socket` | Keeps listening for **new** clients (never used for data) |
| `client_socket` | Used to **read/write data** with THIS specific client |

`server_socket` is the **receptionist**, each `client_socket` is a dedicated **employee** assigned to one customer.

### Why Two `nullptr` Arguments?

```cpp
accept(server_socket, nullptr, nullptr);
//                    ↑        ↑
//                    addr     addr_len
```

If you provide an address struct, `accept()` fills it with the **client's IP and port** so you know who connected. Passing `nullptr, nullptr` means *"I don't care who's connecting, just give me the socket"*.

To know the client's IP, you'd write:
```cpp
sockaddr_in clientAddr;
socklen_t len = sizeof(clientAddr);
accept(server_socket, (sockaddr*)&clientAddr, &len);
// now clientAddr.sin_addr has the client's IP
```

### The Full Server Loop

```cpp
while (running) {
    int client_socket = accept(server_socket, nullptr, nullptr);  // wait for client
    
    threads.emplace_back([client_socket](){                       // spawn thread
        // talk to this client over client_socket
    });
}
```

```
Loop forever:
  accept() blocks → waits
  Client connects → accept() returns new socket
  Spawn thread to handle that client
  Loop back → accept() blocks again for next client
```

### Return Value

- **Success:** returns a new socket ID (positive number)
- **Failure:** returns `-1` (which is why `if (client_socket < 0)` checks for errors)

### Restaurant Analogy

```
server_socket = front door (always open, always waiting)
accept()      = greeting a customer at the door, walking them to a table
client_socket = the table assigned to that specific customer
```

Each accepted client gets their **own dedicated socket** so multiple conversations can happen at once without mixing up data.

**Location:** RedisServer.cpp:78

---

## Q13: How Does the Per-Client Thread Work?

**Question:**
> Explain the thread that handles each connected client, and isn't this architecture wrong since it creates client sockets unnecessarily when no one is connected?

**Answer:**

### Q13.1: What does the per-client thread do?

For every accepted client, the server spawns a **dedicated thread** that handles only that client — reading commands and sending responses in a loop until the client disconnects.

```cpp
threads.emplace_back([client_socket, &cmdHandler](){
    char buffer[1024];
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) break;
        std::string request(buffer, bytes);
        std::string response = cmdHandler.processCommand(request);
        send(client_socket, response.c_str(), response.size(), 0);
    }
    close(client_socket);
});
```

### Q13.2: Why capture by value vs reference?

```cpp
[client_socket, &cmdHandler]
//  ↑               ↑
// by VALUE     by REFERENCE
```

- `client_socket` by **value** → each thread gets its own copy (different per client)
- `&cmdHandler` by **reference** → all threads share the same handler (it's stateless)

### Q13.3: What does each line do?

| Line | Purpose |
|---|---|
| `char buffer[1024]` | Temporary 1KB storage for incoming bytes |
| `memset(buffer, 0, ...)` | Wipe old data before each read |
| `recv(...)` | Blocks waiting for client to send data; returns bytes read |
| `if (bytes <= 0) break` | Client disconnected or error → exit loop |
| `std::string request(buffer, bytes)` | Convert raw bytes to C++ string |
| `cmdHandler.processCommand(...)` | Parse RESP, execute command, return response string |
| `send(...)` | Write response back to client |
| `close(client_socket)` | Free socket resources after client leaves |

### Q13.4: Conversation flow

```
Client connects → accept() returns client_socket → spawn thread
                                                       ↓
                                            ┌──────────────────┐
                                            │  Per-Client Loop │
                                            │  1. recv()       │ ← wait for command
                                            │  2. parse        │
                                            │  3. process      │
                                            │  4. send reply   │
                                            │  5. repeat       │
                                            └──────────────────┘
                                                       ↓
                                            Client disconnects
                                                       ↓
                                            close(socket) + thread ends
```

### Q13.5: Isn't this architecture wasteful — creating sockets when no one is connected?

**No** — because `accept()` is a **blocking call**.

When no clients are connected, the loop **pauses inside `accept()`** consuming ~0% CPU. The OS only wakes the thread when a real client actually connects.

```
while (running) {
    accept(...);   ← THREAD IS PAUSED HERE, doing nothing
                   ← CPU usage: ~0%
                   ← No sockets created
}
```

#### What you imagined (wrong):
```
Loop iteration 1: create socket, no client, close → wasted ❌
Loop iteration 2: create socket, no client, close → wasted ❌
... burns CPU at 100%
```

#### What actually happens:
```
accept() ────────────────────────── (sleeping, no CPU)
                                       ↓
                              Client connects!
                                       ↓
                              accept() returns NEW socket
                                       ↓
                              spawn thread, talk to client
                                       ↓
                              loop back to accept() ───── (sleeping again)
```

The OS handles waiting via an **event-driven mechanism** — your thread doesn't poll, it sleeps until the OS pokes it.

### Q13.6: Why one thread per client?

Without threading, the server could only handle **one client at a time** — everyone else would wait. With a thread per client, multiple clients run in parallel:

```
Client A: SET foo bar  ← Thread A handles this
Client B: GET name     ← Thread B handles this simultaneously
Client C: LPUSH list 1 ← Thread C handles this simultaneously
```

### Real Architectural Issues (For Interview Awareness)

Although the "wasted sockets" concern is wrong, the architecture *does* have real flaws:

| Issue | Why it matters |
|---|---|
| Fixed 1024-byte buffer | Truncates messages over 1KB |
| `threads` vector grows forever | Memory leak |
| 1 thread per client doesn't scale | Real Redis uses async I/O (`epoll`) for 10K+ clients |

**Location:** RedisServer.cpp:77-97

---

## Q14: How Does `accept()` / the OS Know a Client Has Connected? Will Early Messages Be Lost?

**Question:**
> How does accept/OS know a client has connected — is it when client sends PING/GET/SET? And if so, wouldn't that message be lost since no socket or thread exists yet?

**Answer:**

### Connection ≠ Message

In TCP, **connecting is a separate step** from **sending data**. A client doesn't just "send PING" out of nowhere — it must first **establish a connection** via the TCP handshake.

```
Step 1: Client establishes connection  ← accept() responds to THIS
Step 2: Client sends "PING"           ← recv() reads this LATER
```

### The TCP 3-Way Handshake (Done by OS Kernel)

Before any data is sent, the client and OS perform a handshake:

```
Client                              Server (OS kernel)
  │ ──── SYN ─────────────────────→  │  "Hi, can I connect?"
  │ ←──── SYN-ACK ────────────────  │  "Yes, let's connect"
  │ ──── ACK ────────────────────→  │  "Great, we're connected"
  │                                       │
  │       ✅ Connection established       │
  │                                  accept() wakes up!
  │                                  Returns new socket
```

This handshake is handled **entirely by the OS kernel** — your code doesn't see it. The OS does it BEFORE `accept()` returns.

### Who Handles the Handshake?

The **`server_socket`** does, after you called `listen()`.

```
listen() = "OS, please handle handshakes for me and queue connected clients"
accept() = "Give me the next ready client from the queue"
```

### Will Early Messages Be Lost? — NO

Every socket has an **OS-level receive buffer** (~64 KB by default). Any data arriving before you call `recv()` is stored there waiting for you. Nothing is dropped.

```
Time 5ms:   Handshake done, connection established
Time 5.1ms: Client sends "PING"   ← arrives before your thread is ready
            ↓
            OS stores "PING" in the socket's RECEIVE BUFFER
            ↓
Time 7ms:   Your thread calls recv()
            ↓
            recv() pulls "PING" from the buffer  ✅ NOT lost
```

**TCP is reliable by design** — that's its main selling point over UDP. Nothing gets lost in the gap between connection and `recv()`.

### Visual Architecture

```
                  ┌──────────────────────────────────────┐
                  │           OS Kernel                  │
   Client ──TCP─→ │  ┌─────────────────────┐            │
                  │  │  server_socket       │            │
                  │  │  (does handshake)    │            │
                  │  └──────────┬──────────┘            │
                  │             ↓                        │
                  │  ┌─────────────────────┐            │
                  │  │  Backlog Queue      │            │
                  │  └──────────┬──────────┘            │
                  └─────────────┼────────────────────────┘
                                ↓
                        accept() picks one
                                ↓
                  ┌─────────────────────────┐
                  │  client_socket          │
                  │  ┌──────────────────┐   │
                  │  │ Receive Buffer   │   │ ← messages stored here
                  │  │ [PING] [SET k v] │   │   even before recv()
                  │  └──────────────────┘   │
                  └─────────────────────────┘
```

### Summary Table

| Concern | Reality |
|---|---|
| "How does OS know a client connected?" | TCP 3-way handshake — done entirely by OS kernel via `server_socket` |
| "Is it the first message?" | No — connection (SYN packet) is separate from data |
| "Will messages sent too early be lost?" | No — OS stores them in the socket's receive buffer until `recv()` reads them |

### Phone Call Analogy

```
Connection (TCP handshake):
  - You dial → phone rings → person picks up
  - "Hello?" "Hi!" ← connection established

Data (recv/send):
  - If they say "hi" before you put phone to ear,
    the sound was still transmitted — you just heard it slightly later
  - Your brain (recv) processes the sound when ready
```

The OS is like your ear — it captures all sound (data) even if your brain (your thread) hasn't processed it yet.

---

## Q15: How Does a Client Connect, and What If a Client Connects But Sends Nothing?

**Question:**
> How does a client connect (what does it do for OS to know it wants to connect)? And what if a client connects but never sends any data?

**Answer:**

### Q15.1: How Does a Client Actually Connect?

The client uses the **same `socket()` and `connect()`** system calls as the server — just in reverse roles.

```cpp
// CLIENT-SIDE CODE
int sock = socket(AF_INET, SOCK_STREAM, 0);    // 1. create own socket

sockaddr_in serverAddr;
serverAddr.sin_family = AF_INET;
serverAddr.sin_port = htons(6379);             // server's port
serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1"); // server's IP

connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr));  // 2. triggers TCP handshake

send(sock, "PING\r\n", 6, 0);                  // 3. now send data
```

### What Happens Step by Step

```
CLIENT                                    SERVER
                                          
socket()      → creates client socket    socket() bind() listen() (already done)
                                         accept()         (waiting/blocked)
                                          
connect(IP, port)                         
   ↓                                      
   Sends SYN packet  ──────────────────→  OS receives SYN
                                          OS sends back SYN-ACK
   ←──────────────  Receives SYN-ACK     
   Sends ACK packet ──────────────────→  OS receives ACK
                                          OS creates client_socket internally
                                          accept() WAKES UP, returns client_socket
   Connection established  ✅            
   
send("PING") ────────────────────────→   recv() reads "PING"
```

### Real Example with `redis-cli`

When you type `redis-cli -p 6379`, under the hood it:
1. Calls `socket()` to create its own socket
2. Calls `connect("127.0.0.1", 6379)` to your server
3. Waits for handshake to complete
4. Now you can type commands → sends them via `send()`

**Bottom line:** The client **initiates** with `connect()`, the server **responds** via `accept()`.

---

### Q15.2: ⚠️ IMPORTANT — What If Client Connects But Sends Nothing?

This is a **real architectural flaw** in your server.

```cpp
char buffer[1024];
while (true) {
    int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    //               ↑ blocks here FOREVER if client sends nothing
    if (bytes <= 0) break;
    ...
}
```

`recv()` is **blocking** — it waits indefinitely.

### What Happens

```
Client connects → accept() returns → spawn thread
                                         ↓
                                    recv() blocks
                                         ↓
                                    Client sends NOTHING
                                         ↓
                                    Thread waits FOREVER ⏳
                                    Consumes:
                                    - 1 thread (~1 MB stack)
                                    - 1 file descriptor
                                    - 0% CPU (sleeping)
```

### Why This Is a Problem — "Slow Loris" Attack

```
Attacker connects 10,000 times but sends nothing
    ↓
10,000 threads spawned, all sleeping in recv()
    ↓
~10 GB RAM used
    ↓
File descriptor limit hit (~1024 default)
    ↓
Server can't accept any more REAL clients ❌ — DENIAL OF SERVICE
```

This vulnerability is called a **"slow loris" attack** — exhaust server resources by opening connections and never sending data.

### How Real Servers Defend Against It

#### 1. **Receive Timeout (`SO_RCVTIMEO`)**
```cpp
struct timeval timeout = {30, 0};  // 30 seconds
setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
```
After 30s silence → `recv()` returns -1 → loop breaks → socket closes.

#### 2. **TCP Keepalive (`SO_KEEPALIVE`)**
```cpp
int keepalive = 1;
setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
```
OS periodically pings the client. No response → connection killed.

#### 3. **Async I/O (`epoll` / `io_uring`)**
Real Redis uses **one thread + epoll** to handle thousands of idle connections with almost no overhead.

```
Thread-per-client (your server):  10,000 idle clients = 10,000 threads = bad
epoll (real Redis):              10,000 idle clients = 1 thread = great
```

### Your Server's Vulnerabilities

Your server has **none of these protections**:
- ❌ No `SO_RCVTIMEO` set → `recv()` blocks forever
- ❌ No `SO_KEEPALIVE` set → dead clients leak threads
- ❌ Thread per client → vulnerable to slow loris attacks

### Quick Fix

Add a timeout right after `accept()`:

```cpp
int client_socket = accept(server_socket, nullptr, nullptr);

// Add this:
struct timeval timeout = {300, 0};  // 5 minutes
setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
```

Now idle clients get kicked after 5 minutes, freeing the thread.

### Summary

| Question | Answer |
|---|---|
| How does a client connect? | Client calls `socket()` + `connect()` → triggers TCP handshake → server's `accept()` returns |
| What if client sends nothing? | ⚠️ `recv()` blocks forever, thread is stuck — real vulnerability called **slow loris**. Real servers use timeouts, keepalive, or async I/O |

---

## Q16: ⚠️ IMPORTANT — How Does the RESP Parser Work?

**Question:**
> Explain the RESP parser simply.

**Answer:**

The job of this parser is to **convert raw bytes from a client into a clean list of strings**.

```
INPUT:  "*2\r\n$4\r\nPING\r\n$4\r\nTEST\r\n"
OUTPUT: ["PING", "TEST"]
```

### Understanding RESP Format First

RESP is a **text format** with special markers:

```
*2\r\n          ← "array with 2 elements"
$4\r\n          ← "next string is 4 characters long"
PING\r\n        ← the actual string
$4\r\n          ← "next string is 4 characters long"
TEST\r\n        ← the actual string
```

Three markers to remember:
- `*N` = "expect N strings after this"
- `$N` = "next string is N bytes long"
- `\r\n` = separator (carriage return + line feed)

### Parser Step by Step

#### Step 1: Check the Format

```cpp
if (input[0] != '*') { 
    std::istringstream iss(input);
    std::string token;
    while (iss >> token) 
        tokens.push_back(token);
    return tokens;
}
```

**Fallback for inline format:** If input doesn't start with `*`, split by spaces. Handles when someone uses telnet to type commands manually.

```
Input:  "PING TEST"     ← inline format (not RESP)
Output: ["PING", "TEST"]
```

#### Step 2: Read Array Header

```cpp
size_t pos = 0;
pos++; // skip '*'

size_t crlf = input.find("\r\n", pos);
int numElements = std::stoi(input.substr(pos, crlf - pos));
pos = crlf + 2;
```

**What this does:**
1. Skip past `*`
2. Find the next `\r\n`
3. Read the number between them
4. Move position past `\r\n`

#### Step 3: Read Each String (Loop)

```cpp
for (int i = 0; i < numElements; i++) {
    if (input[pos] != '$') break;
    pos++; // skip '$'

    crlf = input.find("\r\n", pos);
    int len = std::stoi(input.substr(pos, crlf - pos));
    pos = crlf + 2;

    std::string token = input.substr(pos, len);
    tokens.push_back(token);
    pos += len + 2;
}
```

For each expected string, do the same pattern:
- Skip `$`
- Read the length
- Read exactly that many bytes
- Skip past the data + `\r\n`

### Complete Walkthrough Diagram

```
Input bytes:   *  2  \r \n $  4  \r \n P  I  N  G  \r \n $  4  \r \n T  E  S  T  \r \n
Position:      0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23

Step 1: Read "*2"  → numElements = 2
Step 2: At pos 4, read "$4" → next string is 4 bytes
        Read 4 bytes from pos 8 → "PING"
        Move to pos 14
Step 3: At pos 14, read "$4" → next string is 4 bytes
        Read 4 bytes from pos 18 → "TEST"
        Move to pos 24 (end)

Result: ["PING", "TEST"]
```

### Why So Complex? Why Not Just Split by Spaces?

Because RESP supports values with **spaces, special characters, or even binary data**:

```
SET msg "hello world"

Inline parse:   ["SET", "msg", "\"hello", "world\""]   ❌ broken
RESP parse:     ["SET", "msg", "hello world"]          ✅ correct
```

The `$N` length prefix tells the parser **exactly how many bytes to read**, so spaces or special characters inside don't break anything.

### Key Concepts

| Concept | Why It Matters |
|---|---|
| **Length-prefixed strings (`$N`)** | Can handle any content (spaces, binary, etc.) without escaping |
| **Position tracking (`pos`)** | Walk through bytes one chunk at a time |
| **CRLF (`\r\n`) delimiters** | Standard protocol marker, like newlines for HTTP |
| **Inline fallback** | Lets humans use telnet to type commands manually |

### Response Format Reference (Memorize This)

| Prefix | Type | Example |
|---|---|---|
| `+` | Simple string | `+OK\r\n` |
| `-` | Error | `-Error: bad command\r\n` |
| `:` | Integer | `:42\r\n` |
| `$` | Bulk string | `$5\r\nhello\r\n` (or `$-1\r\n` for nil) |
| `*` | Array | `*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n` |

### TL;DR

The parser walks through input **byte by byte**, reading markers (`*` and `$`) and length numbers to figure out how to slice the input into clean strings — without getting confused by spaces or weird characters in the data.

**Location:** RedisCommandHandler.cpp:17-57

---

### Q16.1: Edge Cases the Parser Handles

Each `break` statement is a safety check:

```cpp
if (pos >= input.size() || input[pos] != '$') break; // format error
if (crlf == std::string::npos) break;                // no \r\n found
if (pos + len > input.size()) break;                  // length lies — not enough data
```

**Why it matters:** A malicious client could send malformed data like:
```
*5\r\n$4\r\nPING\r\n      ← says 5 elements but only sends 1
```

Without these checks, `substr()` would read garbage memory → crash. The parser **silently stops** instead of crashing.

---

### Q16.2: Real Limitations of This Parser

Three things this parser does **NOT** handle (real Redis does):

| Limitation | Real Redis | Your Parser |
|---|---|---|
| **Pipelining** (multiple commands in one packet) | ✅ Supports | ❌ Only reads first command |
| **Partial data** (command split across multiple `recv()` calls) | ✅ Buffers and resumes | ❌ Assumes full command arrived |
| **Nested arrays** (`*` inside `*`) | ✅ Supports | ❌ Not implemented |

Example: if a client sends `*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n`, your parser only handles the first PING and ignores the second.

---

### Q16.3: ⚠️ Subtle Bug in the Parser

```cpp
int numElements = std::stoi(input.substr(pos, crlf - pos));
```

If a client sends `*abc\r\n...` (invalid number), `std::stoi` **throws an exception** that isn't caught anywhere. The server thread crashes for that client.

**Fix:** wrap in try/catch.

Common interview question: *"What happens if the client sends garbage instead of a number?"*

---

### Q16.4: How RESP Compares to Other Protocols

| Protocol | Style | Example |
|---|---|---|
| **HTTP** | Text headers + body | `GET /foo HTTP/1.1\r\n...` |
| **RESP** | Binary-safe text | `*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n` |
| **gRPC** | Binary (protobuf) | (binary bytes) |
| **JSON-RPC** | JSON | `{"method":"GET","params":["foo"]}` |

**Why Redis chose RESP:**
- **Fast to parse** (no JSON overhead)
- **Human-readable** (debug with telnet)
- **Binary-safe** (handles any value because of length prefixes)
- **Simple** (~5 data types, easy to implement)

---

### Q16.5: Final Practice Exercise

Trace this input through the parser mentally:

```
*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$11\r\nhello world\r\n
```

Expected output: `["SET", "foo", "hello world"]`

If you can walk through it byte-by-byte and explain each step, you've mastered the parser. ✅

---

### Q16.6: What Does `std::istringstream` + `while (iss >> token)` Do?

This is a common C++ idiom for **splitting a string by whitespace**.

```cpp
std::istringstream iss(input);
std::string token;
while (iss >> token) 
    tokens.push_back(token);
```

Takes `"PING TEST HELLO"` → splits into `["PING", "TEST", "HELLO"]`.

#### Line by Line

**`std::istringstream iss(input);`**
Creates an **input string stream** — wraps the string so you can read from it like a file (one piece at a time).

```
input  = "PING TEST HELLO"
                ↓
iss    = [PING TEST HELLO]   ← now treatable as a stream
```

**`while (iss >> token)`**
The `>>` operator on a stream reads one whitespace-separated chunk at a time. Returns `true` if a word was read, `false` if stream is empty.

#### Visual Trace

```
input = "PING TEST HELLO"

Iteration 1:
  iss >> token  →  token = "PING"
  push back     →  tokens = ["PING"]

Iteration 2:
  iss >> token  →  token = "TEST"
  push back     →  tokens = ["PING", "TEST"]

Iteration 3:
  iss >> token  →  token = "HELLO"
  push back     →  tokens = ["PING", "TEST", "HELLO"]

Iteration 4:
  iss >> token  →  empty, returns false → loop exits
```

#### The `>>` Operator Concept

You've already seen `>>` with `cin`:
```cpp
int x;
std::cin >> x;        // reads integer from keyboard
```

It works the same way with a string stream:
```cpp
std::istringstream iss("hello 42 3.14");
std::string word;
int num;
double pi;
iss >> word >> num >> pi;
// word = "hello", num = 42, pi = 3.14
```

#### Why Use This Instead of Manual Splitting?

**Manual (tedious):**
```cpp
size_t start = 0;
size_t end = input.find(' ');
while (end != std::string::npos) {
    tokens.push_back(input.substr(start, end - start));
    start = end + 1;
    end = input.find(' ', start);
}
tokens.push_back(input.substr(start));
```

**Stream way (clean):**
```cpp
std::istringstream iss(input);
std::string token;
while (iss >> token) tokens.push_back(token);
```

Shorter, handles multiple spaces automatically, skips empty tokens.

#### Why This Is Used in the Parser

This is the **fallback path** when input is NOT in RESP format (e.g., when someone uses telnet to type commands by hand):

- **RESP format:** `*2\r\n$4\r\nPING\r\n$4\r\nTEST\r\n` → main parser
- **Inline format:** `PING TEST` → falls back to this whitespace splitter

#### TL;DR

```cpp
std::istringstream iss(input);   // wrap string as a "stream"
std::string token;                // temp holder
while (iss >> token)              // read next word until empty
    tokens.push_back(token);      // store it
```

= "Split this string by whitespace and put each word into the vector."

---

## Q17: ⚠️ IMPORTANT — Command Dispatcher and RESP Response Patterns

**Question:**
> Walk through the dispatcher and the 3 representative response patterns used in command handlers.

**Answer:**

### Q17.1: The Dispatcher — `processCommand()`

```cpp
std::string RedisCommandHandler::processCommand(const std::string& commandLine) {
    auto tokens = parseRespCommand(commandLine);
    if (tokens.empty()) return "-Error: Empty command\r\n";

    std::string cmd = tokens[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
    RedisDatabase& db = RedisDatabase::getInstance();

    if (cmd == "PING")        return handlePing(tokens, db);
    else if (cmd == "ECHO")   return handleEcho(tokens, db);
    // ... 28 commands ...
    else return "-Error: Unknown command\r\n";
}
```

**Step by step:**

1. **Parse raw bytes** → tokens using the RESP parser
2. **Empty check** → return error if parse failed
3. **Extract + uppercase the command** using `std::transform`:
   ```cpp
   std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
   ```
   Applies `::toupper` to every character. Redis is case-insensitive, so `ping`, `PING`, `Ping` all normalize to `PING`.
4. **Get DB singleton** → `RedisDatabase::getInstance()`
5. **Route to handler** via if/else chain

### Q17.2: Why If/Else Instead of `switch`?

C++ `switch` only works on **integers/enums**, not strings. Three options:

| Approach | Pros | Cons |
|---|---|---|
| **if/else chain** (used here) | Simple | O(N) lookup, ugly with many commands |
| **`unordered_map<string, function>`** | O(1) lookup, scalable | Slightly more setup |
| **Hash command names** then `switch` | Fast | Tricky |

For 200+ commands, real Redis uses a command table (map approach). For 28 commands, if/else is fine.

---

### Q17.3: Handler Pattern 1 — Simple String Response (`+`)

```cpp
static std::string handlePing(const std::vector<std::string>& /*tokens*/, RedisDatabase& /*db*/) {
    return "+PONG\r\n";
}
```

**Format breakdown:**
```
+PONG\r\n
↑    ↑
|    end marker
+ = "this is a simple string"
```

**Use case:** Fixed short responses like `OK`, `PONG`. No length needed because `\r\n` marks the end.

**Note:** `/*tokens*/` syntax means "I know about this parameter but don't use it" — avoids unused-variable warnings.

---

### Q17.4: Handler Pattern 2 — Bulk String or Nil (`$`)

```cpp
static std::string handleGet(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2)
        return "-Error: GET requires key\r\n";
    std::string value;
    if (db.get(tokens[1], value))
        return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
    return "$-1\r\n";
}
```

**Step by step:**

1. **Validate args:** `GET` needs `["GET", "<key>"]` → 2 tokens minimum
2. **Call DB with out-parameter pattern:**
   ```cpp
   std::string value;
   if (db.get(tokens[1], value)) { ... }
   ```
   `db.get()` returns `true` if key exists (and fills `value`), `false` otherwise.
3. **Key found → bulk string:**
   ```
   $5\r\n        ← "next string is 5 bytes"
   hello\r\n     ← the actual data
   ```
4. **Key NOT found → nil:** `$-1\r\n` is the RESP convention for null/nil.

**Use case:** Any single value that might exist or not (GET, LPOP, HGET, etc.).

---

### Q17.5: Handler Pattern 3 — Array Response (`*`)

```cpp
static std::string handleKeys(const std::vector<std::string>& /*tokens*/, RedisDatabase& db) {
    auto allKeys = db.keys();
    std::ostringstream oss;
    oss << "*" << allKeys.size() << "\r\n";
    for (const auto& key : allKeys)
        oss << "$" << key.size() << "\r\n" << key << "\r\n";
    return oss.str();
}
```

**Step by step:**

1. **Get all keys** from DB
2. **`ostringstream`** — opposite of `istringstream`. Instead of reading from a string, you **build a string by writing** to it (like `cout`, but writes to memory).
3. **Array header:** `*<N>\r\n` → "array with N elements"
4. **Each element as bulk string:** `$<len>\r\n<value>\r\n`
5. **Convert to string:** `oss.str()`

**Example output for keys `["foo", "bar", "baz"]`:**
```
*3\r\n           ← 3 elements
$3\r\nfoo\r\n    ← first
$3\r\nbar\r\n    ← second
$3\r\nbaz\r\n    ← third
```

**Use case:** Any response with multiple values (KEYS, LGET, HGETALL, etc.).

---

### Q17.6: All RESP Response Patterns (Memorize This)

| Pattern | Prefix | When to Use | Example |
|---|---|---|---|
| **Simple string** | `+` | Fixed short responses | `+OK\r\n`, `+PONG\r\n` |
| **Bulk string** | `$len\r\nval\r\n` (or `$-1\r\n` for nil) | Single value that might exist | `$5\r\nhello\r\n` |
| **Array** | `*N\r\n...elements...` | Multiple values | `*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n` |
| **Integer** | `:N\r\n` | Numeric result | `:42\r\n` |
| **Error** | `-...\r\n` | Anything that fails | `-Error: bad command\r\n` |

---

### Q17.7: The Universal Handler Template

Every other handler follows this same 3-step pattern:

```cpp
static std::string handleXXX(...) {
    if (tokens.size() < N)                    // 1. Validate args
        return "-Error: ...\r\n";
    db.someMethod(tokens[1], tokens[2], ...); // 2. Call DB
    return ":1\r\n";                          // 3. Format response
}
```

Once you understand the dispatcher + the 3 response patterns above, you've seen everything this file teaches.

### Key Concepts Mastered

| # | Concept | Where |
|---|---|---|
| 1 | `std::transform` for case normalization | dispatcher |
| 2 | Why if/else instead of switch on strings | dispatcher |
| 3 | Simple string response (`+`) | handlePing |
| 4 | Bulk string + nil (`$`) with out-parameter pattern | handleGet |
| 5 | Array response (`*`) built with `ostringstream` | handleKeys |
| 6 | The universal validate→call→format template | all handlers |

**Location:** RedisCommandHandler.cpp:60-400

---

## Q18: How Does `purgeExpired()` Work (TTL Eviction)?

**Question:**
> Explain `purgeExpired()` — when is it called and how does it work?

**Answer:**

`purgeExpired()` deletes **expired keys** from the database using **lazy eviction** — only checks when a key is accessed.

### The Code

```cpp
void RedisDatabase::purgeExpired() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = expiry_map.begin(); it != expiry_map.end(); ) {
        if (now > it->second) {
            kv_store.erase(it->first);
            list_store.erase(it->first);
            hash_store.erase(it->first);
            it = expiry_map.erase(it);
        } else {
            ++it;
        }
    }
}
```

### What It Does

1. Get current time
2. Walk every entry in `expiry_map` (which maps `key → expiry_time`)
3. If past expiry → erase from all 3 stores
4. Move on

### When Is It Called?

Automatically inside 6 functions: `get`, `keys`, `type`, `del`, `expire`, `rename`. This is called **lazy eviction** — keys are only purged when accessed.

⚠️ **Bug:** NOT called by list/hash operations (`lpush`, `hset`, etc.). Expired keys leak there.

### The Tricky Iterator Pattern

```cpp
it = expiry_map.erase(it);   // returns next valid iterator
```

You can't `++it` after erasing — the iterator becomes invalid. So `erase()` returns the next valid one, and you assign it back. Notice the loop has **no `++it` in the header**.

### Lazy vs Active Eviction

| Strategy | How | Pros | Cons |
|---|---|---|---|
| **Lazy** (this code) | Check on access | Simple | Expired keys leak if never accessed |
| **Active** | Background thread scans | Memory cleaned | Extra CPU |
| **Hybrid** (real Redis) | Both | Best of both | Complex |

### Performance Concern

This is **O(N)** — scans all expiry entries on every access. For 1M keys with TTL, every GET/SET runs a 1M-iteration loop.

**Fix (planned in improvement plan):** Add background thread that samples ~20 random keys per second (real Redis approach). Quick 2-3 hour add-on.

### Interview Talking Points

- "How does TTL work?" → Lazy vs active eviction
- "Why `steady_clock` not `system_clock`?" → Monotonic, immune to clock changes
- "What if user changes system time?" → `steady_clock` doesn't care
- "How to scale to 1M keys?" → Bucketed timer wheel, random sampling

**Location:** RedisDatabase.cpp:91-104

---

## Q19: `steady_clock` vs `system_clock` — Why It Matters for TTLs

**Question:**
> Why does `purgeExpired()` use `steady_clock` instead of `system_clock`? What happens if the user changes the system time?

**Answer:**

### The Two Clocks

| Clock | Measures | Affected by Time Changes? |
|---|---|---|
| **`system_clock`** | Wall clock time (real-world date/time) | ✅ Yes — NTP, DST, manual changes |
| **`steady_clock`** | Monotonic ticks (always forward) | ❌ No — pure counter |

Your code correctly uses:
```cpp
auto now = std::chrono::steady_clock::now();
```

### What Breaks with `system_clock`?

#### Scenario 1: User jumps clock FORWARD
```
14:00:00  SET session1 abc EXPIRE 3600  → expires at 15:00:00
14:30:00  Admin: sudo date -s "20:00:00"
14:30:01  GET session1 → now=20:00 > expiry=15:00 → DELETED early ❌
```

#### Scenario 2: User jumps clock BACKWARD
```
14:00:00  SET session1 abc EXPIRE 60   → expires at 14:01:00
14:00:30  Admin: sudo date -s "10:00:00"
          → key never expires until system clock catches back up to 14:01 ❌
          → could "live forever" ❌
```

### Why `steady_clock` Is Safe

`steady_clock` is just **ticks since boot** — pure number that only goes up. Doesn't care about NTP, DST, time zones, or admin changes. A 60-second TTL is always exactly 60 real seconds.

### Real-World Time Change Causes

- **NTP synchronization** (constantly, small adjustments)
- **Daylight Saving Time** (twice a year)
- **VM migration / leap seconds / hardware drift**
- **Manual admin changes**

### When to Use Each

| Use Case | Right Clock |
|---|---|
| TTL / expiration / timeouts | `steady_clock` ✅ |
| Benchmarking elapsed time | `steady_clock` ✅ |
| Logging "created at" timestamps | `system_clock` ✅ |
| Displaying time to user | `system_clock` ✅ |

### Interview Answer

> *"`system_clock` represents wall clock time, which can jump forward (NTP, DST) or backward (manual changes), causing TTLs to expire too early or never expire. `steady_clock` is monotonic — only moves forward at constant rate, so a 60-second TTL is always 60 real seconds. Use `system_clock` only when you need human-readable dates."*

**Location:** RedisDatabase.cpp:92, RedisDatabase.h:65

---

## Q20: What is `std::lock_guard<std::mutex>`?

**Question:**
> Explain `std::lock_guard<std::mutex> lock(db_mutex)` — what is it and how does it work?

**Answer:**

`lock_guard` is a **smart wrapper** that locks a mutex on creation and **automatically unlocks** it when it goes out of scope.

### The Code

```cpp
std::lock_guard<std::mutex> lock(db_mutex);  // locks immediately
// ... do stuff ...
}  // ← `lock` destroyed here, mutex AUTO-UNLOCKED
```

### Without lock_guard (Unsafe)

```cpp
db_mutex.lock();
kv_store[key] = value;   // if this throws...
db_mutex.unlock();        // this NEVER runs → DEADLOCK ❌
```

### With lock_guard (Safe)

```cpp
std::lock_guard<std::mutex> lock(db_mutex);
kv_store[key] = value;   // even if this throws...
}  // destructor runs, mutex unlocked ✅
```

### Why This Works — Stack Unwinding

When an exception is thrown:
1. Execution jumps out of the function (next line does NOT run)
2. C++ runtime **destroys all local objects in reverse order**
3. Each destructor runs (this unlocks the mutex)
4. Exception continues bubbling up

This guarantee is called **RAII** — Resource Acquisition Is Initialization.

### Exception Behavior Clarification (Common Confusion)

```cpp
db_mutex.lock();         // ✅ runs
throw std::exception();  // ⚠️ jumps out
db_mutex.unlock();       // ❌ SKIPPED — exception doesn't continue line by line
```

Once `throw` happens, execution **abandons the function** immediately. It does NOT continue to the next line.

### Comparison

| Approach | Auto-unlock? | Exception-safe? |
|---|---|---|
| Manual `lock()/unlock()` | ❌ No | ❌ No |
| `std::lock_guard` | ✅ Yes | ✅ Yes |
| `std::unique_lock` | ✅ Yes | ✅ Yes (more flexible) |

For 95% of cases, `lock_guard` is correct. Use `unique_lock` only when you need to unlock early or transfer ownership.

### Restaurant Analogy

```
Manual lock:    take key, must remember to return it (forget → deadlock)
lock_guard:     key on a wristband that auto-returns when you leave ✅
```

### TL;DR

```cpp
std::lock_guard<std::mutex> lock(db_mutex);
```

= **"Lock this mutex. Auto-unlock when scope exits — even on exceptions."**

The standard, safe way to use mutexes in modern C++.

**Location:** RedisDatabase.cpp — every function (used 25+ times)

---

## Q21: How Does `lrem()` Work? (Most Complex Function)

**Question:**
> Explain `lrem()` — what it does and how the 3 modes work.

**Answer:**

`lrem()` removes occurrences of a value from a list with **3 modes** based on the count parameter:

| Count | Mode |
|---|---|
| `count = 0` | Remove **ALL** occurrences |
| `count > 0` | Remove first N occurrences (head → tail) |
| `count < 0` | Remove last N occurrences (tail → head) |

### Example

```
List:  [a, b, a, c, a, d, a]

LREM key  0  a  →  [b, c, d]            (all 4 'a's removed)
LREM key  2  a  →  [b, c, a, d, a]      (first 2 'a's removed)
LREM key -2  a  →  [a, b, a, c, d]      (last 2 'a's removed)
```

---

### Mode 1: `count == 0` — Erase-Remove Idiom

```cpp
auto new_end = std::remove(lst.begin(), lst.end(), value);
removed = std::distance(new_end, lst.end());
lst.erase(new_end, lst.end());
```

This is the **classic C++ erase-remove idiom**. Key insight: **`std::remove` does NOT actually remove**! It shuffles unwanted elements to the end and returns an iterator to the "new end".

```
Before:  [a, b, a, c, a, d, a]
                                ↑ lst.end()

After std::remove:
         [b, c, d, ?, ?, ?, ?]
                   ↑ new_end (returned)
                                ↑ lst.end() unchanged

After lst.erase(new_end, end):
         [b, c, d]   ✅
```

`std::remove` is a generic algorithm — it works on arrays too where you can't shrink size. The `erase` step physically removes trailing junk.

---

### Mode 2: `count > 0` — Forward Iteration

```cpp
for (auto iter = lst.begin(); iter != lst.end() && removed < count; ) {
    if (*iter == value) {
        iter = lst.erase(iter);   // erase returns next valid iter
        ++removed;
    } else {
        ++iter;
    }
}
```

Standard forward iteration using the **erase-returns-next-iterator pattern** (same as `purgeExpired`).

**Why no `++iter` in header?** Because we either erase (which advances) or manually skip. Putting `++iter` in the header would skip elements after an erase.

---

### Mode 3: `count < 0` — Reverse Iteration (The Tricky One)

```cpp
for (auto riter = lst.rbegin(); riter != lst.rend() && removed < (-count); ) {
    if (*riter == value) {
        auto fwdIter = riter.base();
        --fwdIter;
        fwdIter = lst.erase(fwdIter);
        ++removed;
        riter = std::reverse_iterator<std::vector<std::string>::iterator>(fwdIter);
    } else {
        ++riter;
    }
}
```

#### The Problem

`vector::erase` only accepts **forward iterators**. But we're walking **backwards** with reverse iterators. So we need to dance between the two.

#### The Reverse Iterator Off-By-One

`reverse_iterator.base()` returns a forward iterator pointing **one past** the actual element:

```
Reverse iterator   →   .base() returns
─────────────────       ──────────────
points to 'e'       →   points ONE PAST 'e' (past-the-end)
points to 'd'       →   points to 'e'
```

That's why the code does `--fwdIter` after `.base()` — to step back to the real element.

#### The Full Dance Per Iteration

```cpp
auto fwdIter = riter.base();   // reverse → forward (off by 1)
--fwdIter;                      // fix off-by-one
fwdIter = lst.erase(fwdIter);   // erase (returns forward iter)
riter = std::reverse_iterator<...>(fwdIter);  // forward → reverse, resume backward walk
```

**Hallway analogy:**
- Walking east-to-west (reverse iteration)
- Door-removal tool only works while facing west-to-east
- Turn around (`.base()`) → remove door → turn back (`reverse_iterator(fwdIter)`) → continue walking west

---

### Performance Analysis

| Mode | Complexity | Notes |
|---|---|---|
| Mode 1 (all) | O(N) | Single pass with `std::remove` |
| Mode 2 (head) | O(N×count) | Each vector erase shifts later elements |
| Mode 3 (tail) | O(N×count) | Same shift problem |

**Vector** is bad for middle deletions (O(N) per erase). **Real Redis** uses a custom **quicklist** (linked list of vectors) to balance random access + cheap deletes.

---

### Key Concepts

| Concept | Why It Matters |
|---|---|
| **Erase-remove idiom** | Standard C++ pattern for bulk removal |
| **`std::remove` doesn't remove** | Just shuffles; needs `erase` to actually delete |
| **`erase()` returns next iterator** | Avoids iterator invalidation |
| **`reverse_iterator.base() - 1`** | Convert reverse → forward iterator |
| **`std::make_reverse_iterator`** (C++14) | Cleaner conversion than `std::reverse_iterator<T>(...)` |

### Interview Questions

| Question | What They Test |
|---|---|
| Explain the erase-remove idiom | Standard library knowledge |
| Why does `std::remove` not actually remove? | Generic algorithm understanding |
| How does `reverse_iterator.base()` work? | Iterator semantics (advanced) |
| Time complexity? | Algorithmic thinking |
| Why vector instead of list? | Data structure trade-offs |

### TL;DR

3 modes, 3 different techniques:
- **Mode 0**: erase-remove idiom (one-shot bulk removal)
- **Mode +N**: forward iteration with `it = erase(it)` pattern
- **Mode -N**: reverse iteration with `.base() - 1` conversion dance

The reverse-iterator dance is the **trickiest part of this entire codebase**. Everything else (`hset`, `lpop`, etc.) is straightforward by comparison.

**Location:** RedisDatabase.cpp:191-230

---

## Q22: `lindex()` / `lset()` — Negative Indexing & Why It Matters

**Question:**
> How do `lindex` and `lset` work, and why is negative indexing necessary when positive indices could do the job?

**Answer:**

### What These Functions Do

| Function | Purpose |
|---|---|
| `lindex(key, index)` | Read element at position |
| `lset(key, index, value)` | Update element at position |

Both support **negative indices** (Python-style — count from the end).

### The Code Pattern

```cpp
if (index < 0)
    index = lst.size() + index;        // convert negative to positive
if (index < 0 || index >= static_cast<int>(lst.size()))
    return false;                       // bounds check both ends
```

**Two-stage validation:**
1. Negative → positive via `size + index` math
2. Re-check both ends (negative could still be out of range, e.g., `-size-1`)

**`static_cast<int>(lst.size())`** silences GCC's signed/unsigned comparison warning.

### Negative Index Math

```
List:    [a, b, c, d, e]
Index:    0  1  2  3  4   ← positive
Index:   -5 -4 -3 -2 -1   ← negative

index = -1   →   5 + (-1) = 4  → 'e' ✅
index = -6   →   5 + (-6) = -1 → out of bounds ❌
```

---

### Why Negative Indexing Is Critical

You're right that positive indices could technically do the job. But negative indexing exists for **5 strong reasons**:

#### 1. Atomicity (Race Condition Protection)

**Without negative:**
```
Client A: LLEN mylist → 5
                       ← meanwhile Client B: LPUSH mylist "new" (size=6 now)
Client A: LSET mylist 4 "updated"
          ↑ updates WRONG element ❌
```

**With negative:**
```
Client A: LSET mylist -1 "updated"  ← always updates true last element ✅
```

Server resolves `-1` at execution time after acquiring the lock. No race possible.

#### 2. Single Command vs Two

```
WITHOUT -1: LLEN + LINDEX (2 round trips, 2 commands)
WITH -1:    LINDEX -1     (1 round trip, atomic)
```

#### 3. Cleaner Client Code

```cpp
// Without:
int size = redis.llen("mylist");
int realIndex = (userInput < 0) ? size + userInput : userInput;
std::string val = redis.lindex("mylist", realIndex);

// With:
std::string val = redis.lindex("mylist", userInput);
```

#### 4. Natural Mental Model

| Thought | Index |
|---|---|
| "First item" | `0` |
| "Last item" | `-1` |
| "Second to last" | `-2` |

Matches how humans describe positions.

#### 5. Industry Convention

Python, JavaScript (`.at`), Ruby, Redis — they all do it. Programmers expect it.

---

### Code Smell: Duplication

`lindex` and `lset` are 90% identical. A cleaner design would extract:

```cpp
bool resolveIndex(int& index, size_t size) {
    if (index < 0) index = size + index;
    return (index >= 0 && index < static_cast<int>(size));
}
```

Worth mentioning as a refactor in interviews.

### Key Concepts

| Concept | Why It Matters |
|---|---|
| **Negative indexing** | Atomic "from-the-end" access |
| **`size + (-index)` math** | Standard conversion pattern |
| **Two-stage bounds check** | Validates both negative-out and positive-out cases |
| **`static_cast<int>` for `size_t`** | Signed/unsigned comparison workaround |
| **Out-parameter (bool + ref)** | Same as `get()` — distinguishes "not found" from value |

### TL;DR

Negative indexing isn't a convenience — it's a **correctness + performance feature**. It eliminates race conditions, saves round-trips, and matches universal programmer expectations.

**Location:** RedisDatabase.cpp:232-262

---

## Q23: How Do `dump()` / `load()` Work? (Persistence — High Interview Value)

**Question:**
> Explain `dump()` and `load()` — the persistence format, the known bugs, and how to fix them.

**Answer:**

`dump()` writes the in-memory database to a file; `load()` reads it back. They use a custom **text-based format**.

### The Format

Each line is one record, first character is a **type tag**:

| Tag | Type | Format |
|---|---|---|
| `K` | Key-Value | `K <key> <value>` |
| `L` | List | `L <key> <item1> <item2> ...` |
| `H` | Hash | `H <key> <field1>:<value1> <field2>:<value2> ...` |

**Example `dump.my_rdb`:**
```
K username alice
L fruits apple banana orange
H user:1 name:bob age:30
```

---

### `dump()` — Step by Step

```cpp
bool RedisDatabase::dump(const std::string& filename) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) return false;

    for (const auto& kv: kv_store)
        ofs << "K " << kv.first << " " << kv.second << "\n";

    for (const auto& kv : list_store) {
        ofs << "L " << kv.first;
        for (const auto& item : kv.second) ofs << " " << item;
        ofs << "\n";
    }

    for (const auto& kv : hash_store) {
        ofs << "H " << kv.first;
        for (const auto& fv : kv.second) ofs << " " << fv.first << ":" << fv.second;
        ofs << "\n";
    }
    return true;
}
```

1. Lock the DB (prevents writes during dump)
2. Open file in **binary mode** (avoids `\n` → `\r\n` translation on Windows)
3. Iterate each store, write one line per entry

---

### `load()` — Step by Step

```cpp
bool RedisDatabase::load(const std::string& filename) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs) return false;

    kv_store.clear(); list_store.clear(); hash_store.clear();

    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        char type; iss >> type;
        if (type == 'K') {
            std::string key, value;
            iss >> key >> value;
            kv_store[key] = value;
        } else if (type == 'L') {
            // read key, then items one by one
        } else if (type == 'H') {
            // read key, then field:value pairs, split on ':'
        }
    }
    return true;
}
```

1. Lock + clear existing data (fresh restore)
2. Read line by line via `std::getline`
3. Wrap each line in `istringstream` to tokenize
4. First char = type tag → dispatch to correct parsing logic

---

### ⚠️ Critical Bug: Values With Spaces

The `>>` operator **stops at whitespace**, breaking any value containing a space.

```cpp
SET msg "hello world"
```

**Dumped:** `K msg hello world`

**Loaded:**
```cpp
iss >> key;    // key   = "msg"
iss >> value;  // value = "hello"   ← "world" LOST ❌
```

### Same Bug in Lists & Hashes

```
LPUSH list "hello world"
↓ dump: L list hello world
↓ load: list_store["list"] = ["hello", "world"]   ← 2 items instead of 1 ❌
```

```
HSET user 1 "John Smith"
↓ dump: H user 1:John Smith
↓ load: hash["1"] = "John"   ← "Smith" silently dropped ❌
```

**Hash also breaks on colons:**
```
HSET user url "http://x.com"
↓ load: field=url, value=http   ← wrong colon used ❌
```

---

### How to Fix (Length-Prefixed Encoding)

Real Redis uses length prefixes (same idea as RESP):

```
K 3 11 msg hello world
↑ ↑ ↑
| | length of value
| length of key
type
```

This makes the format **binary-safe** — any byte sequence in keys/values works correctly.

---

### Why `std::ios::binary`?

```cpp
std::ofstream ofs(filename, std::ios::binary);
```

- **Without binary mode (Windows):** `\n` → `\r\n` on disk; reading back `\r\n` → `\n` → file corruption when moved cross-platform
- **With binary mode:** bytes written exactly as-is → portable

---

### Other Persistence Issues

| Issue | Impact |
|---|---|
| **TTLs not persisted** | All expirations lost on restart |
| **Dump locks entire DB** | Server pauses during snapshot |
| **No checksum** | Can't detect file corruption |
| **No transaction log** | Crashes between dumps lose all changes |
| **No compression** | Big DB = big file |

### How Real Redis Solves It

- **RDB format:** Binary, length-prefixed, checksummed
- **AOF (Append-Only File):** Every write logged → durability
- **`fork()` + Copy-on-Write:** Background snapshot without blocking

---

### Key Concepts

| Concept | Why It Matters |
|---|---|
| **`ofstream` / `ifstream`** | Standard C++ file I/O |
| **`std::ios::binary`** | Cross-platform byte consistency |
| **`std::getline`** | Read file line by line |
| **`istringstream`** | Tokenize a single line |
| **`>>` whitespace splitting** | Source of all persistence bugs |
| **Length prefixing** | The fix — binary-safe encoding |

### Interview Questions

| Question | What They're Testing |
|---|---|
| "How would you serialize a key-value store?" | Format design awareness |
| "What's wrong with space-delimited values?" | Bug recognition |
| "How would you make it binary-safe?" | Length-prefix understanding |
| "Why does Redis use a binary format?" | Performance + correctness reasoning |
| "What if the server crashes between dumps?" | Durability / AOF knowledge |
| "How to reduce dump latency?" | `fork()` + COW (real Redis approach) |

### TL;DR

```
dump() = iterate all 3 stores → write each entry as text line
load() = read each line → first char = type → parse & restore

Bug: Values with spaces silently corrupt (>> stops at whitespace) ❌
Fix: Length-prefixed encoding (like RESP)
```

| Aspect | Detail |
|---|---|
| **Strength** | Human-readable, easy to debug |
| **Weakness** | Not binary-safe → silent data loss |
| **Production fix** | Length prefixes OR escaping OR binary format |
| **Real Redis** | RDB (binary snapshot) + AOF (write log) |

**This is the most interview-rich function pair** in the codebase — it has a real bug with a known fix and opens discussions about file I/O, serialization, durability, and crash recovery.

**Location:** RedisDatabase.cpp:353-441

---

# Phase 3 — epoll: Questions & Answers

> Beginner-friendly Q&A captured while planning the `epoll` event-loop rewrite (Phase 3).
> Companion to [`PHASE3_EPOLL_PLAN.md`](PHASE3_EPOLL_PLAN.md). Built up from analogies, no jargon dumps.

---

## Q24: What is `epoll`? Why use it, and how will we implement it?

**Question:**
> Explain epoll simply — what is it, why are we using it, and how will we implement it in our project (architecture level)?

**Answer:**

### What we have now (the problem)

Today the server uses **one thread per client**. A thread is like a **worker employee**:

- Client connects → the server hires **one dedicated worker** for that client.
- That worker **waits** for the client to send a command. While waiting, it does nothing — it's *blocked*.
- 1,000 clients → 1,000 workers, most just standing around.

**Restaurant analogy:** one waiter per table. A table reading the menu still has a waiter frozen next to it, unable to help anyone else. 1,000 tables = 1,000 waiters — expensive (~1 MB memory each) and chaotic (constant switching). This is the classic **C10K problem** — can you handle 10,000 clients at once? Not this way.

### What epoll is

> **epoll is a Linux feature that lets ONE worker watch THOUSANDS of clients at once, and act only on the ones that need attention right now.**

Instead of one waiter per table, you have **one smart waiter with a magic notepad** that says exactly which tables just raised their hand. The waiter ignores everyone reading the menu and only serves tables ready to order. That notepad is epoll:

- You tell epoll: *"watch these 1,000 connections."*
- Your one thread calls **`epoll_wait()`**, which **sleeps** until something happens.
- epoll wakes the thread and hands it a **list of just the clients that are ready**.
- Handle those, loop back, wait again.

So instead of 1,000 blocked threads, you have **1 thread** that's either working or efficiently sleeping. Redis, nginx, and Node.js all work this way.

### Why we're switching

| | Now (thread-per-client) | With epoll |
|---|---|---|
| Workers | 1 per client | **1 total** |
| Memory per client | ~1 MB | a few KB |
| Realistic limit | ~1,000 clients | 10,000–100,000 |

### How we'll implement it (architecture)

Everything epoll watches is a **file descriptor**. The server becomes one loop:

```
   ┌──────────── the event loop (one thread) ───────────┐
   │   epoll_wait()  ←— sleeps until something is ready  │
   │        ├─ listening socket ready? → NEW client      │
   │        ├─ a client sent data?     → read, run, reply│
   │        └─ Ctrl+C signal?          → shut down       │
   │   ...loop back to epoll_wait()                      │
   └─────────────────────────────────────────────────────┘
```

1. **The accept loop** (`RedisServer::run`) becomes the event loop — no thread per client; we register each new client with epoll.
2. **Each client gets a small "state folder"** instead of a thread — two buffers (bytes in, bytes out). Needed because a client may send half a command now, the rest later → we **accumulate** bytes until a full command arrives (**framing**).
3. **The command brain is untouched** — `processCommand()` and the database stay the same. We only change *how bytes reach* the brain, which is why all 72 tests keep passing.

**The one genuinely new concept — framing:** with one shared worker, a client might dribble in `SET na`… then `me bob`. The loop must collect the pieces and recognise a complete command before running it. That's `respFrameLength()` — built and unit-tested first, with no networking.

---

## Q25: How do `epoll`, sockets, and file descriptors work together?

**Question:**
> What is a file descriptor, what is a socket, and how do these three (epoll, socket, fd) work together?

**Answer:**

### File descriptor (fd) — "a ticket number"

In Linux, *"everything is a file."* When your program **opens** anything (file, network connection, signal), the OS keeps the real thing and hands you back a **small integer** — the file descriptor.

**Analogy:** a coat-check. You hand over your coat, they keep it, you get **ticket #42**. You refer to the coat by its number. `open a file → fd 3`, `open a connection → fd 5`.

### Socket — "a phone line"

A **socket** is one kind of thing you can open: an **endpoint for network communication**. A client connection *is* a socket. You talk to a socket **through its fd**:
- `recv(fd, ...)` = "listen on phone line #5"
- `send(fd, ...)` = "talk into phone line #5"

Two flavors: **one listening socket** (the front-desk phone that rings on new connections) and **many client sockets** (the actual conversations).

### epoll — "the receptionist watching all the lines"

Your one thread can't pick up 1,000 phone lines at once. **epoll is a notifier:** give it a list of fds, and `epoll_wait()` sleeps until one needs attention, then says *"lines 6, 19, 340 are ready."* It's a **switchboard receptionist** watching 1,000 lights, telling the worker only which lit up. epoll **itself is also an fd** (from `epoll_create1()`).

### The three together — concrete walkthrough

```
fd 3 = listening socket   | fd 4 = epoll instance | fd 5,6,7 = clients

1. create listen socket → fd 3
2. create epoll → fd 4
3. tell epoll: "watch fd 3"        ← epoll_ctl(ADD)
4. epoll_wait(fd 4) → sleeps
5. client connects, fd 3 lights up → epoll: "fd 3 ready"
6. accept() → new socket fd 5; tell epoll "also watch fd 5"
7. epoll_wait → sleeps
8. client sends "PING", fd 5 lights up → epoll: "fd 5 ready to read"
9. recv(fd 5) → "PING" → processCommand → "PONG" → send(fd 5)
10. loop back to epoll_wait
```

**The key subtlety:** epoll tells you a socket is **ready** — it does **not** read/write for you. The receptionist says "line #5 lit up"; *you* still pick up the line (`recv`). epoll is a **notifier of readiness**, not a delivery service.

---

## Q26: On what occasions does epoll notify the thread?

**Question:**
> When exactly does epoll wake the thread? Does it notify when a client sends a request?

**Answer:**

Think of each as a light on the switchboard. Two main lights + a couple of "something broke" lights.

### 🟢 "Ready to READ" (`EPOLLIN`) — the common one

| Which socket | What it means |
|---|---|
| Listening socket | A **new client is connecting** → `accept()` it |
| Client socket | The client **sent data** (a request) → `recv()` it |
| Client socket | The client **disconnected** → `recv()` returns 0 |

So **yes** — a client sending a request fires this "ready to read" event.

### 🟡 "Ready to WRITE" (`EPOLLOUT`)

Fires when a socket has **room to accept outgoing data**. You only care about it occasionally — when a previous `send()` couldn't push the whole reply (see Q27).

### 🔴 "Something broke" (`EPOLLERR`, `EPOLLHUP`)

Connection errored or hung up → clean up and close the fd.

### In plain English, epoll wakes the thread when:

1. 📞 A new client wants to connect
2. 💬 A client sent a command ← *the request case*
3. 👋 A client disconnected
4. ✍️ A clogged socket can finally receive the rest of a reply
5. ⚠️ A connection broke

Between these, the thread **sleeps** — never woken for idle clients. You tell epoll *per socket* which lights you care about (usually "ready-to-read"; add "ready-to-write" only when a reply gets stuck).

---

## Q27: What does "ready to write" mean?

**Question:**
> I don't understand "ready to write." Explain it.

**Answer:**

### `send()` doesn't go straight to the client

`send(fd, reply)` drops bytes into an **OS outbox** for that socket; the OS delivers them across the network at its own pace:

```
your code --send()--> [ OS outbox for fd 5 ] --network--> client
```

`send()` returns as soon as the bytes are *in the outbox*, not when the client receives them.

### The outbox has a limited size

Usually there's room — a small `+PONG\r\n` fits instantly, and you never think about "ready to write." But the outbox can **fill up** if (1) the reply is huge, or (2) the client is slow to receive. Then `send()` accepts only **part** of your data — e.g. *"took 2,000 of your 10,000 bytes."*

### Why we can't just retry

Looping on `send()` would **freeze the one thread**, spinning on a full outbox while 999 other clients wait. In a single-threaded server you must **never wait on one client**.

### So "ready to write" is:

1. **Save the leftover bytes** in the client's output buffer.
2. Tell epoll: *"notify me when fd 5's outbox has room again"* ← register for **ready-to-write**.
3. **Move on** and serve other clients.
4. When the outbox drains, epoll fires **`EPOLLOUT`** = *"room now"* → send the next chunk. Repeat until done, then tell epoll to **stop** notifying writes.

**Mail-outbox analogy:** you drop letters in the box (`send`); the carrier (OS/network) empties it. Usually there's space — drop and walk away. But with a huge stack and a full box, you keep the rest **in your hand** and raise a flag: *"tell me when the box has space."* That flag is "ready to write."

> **In one sentence:** "ready to write" means the OS's outbox for this client had filled up and has now drained enough to accept more bytes.

This is exactly why each client needs an **output buffer** (`outbuf` in `ClientState`) — the "leftover letters in your hand."

---

## Q28: Internally, how does epoll know a socket needs attention?

**Question:**
> What happens internally so epoll gets notified that *this* socket needs attention?

**Answer:**

### Step 1 — data arrives (hardware interrupt)

Bytes hit your **network card**. Instead of the CPU constantly asking "anything yet?", the card **taps the CPU on the shoulder** — a **hardware interrupt**. **Analogy:** a doorbell — it interrupts you when someone arrives; you don't keep checking the door.

### Step 2 — the kernel sorts the mail

The interrupt enters the **kernel**, which figures out **which socket** the packet belongs to and drops the bytes into **that socket's inbox** (receive buffer).

### Step 3 — the clever part: callback registration

When you did `epoll_ctl(ADD)` for fd 5, the kernel added epoll to **fd 5's wait queue** ("when I get data, here's who to tell"). So the moment data lands in fd 5's inbox, the kernel **fires epoll's callback**, which says: *"add fd 5 to epoll's ready list."*

### Step 4 — the ready list

epoll keeps one **ready list**. `epoll_wait()` is dead simple:
- ready list non-empty? → return those fds immediately.
- empty? → sleep the thread; wake it when a callback adds something.

So the socket **reports itself** the instant it gets data — epoll never goes looking.

### Why this matters — push vs pull

- **Old way (`select`/`poll`, pull):** the worker checks **all** 1,000 lines every time, even if only one has data → **O(n)**, slows down as clients grow.
- **epoll (push):** each line **raises its own hand** via the callback → the worker reads only the short list of raised hands → **O(active fds)**, ignores idle ones.

**Analogy:** old way = teacher reading the whole roll to find a raised question; epoll = students raise hands, teacher looks only at raised hands. 30 or 30,000 students — same cost per question.

```
client sends "PING"
  → network card (interrupt) → CPU
  → kernel: packet is for fd 5 → drop in fd 5 inbox
  → kernel checks fd 5 wait queue → fires epoll callback
  → callback: put fd 5 on epoll ready list
  → epoll_wait() wakes → "fd 5 ready"
  → recv(fd 5) → process → reply
```

---

## Q29: It's single-threaded and sequential — isn't that slow at scale?

**Question:**
> Single-threaded means it handles requests one after another. With lakhs of users hitting Redis, isn't sequential slow vs parallel?

**Answer:**

### Yes — it IS sequential. Here's why it's still fast.

### The work itself is absurdly fast

Redis stores data **in RAM** in a hash map. A `GET` is a memory lookup — about **100 nanoseconds**. One thread can do **~10 million** such ops/second. So **computation was never the bottleneck** — the slow part is **waiting** for the network, and waiting isn't "work."

### CPU-bound vs I/O-bound

- **CPU-bound** (image resize, math): heavy work → more threads/cores genuinely help.
- **I/O-bound** (Redis): trivial work, all time spent **waiting** for data → threads just sit **blocked**. 1,000 threads asleep on `recv()` aren't doing 1,000× work — they're doing nothing 1,000 times.

### epoll removes the waiting

The epoll thread is only ever handed clients that are **ready now**, so it runs nonstop: *ready client → 100 ns op → next ready client → 100 ns op → …* It interleaves the **waiting** of thousands of clients while never pausing on any one.

**Cashier analogy:** ringing up takes 1s; the slow part is customers fumbling for a wallet. Thread-per-client = one cashier per customer, 999 standing frozen. epoll = **one** cashier who only serves people who are **ready** — clears a huge line, because no single transaction was ever the holdup.

### Sequential is actually *faster* here

- **No locks** — one thread touches the data → no mutexes, no contention, no races (locking overhead can cost more than the op itself).
- **No context-switching** between thousands of threads.
- **Cache-friendly + predictable.**

Real Redis chose single-threaded **on purpose** for in-memory work.

### The honest downside

If a **single command** is slow (`KEYS *` on 10M keys, a giant sort), it **stalls everyone** behind it. Real Redis warns against slow commands. (Redis 6+ multi-threads only the network read/write, keeping **command execution single-threaded** to stay lock-free.)

### "But millions of users!"

1. **One thread is already plenty** — a single instance does **100K–1M ops/sec**; a lakh of tiny lookups is easy.
2. **For massive scale, run many instances** and split the data (**sharding/clustering**) — scale **horizontally**, not by adding threads to one instance.

> **Punchline:** for in-memory work the operation is so fast that "parallel" buys nothing — the win is **never sitting idle**, which is exactly what epoll delivers. One thread that never waits beats a thousand that mostly sleep.

---

## Q30: What is `signalfd` (and what is a file descriptor)?

**Question:**
> What is `signalfd`, what is an fd, and how does it work? (Phase 3 Step 6 uses it for shutdown.)

**Answer:**

### File descriptor (fd) recap
An **fd is a numbered ticket** the OS hands you to refer to something it manages — a file, a socket, a timer, even a signal. *Almost everything in Linux can be an fd*, which is what lets `epoll` watch them all the same way.

### What a signal is
A **signal is the OS tapping your program on the shoulder**: SIGINT = "user pressed Ctrl+C", SIGTERM = "please terminate", etc. It's a tiny urgent notification, not data.

### The old, awkward way — signal handlers
Traditionally you register a handler; when the signal arrives the OS **freezes your program wherever it is, runs the handler, then resumes.** This is asynchronous and disruptive — the handler can fire mid-operation, so it may only do tiny "async-signal-safe" things (like setting a flag). Our old code did exactly that: handler set `g_shutdown`, the loop polled it and juggled `EINTR`.

### What signalfd does
> **`signalfd` turns a signal into a readable file descriptor** — instead of interrupting you to run a handler, the signal makes an fd *readable*, which you check at a calm point in your loop.

The urgent shoulder-tap becomes a polite note in your inbox.

### How it works — 3 steps
1. **Block the signal** so the default action / handler never fires (it becomes *pending*):
   `pthread_sigmask(SIG_BLOCK, &mask, ...)` — done in the constructor.
2. **Create a signalfd** that collects it: `signal_fd = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);`
3. **Read it when ready** — when SIGINT arrives, `signal_fd` becomes readable; `read()` it to learn which signal it was, then break the loop → `shutdown()`.

### Why it's perfect for the epoll loop
SIGINT becomes **just one more light on the switchboard**. `epoll_wait()` wakes → "`signal_fd` is readable" → that means SIGINT → break → shut down. No async handler, no global flag, no `EINTR`. One mechanism handles sockets *and* signals.

> **Mental model:** a normal signal **interrupts** you and runs a handler at a random moment; `signalfd` **demotes** it into a readable fd you handle calmly in your loop. (Its cousins `timerfd` and `eventfd` do the same "turn X into an fd" trick.)

---

## Q31: Why does the OS hand you an fd (a number) instead of keeping it to itself?

**Question:**
> When I open a file the OS gives me back a number (fd). Why give me anything — why not keep it internal? And why a number rather than the real thing?

**Answer:**

### Why give you anything at all
Opening a file isn't one-shot — what you do is a **sequence** across separate calls: open → read → read again → write → close. Every later call must say *"operate on the thing I opened."* So the OS **must** give you a handle to name it; otherwise you'd have no way to ask for the next operation. It can't "keep it to itself" because *you* drive the follow-up calls.

### Why a *number*, not the real object
The real thing — kernel buffers, disk locations, permission records — lives in **kernel memory you're not allowed to touch.** If the OS handed you a raw pointer to its internals you could corrupt the kernel, read other processes' data, or crash the system. So it gives you a **meaningless little number** you can't misuse: the only thing you can do with fd 5 is ask the OS *"read from #5,"* and the OS **validates** it first. It's a safe, indirect reference — a leash, not the animal.

### How it works under the hood
The kernel keeps a **per-process table**: fd number → real object. The fd is literally the **row index** into that table.
```
3 → [open file X: position, permissions, ...]
5 → [socket to client Y: buffers, ...]
```
You hold the index; the kernel holds (and guards) the real object.

### It's also a permission token
`open()` checks "are you allowed?" **once**, up front. The fd you get back is **proof you passed** — future reads just present the fd and the OS trusts it.

### Analogies
- **Coat-check ticket:** they store your coat in a back room you can't enter and give you ticket #42; you present #42, they fetch it. You never roam the coatroom.
- **Bank account number:** the bank holds your money in a vault you can't access; you reference it by number and they act on your behalf after checking it's you.

> **One line:** the number exists because you need a handle to keep using the thing across many calls, and it's a *number* (not the real object) so the dangerous internals stay safely inside the kernel — a **safe, validated reference**, never the keys to the engine room.

---

## Q32: What is the lifecycle of an fd, and what if permission is revoked while a file is open?

**Question:**
> How long does an fd survive? And if I'm reading a file and an admin removes my permission to it mid-read, what happens?

**Answer:**

### Lifecycle of an fd
- 🟢 **Birth** — `open("abg.docx")`: OS checks permission, creates the kernel object, adds it to *your* process's fd table, returns the number (say fd 4).
- 🔵 **Life** — fd 4 stays valid across as many `read`/`write`/`seek` calls as you like. It lives **inside your process only**.
- 🔴 **Death** — when you `close(4)`, **or** the process exits (OS auto-closes all fds), **or** on `exec` for close-on-exec fds. The number is then **recycled** for a future `open()`.

Nuances: `fork()` gives a child copies of your fds; the underlying kernel object is **reference-counted** and freed only when the *last* fd referring to it closes.

### Permission revoked mid-read — the surprising rule
> **Permission is checked at `open()` time, NOT on every read.**

The fd *is* your capability — granted once at the door. So if an admin `chmod`s away your access **after** you've opened it:

> **Your open fd keeps working — you keep reading.** Only **future** `open()` calls are denied.

**Analogy:** your concert ticket is checked **at the door**; security doesn't re-check every song. If the venue stops new entries, everyone already inside keeps watching — only new arrivals are turned away.

| What the admin does | Your already-open fd |
|---|---|
| `chmod` removes your read permission | ✅ Keeps reading; only new opens blocked |
| Deletes the file (`rm`) | ✅ Keeps reading — data stays on disk until your fd closes ("deleted-but-open") |
| `chown` (changes owner) | ✅ Unaffected; only future opens re-check |
| Locks your user account | ✅ Running process + fds keep going (lockout stops new logins) |

### Honest exceptions
- True for **regular files on a local filesystem**. **NFS** may re-check and can fail mid-read.
- **Unmounting the disk** or pulling storage makes reads fail — that's the data vanishing, not a permission check.
- **Killing your process** closes the fd with it.
- **SELinux/AppArmor** can be configured to revoke more aggressively; plain `chmod`/`chown` do not touch open fds.

> **Principle:** access is checked at the **gate** (`open`), not while you hold the fd (already inside). Revoking permission later stops *new* entries but doesn't eject a valid open handle.

---

**Last Updated:** 2026-06-24
