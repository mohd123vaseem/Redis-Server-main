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

#### Category B: Persistence Bugs (Silent Data Corruption ⚠️)

- [ ] **String values with spaces break on load**
  - Location: `RedisDatabase.cpp:359`
  - `SET msg "hello world"` → dumps as `K msg hello world` → loads as `value="hello"` (lost "world")
  - Fix: Use length-prefixed format (`K <keylen> <key> <vallen> <value>`)
- [ ] **List items with spaces break on load**
  - Same root cause — `>>` operator splits on whitespace
  - `LPUSH list "hello world"` → loads as 2 separate items
- [ ] **Hash values with spaces break on load**
  - Even worse: hash entries become separate orphan tokens silently skipped
- [ ] **Hash values with colons break on load**
  - Parser does `pair.find(':')` — if value contains `:`, the wrong colon is used
  - `HSET user url "http://x.com"` → field=`url`, value=`http`
- [ ] **TTLs are not persisted**
  - `expiry_map` is never dumped/loaded → all expirations lost on restart
- [ ] **No corruption detection**
  - No checksum, no version field, no sanity checks during load
- [ ] **Dump blocks all reads/writes**
  - `dump()` holds `db_mutex` for the entire serialization
  - For a 1M-key DB, server is unresponsive for seconds
  - Real Redis solution: `fork()` + copy-on-write for snapshot

#### Category C: Input Validation Bugs

- [x] **`std::stoi` in RESP parser can crash** ✅ FIXED
  - Location: `RedisCommandHandler.cpp:39, 48`
  - `*abc\r\n...` would throw `std::invalid_argument` → thread crashes
  - Wrapped parser body in try/catch — returns empty tokens on malformed input
- [x] **`std::stoi` in handlers can also crash** ✅ VERIFIED — already wrapped in try/catch
  - Used in `handleExpire`, `handleLrem`, `handleLindex`, `handleLset`
- [ ] **`keys()` may return duplicates**
  - Iterates all 3 stores without dedup; harmless in practice but technically wrong

#### Category D: Error Messages

- [x] **Wrong error message in `handleHget`** — says "HSET" instead of "HGET" ✅ FIXED
  - Location: `RedisCommandHandler.cpp:251`
- [x] **Typo "LEST" in `handleLset`** — should be "LSET" ✅ FIXED
  - Location: `RedisCommandHandler.cpp:229`

#### Category E: Resource / Security Bugs

- [x] **No `SO_RCVTIMEO` set on client sockets** ⚠️ **slow loris vulnerability** ✅ FIXED
  - Attacker connects + sends nothing → thread blocked forever in `recv()`
  - Set 300-second idle timeout after `accept()` (RedisServer.cpp:86-90)
- [ ] **No `SO_KEEPALIVE`** — dead clients leak threads
- [ ] **Thread vector grows forever**
  - Location: `RedisServer.cpp:74`
  - `threads.emplace_back(...)` never removes finished threads
  - 10,000 disconnected clients = 10,000 stale thread objects
- [ ] **Fixed 1024-byte recv buffer truncates large requests**
  - Location: `RedisServer.cpp:86`
  - Large `SET` values or `HMSET` calls get cut off
  - Fix: Loop-read until full RESP message is parsed
- [x] **`purgeExpired()` is public but should be private** ✅ FIXED
  - Moved to private section in RedisDatabase.h
- [x] **`purgeExpired()` not called by list/hash operations** ✅ FIXED
  - Added to all 9 list ops + all 9 hash ops
- [ ] **Detached persistence thread has no clean shutdown**
  - Location: `main.cpp:28`
  - `persistanceThread.detach()` runs `while(true)` forever; relies on `exit()`
- [ ] **Dead code in `RedisServer::run()` cleanup**
  - Location: `RedisServer.cpp:99-107`
  - `signalHandler` calls `exit(signum)` before `run()` ever reaches this code
- [ ] **`globalServer` is a workaround**
  - Because `RedisServer` isn't a singleton, a global pointer is used to bridge into `signalHandler`
  - Cleaner: make `RedisServer` a singleton like `RedisDatabase`

#### Category F: Code Smells (Nice to Have)

- [ ] **`lindex` and `lset` are 90% duplicate code**
  - Extract `resolveIndex(int& index, size_t size)` helper
- [ ] **Vector for lists is poor choice for middle deletions**
  - `lrem` mode 2/3 is O(N×count) due to shifts
  - Real Redis uses a quicklist (linked list of vectors)
- [ ] **`std::reverse_iterator<...>(fwdIter)` is verbose**
  - Replace with `std::make_reverse_iterator(fwdIter)` (C++14)

---

### Phase 2: Add a Test Suite ⭐⭐ (2–3 days)

**Why this matters:** *"I added a test suite"* is shorthand for *"I write production-quality code."* Zero-test projects scream "tutorial."

#### Tasks

- [ ] Integrate **Google Test** framework
- [ ] Unit tests for each database operation
  - Set/Get, List ops, Hash ops, Expiry
- [ ] Integration tests for RESP protocol
  - Valid commands, malformed input, edge cases
- [ ] Concurrency tests
  - Multiple threads hitting same key simultaneously
  - Verify mutex correctness
- [ ] Set up **GitHub Actions CI**
  - Build + test on every push
- [ ] Add code coverage reporting (e.g., `gcov` + Codecov badge)

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

**Persistence (silent data corruption):**
- [ ] Fix string values with spaces
- [ ] Fix list items with spaces
- [ ] Fix hash values with spaces
- [ ] Fix hash values containing `:`
- [ ] Persist TTLs in dump file
- [ ] Add corruption detection (checksum)
- [ ] Non-blocking dump (fork or async)

**Validation:**
- [x] Wrap `std::stoi` calls in try/catch (parser) ✅ — handlers already had try/catch
- [ ] Dedupe `keys()` output

**Error messages:**
- [x] Fix `handleHget` message ✅
- [x] Fix `handleLset` "LEST" typo ✅

**Resources / Security:**
- [x] Add `SO_RCVTIMEO` to prevent slow loris ✅
- [ ] Add `SO_KEEPALIVE`
- [ ] Prune finished threads from vector
- [ ] Loop-read until full RESP message (fix 1024-byte truncation)
- [x] Make `purgeExpired()` private ✅
- [x] Call `purgeExpired()` in list/hash ops too ✅
- [ ] Add clean shutdown signal for persistence thread
- [ ] Remove dead code in `RedisServer::run()` cleanup
- [ ] Replace `globalServer` with `RedisServer` singleton

**Code smells (optional):**
- [ ] Extract `resolveIndex()` helper for `lindex`/`lset`
- [ ] Use `std::make_reverse_iterator` in `lrem`
- [ ] Consider quicklist/deque for lists (real Redis approach)

### Phase 2: Testing
- [ ] Set up Google Test
- [ ] Unit tests for DB operations
- [ ] Integration tests for RESP
- [ ] Concurrency tests
- [ ] GitHub Actions CI
- [ ] Coverage badge

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
**Last Updated:** 2026-05-17
