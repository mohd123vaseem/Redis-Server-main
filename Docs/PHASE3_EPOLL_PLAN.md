# Phase 3 — Implementation Plan: `epoll` Event Loop

> **Goal:** Replace the thread-per-client model with a single-threaded, non-blocking
> `epoll` event loop (the Reactor pattern) — the architecture real Redis, nginx, and
> Node.js use. This is the headline differentiator from `IMPROVEMENT_PLAN.md` Phase 3.

**Created:** 2026-06-20
**Status (2026-06-21):** Functionally complete — **Steps 1–5, 7, 8 done**: a working
single-threaded epoll server (accept → frame → process → buffered write) with idle-connection
timeout (slow-loris protection) restored. Verified clean under ASan, TSan, and the full
`test_all.sh` harness (34/34); idle timeout verified live (idle client dropped after 300s).
**Step 6** (graceful shutdown) works via an atomic flag + 1s `epoll_wait` timeout, but **not**
yet via `signalfd` (optional polish). Edge-triggered (EPOLLET) deferred — level-triggered for now.
**Prereq:** Phase 2 (test suite) complete — 72 tests, CI, coverage in place.

---

## 1. Why This Change

| | Thread-per-client (today) | `epoll` event loop (target) |
|---|---|---|
| Concurrency unit | 1 thread per client | 1 thread for all clients |
| Memory/client | ~1 MB (thread stack) | a few KB (buffers) |
| Practical ceiling | ~1,000 connections | 10K–100K connections (C10K) |
| I/O | blocking `recv`/`send` | non-blocking + readiness events |
| Scheduling cost | kernel context-switches | one loop, no per-client switch |

It also **unblocks two deferred Phase 1 items** (1024-byte recv buffer truncation; non-blocking
dump) and forces us to write proper **RESP framing**, which is the part that was "throwaway to
do earlier" and is now done natively.

---

## 2. Current Architecture (what we're replacing)

Mapped to the actual code:

**`main.cpp`**
- Loads `dump.my_rdb`, constructs `RedisServer`, spawns a **joinable persistence thread**
  (parks on a `condition_variable`, dumps every 300s, woken early on shutdown), calls
  `server.run()`, then stops + joins the persistence thread.

**`RedisServer::run()` (`src/RedisServer.cpp:78`)**
- `socket()` → `SO_REUSEADDR` → `bind()` → `listen(backlog=10)` → **blocking `accept()` loop**.
- Per accepted client: sets `SO_RCVTIMEO=300s` (slow-loris guard) + `SO_KEEPALIVE`, reaps
  finished workers, then spawns a **`std::thread` worker**.
- **Worker (`:140`)**: blocks `SIGINT`, then loops: `recv()` into a **1024-byte buffer** →
  `cmdHandler.processCommand(request)` → `send()`. Breaks on `recv <= 0`, closes, sets `done`.
- Tracks clients in `std::vector<ClientWorker>` where `ClientWorker = {thread, done, socket}`.

**Shutdown (`:44`, `signalHandler` `:25`)**
- `SIGINT` handler sets `std::atomic<bool> g_shutdown` (async-signal-safe, nothing else).
- `sigaction` installed **without `SA_RESTART`** so `accept()` returns `EINTR` and the loop
  notices the flag.
- `shutdown()` dumps the DB, closes the listen socket, `::shutdown(SHUT_RDWR)`s each live
  worker to unblock its `recv()`, joins all threads.

**Command layer (`RedisCommandHandler.cpp`)**
- `processCommand(string) -> string` is **pure** (already unit-tested, 19+ tests).
- `parseRespCommand()` assumes the **entire command is in one buffer** — the core assumption
  that breaks under non-blocking I/O.

### The assumptions that break under `epoll`
1. **"One `recv()` == one complete command."** False on a byte stream — commands can span
   multiple reads (partial reads) or arrive batched (pipelining). → need **framing + input buffer**.
2. **"`send()` writes everything."** False — the kernel send buffer can fill; `send()` returns
   `EAGAIN` with bytes unsent. → need **output buffer + `EPOLLOUT` backpressure**.
3. **"`SO_RCVTIMEO` protects against slow loris."** It does nothing on a non-blocking socket. →
   need an **idle-timeout sweep**.
4. **"Blocking `accept()` + `EINTR` drives shutdown."** Replaced by `epoll_wait`. → need
   **`signalfd`** (or an `eventfd`/self-pipe) in the loop.

---

## 3. Target Architecture (the Reactor)

One thread owns one `epoll` instance. Everything is a file descriptor in that `epoll` set:

```
            ┌──────────────────────────── epoll_wait() ────────────────────────────┐
            │                                                                       │
   listen_fd (EPOLLIN)        client_fd … (EPOLLIN/EPOLLOUT)        signal_fd (EPOLLIN)
            │                              │                              │
       accept() loop              read → frame → process            SIGINT → begin
   (until EAGAIN), add            → queue reply → write              graceful shutdown
   each new client_fd             (buffer remainder on EAGAIN)
```

- **Non-blocking sockets**: `accept4(..., SOCK_NONBLOCK)`; `fcntl(fd, F_SETFL, O_NONBLOCK)`.
- **Level-triggered (LT) first** (see §6, Decision 1).
- **Per-client state** keyed by fd in a hash map.
- Persistence and DB locking: see §6, Decision 4.

---

## 4. New Components & Data Structures

### 4.1 `ClientState` (replaces `ClientWorker`)
```cpp
struct ClientState {
    int fd;
    std::string inbuf;          // accumulates bytes until a full command is parseable
    std::string outbuf;         // pending reply bytes not yet written (backpressure)
    std::chrono::steady_clock::time_point lastActivity;  // for idle timeout
    bool wantWrite = false;     // are we currently registered for EPOLLOUT?
};
```
Stored as `std::unordered_map<int /*fd*/, ClientState>` on the server — O(1) lookup per event,
no thread, no `done` flag.

### 4.2 RESP framing function (THE key new logic — and it's unit-testable)
A pure function that, given the current input buffer, reports the byte length of the **first
complete** command at the front (or that more data is needed / input is malformed):

```cpp
// Returns:
//   > 0  : number of bytes the first complete command occupies
//   == 0 : incomplete — need more bytes, wait for the next EPOLLIN
//   < 0  : malformed framing — caller should reply an error / drop the connection
ssize_t respFrameLength(const std::string& buf);
```
- For `*`-prefixed RESP: parse `*N\r\n`, then `N` × (`$len\r\n` + `len` bytes + `\r\n`),
  verifying every byte is present; return total length or 0 if truncated.
- For inline commands (not starting with `*`): a complete command ends at the next `\n`;
  return up-to-and-including it, else 0.

The read path then loops (handles **pipelining**):
```cpp
while ((ssize_t n = respFrameLength(c.inbuf)) > 0) {
    std::string frame = c.inbuf.substr(0, n);
    c.inbuf.erase(0, n);
    c.outbuf += handler.processCommand(frame);   // reuse the existing, tested handler
}
// n == 0 -> wait for more; n < 0 -> error/close
```
`processCommand` is reused unchanged — framing only finds boundaries; parsing/dispatch already work.

---

## 5. File-by-File Change Plan

| File | Change |
|---|---|
| `include/RedisServer.h` | Replace `ClientWorker`/`std::vector` with `ClientState`/`unordered_map`; add `epoll_fd`, `signal_fd`; declare event handlers (`onAccept`, `onReadable`, `onWritable`, `closeClient`, `sweepIdle`). |
| `src/RedisServer.cpp` | Rewrite `run()` as the epoll loop; remove the worker-thread lambda, `SO_RCVTIMEO`, per-thread `SIGINT` blocking; add non-blocking setup, `signalfd`, accept loop, read/write handlers, idle sweep. |
| `include/RedisCommandHandler.h` / `.cpp` | Add `respFrameLength()` (free function or static). `processCommand` unchanged. |
| `tests/test_resp_framing.cpp` *(new)* | Unit tests for `respFrameLength` (partial, complete, pipelined, inline, malformed, large). |
| `tests/test_command_handler.cpp` | Add a **>1024-byte** payload test (the deferred recv-buffer bug — now fixable). |
| `main.cpp` | Likely unchanged, OR move persistence into the loop via `timerfd` (Decision 4). `SIGINT` is now consumed by `signalfd`, so block it in `main` before the loop. |
| `src/RedisServer.cpp` (signal) | `setupSignalHandler` / `g_shutdown` replaced by `signalfd` integration (Decision 2). |

---

## 6. Key Design Decisions (with rationale)

**Decision 1 — Level-triggered first, edge-triggered (EPOLLET) as a later optimization.**
LT is "safe and chatty"; ET is "quiet and fast but only if you handle it like a pro" — ET
*requires* draining each fd until `EAGAIN` and a per-fd state machine, or you lose events and
hang. We ship **correct LT first**, then optionally convert to `EPOLLET` for the perf/interview
talking point. ([epoll(7) man page](https://www.man7.org/linux/man-pages/man7/epoll.7.html))

**Decision 2 — `signalfd` for shutdown, not the `sigaction` + atomic dance.**
Linux lets `signalfd`/`timerfd`/`eventfd` be polled by `epoll`. Putting `SIGINT` on a `signalfd`
in the epoll set means shutdown is "just another readable fd" — no async-signal-safety
constraints, no `EINTR` handling, no global atomic. Block `SIGINT` in the process first so the
kernel routes it to the `signalfd`. Cleaner and more epoll-native than today's approach.

**Decision 3 — No `EPOLLEXCLUSIVE`.** That flag only matters when *multiple threads* wait on the
same listen fd (thundering herd). We have **one** loop thread, so it's irrelevant. In LT mode we
simply `accept()` in a loop until `EAGAIN` to drain backlogged connections.

**Decision 4 — Keep `db_mutex`; keep the persistence thread (for now).**
After this change, command processing is single-threaded, so commands no longer race each other.
But the **background persistence thread still calls `dump()`**, which reads all stores — so
`db_mutex` is still required (event loop vs persistence thread). Two future options, documented
not done here:
  - *(a)* Move persistence into the loop via a `timerfd` → only the loop touches the DB → the
    mutex could be dropped — but an inline `dump()` blocks the loop for large datasets.
  - *(b)* The deferred **B7** fix: `fork()` + copy-on-write snapshot so the parent loop never
    blocks. This is the "real Redis" answer and the natural finish to B7.
  We keep the thread + mutex now (minimal, correct) and revisit (b) after the loop is stable.

**Decision 5 — Idle timeout via an `epoll_wait` timeout + activity sweep** (replaces
`SO_RCVTIMEO`). Give `epoll_wait` a timeout (e.g. 1s); each wake, sweep clients whose
`lastActivity` exceeds 300s and close them. Preserves the slow-loris protection that
`SO_RCVTIMEO` gave us in the blocking model.

**Decision 6 — `unordered_map<fd, ClientState>`.** O(1) per-event lookup; fds are unique and
dense-ish, so a hash map (or even a vector indexed by fd) is ideal.

---

## 7. Implementation Order (incremental — keep tests green at every step)

1. ✅ **DONE — RESP framing function + unit tests** (no epoll yet). Pure, TDD, epoll-proof.
   `respFrameLength` + `tests/test_resp_framing.cpp` (18 tests). Committed.
2. ✅ **COVERED — >1024-byte handling** — proven by `LargePayloadOver1024Bytes` in the framing
   tests; a separate handler-level test was redundant. Truncation bug now fixed by framing.
3. ✅ **DONE — Epoll skeleton**: non-blocking listen fd, `epoll_create1`, `epoll_wait` loop,
   accept loop (LT). In `run()` / `makeNonBlocking()` / `handleAccept()`. (Built directly into
   the real server below rather than as a throwaway echo stub.)
4. ✅ **DONE — Per-client `ClientState`** + input buffer + framing + `processCommand`. In
   `ClientState{inbuf,outbuf}` + `handleRead()`. Real single-threaded server, verified via `redis-cli`.
5. ✅ **DONE — Non-blocking writes + `EPOLLOUT`**: buffer unsent bytes on `EAGAIN`, register
   `EPOLLOUT`, flush on writable, deregister when drained. In `flushOutput()` / `updateEpollOut()`.
6. 🟡 **PARTIAL — graceful shutdown works, but not via `signalfd`.** Dump + close all fds + exit
   loop all work (`shutdown()`), driven by an atomic `g_shutdown` flag + a 1s `epoll_wait` timeout
   (closes the SIGINT race). Switching to `signalfd` is optional polish, not yet done.
7. ✅ **DONE — Idle-timeout sweep (slow-loris).** Each `ClientState` tracks `lastActivity`
   (stamped on accept, refreshed on read); `sweepIdleClients()` closes clients idle past 300s,
   run ~once a second off the `epoll_wait` timeout. Replaces the removed `SO_RCVTIMEO`; verified
   live (idle client dropped after 300s, next request → broken pipe).
8. ✅ **DONE — Ran `test_all.sh`** (34/34) + manual `redis-cli` + large/pipelined checks, all under
   ASan/TSan. (B7 fork-dump still a future item.)

Each step is independently testable; the unit tests and `test_all.sh` are the safety net.

---

## 8. Testing Strategy

- **`respFrameLength` unit tests (new, epoll-proof):** complete single command; **partial**
  (returns 0); **pipelined** (two commands back-to-back); **inline**; **malformed** (`<0`);
  **large** (>1024 bytes / multi-read); empty buffer. This is the natural home for the
  partial-read concepts.
- **`processCommand`:** unchanged — already covered (32 handler tests).
- **`test_all.sh`:** black-box, drives a real server with `redis-cli`; should pass **unchanged**
  and is the proof the rewrite is behavior-preserving. Add a large-value + pipelined case.
- **Concurrency (deferred Step 5 from `TEST_STRATEGY.md`):** revisit here. The old "many threads
  hammer one key" tests no longer model the architecture (command processing is single-threaded).
  The remaining real concurrency is **event loop vs persistence thread on `db_mutex`** — a much
  smaller surface. Decide then whether a targeted test is worth it.

---

## 9. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Partial-write bugs interleave replies | Strict per-client `outbuf`; only ever append, flush from front; never `send` from two places. |
| Framing bugs (hang on incomplete / over-read) | Dedicated unit tests for the `0` / `>0` / `<0` cases before wiring into the loop. |
| fd leaks / stale epoll entries | One `closeClient(fd)` that does `epoll_ctl(DEL)` + `close()` + map-erase; call it on every error/EOF path. |
| Busy-loop from leaving `EPOLLOUT` armed | Deregister `EPOLLOUT` the moment `outbuf` drains. |
| `dump()` blocks the single loop thread | Keep it on the persistence thread for now; plan B7 (fork+COW). |
| Behavior regression vs current server | `test_all.sh` must pass unchanged before declaring done. |

**Rollback:** the rewrite is confined to `RedisServer.{h,cpp}` (+ a framing function and tests).
The DB, command handler, and persistence are untouched, so reverting is a single-file revert if needed.

---

## 10. Interview Talking Points (the payoff)

- **The C10K problem** and why thread-per-client doesn't scale (stack memory + context switches).
- **Reactor pattern** / event-driven I/O (nginx, Redis, Node.js).
- **Level- vs edge-triggered**, and why ET needs drain-to-`EAGAIN` + a state machine.
- **The framing problem**: "TCP is a byte stream, not a message protocol — one `send` ≠ one
  `recv`. I solved partial reads with a per-client input buffer and a RESP length-prefix framer,
  unit-tested independently of the socket."
- **Backpressure**: buffering writes and using `EPOLLOUT` when the kernel send buffer fills.
- **`signalfd`/`timerfd`**: turning signals and timers into pollable fds for a uniform loop.

---

## 11. Out of Scope (explicitly)
- `EPOLLET` conversion (optional follow-up after LT is correct).
- `fork()`+COW non-blocking dump (B7) — planned, separate effort.
- Multi-threaded/sharded event loops (`EPOLLEXCLUSIVE`, `SO_REUSEPORT`) — beyond a single-loop design.
- Phase 4 benchmarking — separate phase; this plan only makes it *possible*.

---

## References
- [epoll(7) — Linux manual page](https://www.man7.org/linux/man-pages/man7/epoll.7.html) — LT vs ET semantics, the drain-to-`EAGAIN` rule.
- [epoll_ctl(2) — Linux manual page](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html) — `EPOLLEXCLUSIVE`, event registration.
- ["The method to epoll's madness" — Cindy Sridharan](https://copyconstruct.medium.com/the-method-to-epolls-madness-d9d2d6378642) — practical LT/ET tradeoffs.
