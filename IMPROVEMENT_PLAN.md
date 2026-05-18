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

#### Bugs to Fix

- [ ] **`del()` returns false** — should return `erased`
  - Location: `RedisDatabase.cpp:75`
- [ ] **`del()` doesn't clear `expiry_map`**
  - Re-adding a deleted key inherits old TTL
- [ ] **`flushAll()` doesn't clear `expiry_map`**
  - Same TTL inheritance issue
- [ ] **Persistence breaks on values with spaces**
  - Location: `RedisDatabase.cpp:359`
  - Fix: Use length-prefixed format instead of space-separated
- [ ] **Wrong error message in `handleHget`** — says "HSET"
  - Location: `RedisCommandHandler.cpp:251`
- [ ] **Typo "LEST" in `handleLset`**
  - Location: `RedisCommandHandler.cpp:229`
- [ ] **`std::stoi` in RESP parser can crash on garbage input**
  - Wrap in try/catch
- [ ] **Add `SO_RCVTIMEO` to prevent slow loris attack**
  - Set 30-second idle timeout on each client socket

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
- [ ] Fix `del()` return value
- [ ] Fix `del()` expiry leak
- [ ] Fix `flushAll()` expiry leak
- [ ] Fix persistence with spaces (length-prefixed format)
- [ ] Fix error message typos
- [ ] Wrap `std::stoi` in try/catch
- [ ] Add `SO_RCVTIMEO` timeout

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
