# Redis Server — Production-Grade Improvement Plan

> **Goal:** Transform this project from a "tutorial follower" into a standout portfolio piece for FAANG SDE1/SDE2 interviews.

---

## Why Improve This Project?

### Current State vs Target State

| | Current State | After Improvements |
|---|---|---|
| **FAANG interview material?** | ❌ Generic tutorial | ✅ Standout project |
| **Conversation depth** | ~5 minutes | 30+ minutes of deep tech |
| **Differentiation** | Same as 1000 others | Top 5% of "Redis clones" |
| **SDE1 viability** | Weak | **Strong** |
| **SDE2 viability** | Very weak | **Plausible (with strong DSA)** |

---

## The Roadmap (Recommended Order)

### Phase 1: Fix Existing Bugs ⭐ (1–2 days)

**Why first:** Quick wins, builds confidence, gives you talking points like *"I found and fixed these bugs."*

#### Category A: Logic Bugs (Critical)

- [x] **`del()` returns false** — should return `erased` ✅ FIXED
  - Location: `RedisDatabase.cpp:75`
  - Impact: Client always sees `:0` even when key was deleted
- [x] **`del()` doesn't clear `expiry_map`** ✅ FIXED
  - Re-adding a deleted key inherits old TTL → silently expires
- [x] **`flushAll()` doesn't clear `expiry_map`** ✅ FIXED
  - Same TTL inheritance issue after FLUSHALL
- [x] **`HSET` only stores the first field/value pair** ⚠️ **Silent data loss** ✅ FIXED
  - Location: `RedisCommandHandler.cpp` → `handleHset()`
  - Discovered: 2026-05-27 via `test_all.sh`
  - Real Redis supports `HSET key f1 v1 f2 v2 ...` (multi-field, since Redis 4.0)
  - Was: read only `tokens[2]`/`tokens[3]` — extra pairs silently dropped
  - Fix: Loops through pairs; returns count of NEW fields added (per Redis spec, not just `:1`)
  - Also: validates even token count to reject `HSET key f1 v1 f2` (stray field)

#### Category B: Persistence Bugs (Silent Data Corruption ⚠️)

B1–B6 fixed together via one format change — see [`BUG_FIXES.md`](BUG_FIXES.md) for the full rationale and the cross-cutting format design.

- [x] **String values with spaces break on load** ✅ FIXED
  - Length-prefixed K records (`K <keylen> <key> <vallen> <value>`)
- [x] **List items with spaces break on load** ✅ FIXED
  - Length-prefixed L records (`L <keylen> <key> <count> <len> <item> ...`)
- [x] **Hash values with spaces break on load** ✅ FIXED
  - Length-prefixed H records (paired `<flen> <field> <vlen> <value>`)
- [x] **Hash values with colons break on load** ✅ FIXED
  - Eliminated by construction — no in-band delimiter in H records anymore
- [x] **TTLs are not persisted** ✅ FIXED
  - New E record type stores absolute unix epoch ms; converted to/from steady_clock at dump/load
- [x] **No corruption detection** ✅ FIXED
  - Added `REDIS_DUMP_V1` magic header + CRC32 over body; load refuses bad files (fail-loud)
- [ ] **Dump blocks all reads/writes** ⏸️ **DEFERRED to after Phase 3 (epoll)**
  - `dump()` still holds `db_mutex` for the entire serialization (now serialized to a memory buffer first, then written — slightly faster but still blocking)
  - Real Redis solution: `fork()` + copy-on-write. Cleaner to do after Phase 3 when the I/O model is single-threaded by design (fork() in a multithreaded program is fragile).

#### Category C: Input Validation Bugs

- [x] **`std::stoi` in RESP parser can crash** ✅ FIXED
  - Location: `RedisCommandHandler.cpp:39, 48`
  - `*abc\r\n...` would throw `std::invalid_argument` → thread crashes
  - Wrapped parser body in try/catch — returns empty tokens on malformed input
- [x] **`std::stoi` in handlers can also crash** ✅ VERIFIED — already wrapped in try/catch
  - Used in `handleExpire`, `handleLrem`, `handleLindex`, `handleLset`
- [x] **`keys()` may return duplicates** ✅ FIXED
  - Location: `RedisDatabase.cpp:42-51`
  - Was: iterated all 3 stores into a vector; a key in multiple stores appeared multiple times
  - Fix: collect into `std::unordered_set` then convert to vector — O(N) dedup, no behavior change for callers

#### Category D: Error Messages

- [x] **Wrong error message in `handleHget`** — says "HSET" instead of "HGET" ✅ FIXED
  - Location: `RedisCommandHandler.cpp:251`
- [x] **Typo "LEST" in `handleLset`** — should be "LSET" ✅ FIXED
  - Location: `RedisCommandHandler.cpp:229`

#### Category E: Resource / Security Bugs

- [x] **No `SO_RCVTIMEO` set on client sockets** ⚠️ **slow loris vulnerability** ✅ FIXED
  - Attacker connects + sends nothing → thread blocked forever in `recv()`
  - Set 300-second idle timeout after `accept()` (RedisServer.cpp:86-90)
- [x] **No `SO_KEEPALIVE`** — dead clients leak threads ✅ FIXED
  - Location: `RedisServer.cpp:92-93`
  - OS-level probes detect silently-dead clients (network failure, crash) so `recv()` returns instead of blocking forever
- [x] **Thread vector grows forever** ✅ FIXED (Group B)
  - Was: `threads.emplace_back(...)` never removed finished threads
  - 10,000 disconnected clients = 10,000 stale thread objects
  - Fix: per-worker `done` flag; accept loop reaps finished workers (join + erase); `shutdown()` joins the rest. Vector now tracks only live connections.
- [ ] **Fix 1024-byte recv buffer truncates large requests** ⏸️ **DEFERRED to Phase 3 (epoll)**
  - Location: `RedisServer.cpp:96`
  - Large `SET` values or `HMSET` calls get cut off; pipelined commands processed only partially
  - Root cause: TCP is a byte stream, not a message protocol — one `send()` ≠ one `recv()`. Need application-layer framing (RESP's `*N\r\n` / `$N\r\n` length prefixes) + a per-client input buffer that accumulates bytes until a complete message is parseable.
  - **Why deferred:** ~60% of the fix (blocking recv loop in a thread) would be throwaway once epoll lands. The RESP framing function and per-client buffer pattern get written natively in Phase 3, where non-blocking sockets make correct framing unavoidable. Concepts (TCP-as-stream, partial reads, framing) land better in that context.
  
- [x] **`purgeExpired()` is public but should be private** ✅ FIXED
  - Moved to private section in RedisDatabase.h
- [x] **`purgeExpired()` not called by list/hash operations** ✅ FIXED
  - Added to all 9 list ops + all 9 hash ops
- [x] **Detached persistence thread has no clean shutdown** ✅ FIXED (Group B)
  - Was: `persistanceThread.detach()` ran `while(true)` forever; relied on `exit()`
  - Fix: joinable thread parked on a `condition_variable` (300s timeout, wakes early on shutdown); `main()` signals stop + joins it. SIGINT blocked in the thread so only main fields Ctrl+C.
- [x] **Dead code in `RedisServer::run()` cleanup** ✅ FIXED (surgical — Group A)
  - Removed unreachable cleanup block; left TODO marker
  - Root cause (`exit()` in `signalHandler`) → see Group B item below
- [x] **`signalHandler` calls `exit()` — prevents proper shutdown cleanup** ✅ FIXED (Group B)
  - Was: `exit(signum)` in the handler killed the process before any cleanup
  - Fix: handler now only sets a file-scope `std::atomic<bool> g_shutdown` (async-signal-safe); installed via `sigaction` without `SA_RESTART` so `accept()` returns `EINTR`. Real cleanup (dump + close + join) runs in `run()` on the main thread.
- [x] **`globalServer` is a workaround** ✅ FIXED (Group B)
  - Was: a global pointer bridged the C-style handler into `globalServer->shutdown()`
  - Fix: deleted entirely. Once the handler only sets `g_shutdown`, no instance access is needed. (Singleton considered but rejected — a server needs a `port`, which makes singleton init awkward; a bare atomic removed the global with less code.)

#### Category F: Code Smells (Nice to Have)

- [ ] **`lindex` and `lset` are 90% duplicate code**
  - Extract `resolveIndex(int& index, size_t size)` helper
- [ ] **Vector for lists is poor choice for middle deletions**
  - `lrem` mode 2/3 is O(N×count) due to shifts
  - Real Redis uses a quicklist (linked list of vectors)
- [x] **`std::reverse_iterator<...>(fwdIter)` is verbose** ✅ FIXED
  - Replaced with `std::make_reverse_iterator(fwdIter)` (RedisDatabase.cpp:228)

---

### Phase 2: Add a Test Suite ⭐⭐ (2–3 days)

**Why this matters:** *"I added a test suite"* is shorthand for *"I write production-quality code."* Zero-test projects scream "tutorial."

> 📋 **Working tracker:** see [`TEST_STRATEGY.md`](TEST_STRATEGY.md) for the ordered step-by-step
> plan, the reasoning behind the order, and which steps survive the Phase 3 (epoll) rewrite.

#### Tasks

- [x] Integrate **Google Test** framework ✅ — vendored under `third_party/`, `make test` target
- [x] Unit tests for each database operation ✅ — 37 tests (`tests/test_database.cpp`)
  - Set/Get, List ops, Hash ops, Expiry, persistence round-trip, corruption detection
- [x] Integration tests for RESP protocol ✅ — 19 tests (`tests/test_command_handler.cpp`)
  - Valid commands, malformed input (no crash), inline + array framing, error messages
- [ ] Concurrency tests ⏸️ **decision pending** — defer to after Phase 3 (epoll makes the
  server single-threaded, so most of these would be throwaway)
  - Multiple threads hitting same key simultaneously
  - Verify mutex correctness
- [x] Set up **GitHub Actions CI** ✅ — `.github/workflows/ci.yml`, first run green in 55s
  - Build + unit tests + `test_all.sh` on every push and PR
- [x] Add code coverage reporting ✅ — `make coverage` (gcov + gcovr), Codecov upload + badge

**Status (2026-06-20):** Phase 2 effectively complete. 69 tests passing locally and in CI;
Codecov badge live (~86%). Concurrency tests deferred to after Phase 3 (epoll). See
[`TEST_STRATEGY.md`](TEST_STRATEGY.md).

#### Outcome

A README badge showing **"Tests: passing | Coverage: 85%"** instantly signals seriousness.

---

### Phase 3: Replace Threading with `epoll` ⭐⭐⭐ (1–2 weeks)

**This is the game changer.** Most candidates cannot explain epoll. If you can, you stand out immediately.

#### Architecture Change

```
BEFORE: Thread-per-client                AFTER: Event loop with epoll
─────────────────────────                ──────────────────────────
1 client = 1 thread                      1 thread handles 10,000 clients
~1 MB RAM per client                     ~few KB per client
Limited to ~1,000 connections            Scales to 10K–100K connections
Blocking recv()/send()                   Non-blocking + event notification
```

#### What You'll Build

- [ ] **Non-blocking sockets** with `fcntl(fd, F_SETFL, O_NONBLOCK)`
- [ ] **Epoll instance** via `epoll_create1()`
- [ ] **Event registration** via `epoll_ctl()`
- [ ] **Event loop** via `epoll_wait()`
- [ ] **Per-client state machine**:
  - Input buffer (accumulate partial reads)
  - Output buffer (queue partial writes)
  - Parser state (incomplete commands)
- [ ] **Edge-triggered (EPOLLET) handling**
- [ ] **Graceful client disconnect handling**

#### Concepts You'll Master

- The **C10K problem** (historical context)
- **Event-driven architecture** (Nginx, Redis, Node.js use this)
- **Non-blocking I/O** vs blocking I/O
- **Edge-triggered vs level-triggered** events
- **The Reactor pattern**

These are all common systems-interview topics.

---

### Phase 4: Benchmarking ⭐⭐ (1–2 days)

**Interview gold:** Concrete numbers are 10x more impressive than vague claims.

#### Tasks

- [ ] Benchmark with **`redis-benchmark`** tool
- [ ] Measure:
  - Operations per second (ops/sec)
  - Latency p50, p95, p99
  - Memory usage per connection
  - Max concurrent connections
- [ ] Compare **thread-per-client vs epoll** versions
- [ ] Document results in README with charts

#### The Killer Line for Interviews

> *"After switching from thread-per-client to epoll, my server handles **50,000 req/sec** vs **5,000 req/sec** previously, while using **10x less memory** per connection."*

#### Bonus: Quick Active TTL Eviction (~2-3 hours)

**Problem:** Current `purgeExpired()` is **O(N)** — scans the entire `expiry_map` on every read/write. Worse, expired keys leak memory if never accessed (lazy-only eviction).

**Solution:** Add a background thread that samples ~20 random expired keys per second (same approach as real Redis). Add an `activeExpire()` method, spawn a detached thread in `main.cpp` calling it every second.

**Talking point:** *"I added active eviction with random sampling, similar to real Redis. For production scale, a bucketed timer wheel would give O(log N) instead of O(N)."*

---

### Phase 5 (Stretch Goals): Real Differentiators

Only do these if you have extra time. **Pick ONE**, don't try all.

| Feature | What It Demonstrates |
|---|---|
| **AOF (Append-Only File) persistence** | Durability trade-offs, write-ahead logging |
| **Pub/Sub** | Protocol extension, fan-out patterns |
| **Cluster mode (basic)** | Distributed systems, consistent hashing |
| **Lua scripting** | Embedded interpreter, advanced |
| **Master/Replica replication** | Distributed systems, consensus |
| **Full TTL optimization** (bucketed timer wheel) | Advanced data structures, O(log N) expiry — only if Phase 4's quick fix isn't enough |

---

## Time Investment vs Return

| Path | Time | FAANG SDE1 Chance |
|---|---|---|
| Leave as-is | 0 hours | Low |
| Just fix bugs | ~3 days | Slightly better |
| **Bugs + tests + epoll + benchmarks** | **~3–4 weeks part-time** | **Significantly better** |
| Build a NEW project from scratch | 2–3 months | Comparable |

**Verdict:** Upgrading this project is **way more time-efficient** than starting over. You already understand the codebase.

---

## Interview Impact

### Before Improvements

> *"I built a Redis clone in C++. It supports basic commands, uses a singleton database, and persists to disk every 5 minutes."*

**Interviewer reaction:** *"OK, next question."*

### After Improvements

> *"I built a Redis-compatible server in C++ that handles 50K req/sec on a single thread using epoll. I wrote unit tests with Google Test, set up CI, and benchmarked it against thread-per-client to measure the improvement. I learned about non-blocking I/O, the C10K problem, and edge-triggered vs level-triggered events. The biggest challenge was handling partial reads when commands span multiple recv() calls — I solved it with per-client input buffers."*

**Interviewer reaction:** *"Tell me more. How did you handle X? What if Y? Have you considered Z?"*

That's a **20-minute deep technical conversation** vs *"OK, next question."*

---

## ⚠️ Critical Reminder

Even with a perfect project, **DSA is still ~60% of FAANG interviews**. You can have the best Redis clone on GitHub and still fail because you couldn't solve a graph problem.

### Recommended Balance

```
40% LeetCode (300+ problems, medium/hard focus)
30% System design study (for SDE2)
20% Project work (this Redis clone upgrade)
10% Behavioral prep + resume + applying
```

---

## Realistic Expectations

### SDE1 (New Grad / Early Career)

✅ **Very competitive** if you have:
- This upgraded project (epoll + benchmarks + tests)
- 300+ LeetCode problems solved (medium/hard)
- Decent communication
- Solid resume

### SDE2 (2–5 yrs Experience)

⚠️ **Possible but harder** — FAANG SDE2 expects:
- Real production experience (work history)
- System design knowledge (DDIA-level)
- Track record of shipping at scale
- This project = strong supplement, not a substitute

---

## Final Checklist

Use this as your tracker:

### Phase 1: Bug Fixes

**Logic:**
- [x] Fix `del()` return value ✅
- [x] Fix `del()` expiry leak ✅
- [x] Fix `flushAll()` expiry leak ✅
- [x] Fix `HSET` to handle multiple field/value pairs (silent data loss) ✅

**Persistence (silent data corruption):**
- [x] Fix string values with spaces ✅
- [x] Fix list items with spaces ✅
- [x] Fix hash values with spaces ✅
- [x] Fix hash values containing `:` ✅
- [x] Persist TTLs in dump file ✅
- [x] Add corruption detection (checksum) ✅
- [ ] Non-blocking dump (fork or async) ⏸️ deferred to after Phase 3

**Validation:**
- [x] Wrap `std::stoi` calls in try/catch (parser) ✅ — handlers already had try/catch
- [x] Dedupe `keys()` output ✅

**Error messages:**
- [x] Fix `handleHget` message ✅
- [x] Fix `handleLset` "LEST" typo ✅

**Resources / Security:**
- [x] Add `SO_RCVTIMEO` to prevent slow loris ✅
- [x] Add `SO_KEEPALIVE` ✅
- [x] Prune finished threads from vector ✅ (Group B)
- [x] Make `purgeExpired()` private ✅
- [x] Call `purgeExpired()` in list/hash ops too ✅
- [x] Add clean shutdown signal for persistence thread ✅ (Group B)
- [x] Remove dead code in `RedisServer::run()` cleanup ✅
- [x] Remove `exit()` from `signalHandler` & restore cleanup ✅ (Group B)
- [x] Remove `globalServer` global (atomic flag, not singleton) ✅ (Group B)

**Code smells (optional):**
- [ ] Extract `resolveIndex()` helper for `lindex`/`lset`
- [x] Use `std::make_reverse_iterator` in `lrem` ✅
- [ ] Consider quicklist/deque for lists (real Redis approach)

### Phase 2: Testing
- [x] Set up Google Test ✅
- [x] Unit tests for DB operations ✅ (37)
- [x] Integration tests for RESP ✅ (19)
- [ ] Concurrency tests ⏸️ (deferred to after Phase 3 — decision pending)
- [x] GitHub Actions CI ✅ (green, 55s)
- [x] Coverage badge ✅ (Codecov live, ~86%)

### Phase 3: epoll
- [ ] Non-blocking sockets
- [ ] Epoll event loop
- [ ] Per-client buffer state machine
- [ ] Partial read/write handling
- [ ] Edge-triggered handling

### Phase 4: Benchmarks
- [ ] Run `redis-benchmark`
- [ ] Document ops/sec, latency, memory
- [ ] Comparison: threads vs epoll
- [ ] Charts in README

### Phase 5: (Optional) Pick One
- [ ] AOF persistence, OR
- [ ] Pub/Sub, OR
- [ ] Cluster mode, OR
- [ ] Replication

---

## TL;DR

**Yes — this upgrade is worth doing.**

1. ✅ Fix bugs (~3 days)
2. ✅ Add tests (~3 days)
3. ✅ Add epoll (~2 weeks) ← the differentiator
4. ✅ Benchmark + document (~2 days)

Total time: **~3–4 weeks part-time**.

After this, you'd be a **competitive SDE1 candidate** (paired with strong DSA). For SDE2, you'd need this + real work experience + strong system design.

The **"tutorial follower → real engineer"** transition is one of the best things you can show on a resume.

---

**Created:** 2026-05-17
**Last Updated:** 2026-06-20
