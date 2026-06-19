# Test Strategy — Phase 2 of the Improvement Plan

> **Scope:** This document is the working tracker for **Phase 2 (Add a Test Suite)** from
> [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md). It records *what* we test, *in what order*,
> and *why that order* — so we don't lose the thread across sessions.

**Created:** 2026-06-19
**Status:** Step 1 complete ✅ — scaffold verified green (`make gtest` + `make test`, 1 test passing)

---

## Guiding Principle

> Build the foundation first, test the most **stable and valuable** layer next,
> **automate early**, and **defer anything the epoll rewrite (Phase 3) will throw away.**

Two facts shape every decision below:

1. **`RedisDatabase` is a singleton** (private ctor, one global instance via `getInstance()`).
   State persists across tests, so every test must reset it first. A Google Test
   *fixture* that calls `flushAll()` in `SetUp()` handles this — **no code restructuring needed.**
2. **Phase 3 replaces threading with epoll.** The data layer and RESP-parsing logic
   survive that rewrite untouched. The thread-per-client concurrency model does **not** —
   so concurrency tests are partly throwaway and are deliberately scheduled last.

---

## What We Already Have (and why it's not Phase 2)

[`test_all.sh`](test_all.sh) — a **black-box bug-regression harness** (bash + `redis-cli`).
It manages its own server on port 6390, does restart cycles, and asserts one case per
documented fix in [`BUG_FIXES.md`](BUG_FIXES.md). It is **good** end-to-end coverage of the
bug fixes, but it is **not** the Phase 2 deliverable:

- It is a shell script, not a unit-test framework.
- It tests through the socket — it cannot test a function in isolation.
- It has no concurrency tests and no CI.

`test_all.sh` stays. CI (Step 3) will run it alongside the new unit tests.

---

## The Order

```
1. GoogleTest scaffold + 1 test   ← foundation
2. RedisDatabase unit tests        ← epoll-proof, highest value   ⭐ the meat
3. GitHub Actions CI + badge       ← automate early
4. RESP parser integration tests   ← epoll-proof
5. Concurrency tests               ← OPTIONAL, epoll will obsolete much of it
6. Coverage badge                  ← polish
```

---

### Step 1 — Google Test scaffolding (the foundation) ✅ DONE

- [x] Add a `tests/` directory
- [x] Add `gtest` (fetch) + `test` targets to the Makefile — gtest vendored under `third_party/` (gitignored), compiled from source with g++ (no cmake)
- [x] Write **one trivial test** (`set("foo","bar")` → `get` returns `"bar"`) to prove the plumbing
- [x] Confirm `make test` builds and the one test passes — **verified green 2026-06-19**

**Run:** `make gtest` (once) then `make test`.

**Why first:** every later step plugs into this. Get a single green test before writing fifty.

---

### Step 2 — Unit tests for `RedisDatabase` (the data layer) ⭐

Test each operation **directly** (no socket). Use a fixture that calls `flushAll()` in `SetUp()`.

- [ ] Key/Value: `set`/`get`, `get` of missing key, `type`, `rename`
- [ ] `del` — returns `true`/`1` on hit, `false`/`0` on miss (Phase 1 bug A1)
- [ ] `del` — clears the TTL so a re-added key does not inherit old expiry (bug A2)
- [ ] `flushAll` — clears the TTL map too (bug A3)
- [ ] `keys()` — no duplicates when a name exists in multiple stores (bug C3)
- [ ] List: `lpush`/`rpush`/`lpop`/`rpop`/`llen`/`lindex`/`lset`
- [ ] `lrem` — all three modes (count > 0, count < 0, count == 0)
- [ ] Hash: `hset` single + **multi-field** (bug A4), `hget`, `hdel`, `hexists`, `hlen`, `hgetall`, `hkeys`, `hvals`, `hmset`
- [ ] Expiry: `expire` then immediate read = present; after timeout = gone (lazy purge)
- [ ] Persistence round-trip: `dump()` then `load()` preserves values with **spaces and colons** (bugs B1–B4) and TTLs (B5)
- [ ] Persistence: `load()` rejects a corrupted dump — bad magic header / bad CRC (bug B6)

**Why second:** pure logic, easiest to isolate; **survives the epoll rewrite untouched**;
locks in the Phase 1 bug fixes at the unit level instead of only end-to-end.

---

### Step 3 — GitHub Actions CI + badge

- [ ] Add `.github/workflows/ci.yml`
- [ ] CI steps: check out → install `redis-cli` → `make` → `make test` → `./test_all.sh`
- [ ] Confirm the workflow goes green on a push
- [ ] Add the `![CI](...)` "passing" badge to `README.md`

**Why here, not last:** CI's value compounds the longer it runs; the green badge is
motivating; and it catches breakage *while* the rest of the tests are still being written.

---

### Step 4 — Integration tests for the RESP parser (`RedisCommandHandler`)

- [ ] Feed raw RESP byte-strings, assert the parsed command tokens
- [ ] Malformed input does not crash (e.g. `*abc\r\n...`) — bug C1
- [ ] Correct error messages: HGET names "HGET", LSET names "LSET" (bugs D1/D2)
- [ ] Edge cases: empty array, wrong arg counts, inline vs array form

**Why fourth:** parser logic also **survives epoll** (framing changes in Phase 3, but
parsing a *complete* RESP message does not). Higher setup cost than data-layer tests.

---

### Step 5 — Concurrency tests ⚠️ OPTIONAL

- [ ] Many threads hammering the same key — assert no corruption / no crash (validates `db_mutex`)
- [ ] Mixed read/write load across all three stores

**Why nearly last — real caveat:** these validate the **thread-per-client** model.
**Phase 3 (epoll) makes the server single-threaded, deleting most of what these check.**
Do this now only for the *"I verified mutex correctness"* talking point; otherwise skip and
add lighter concurrency checks after epoll lands.

**Decision needed before starting Step 5:** do it now, or defer past Phase 3? *(unresolved)*

---

### Step 6 — Coverage reporting (polish)

- [ ] Compile tests with `--coverage` (gcov)
- [ ] Generate a coverage report (lcov / gcovr)
- [ ] Add a coverage badge to `README.md` (optional: Codecov)

**Why last:** it measures the tests that already exist — pointless until they do.

---

## Durable vs Throwaway (quick reference)

| Step | Survives epoll rewrite? |
|---|---|
| 1. GoogleTest scaffold | ✅ Yes |
| 2. RedisDatabase unit tests | ✅ Yes |
| 3. CI + badge | ✅ Yes (recipe may add an epoll build later) |
| 4. RESP parser tests | ✅ Yes |
| 5. Concurrency tests | ⚠️ Mostly **no** — thread model is removed |
| 6. Coverage badge | ✅ Yes |

**Steps 1–4 are the durable core. Step 5 is the one conscious bet.**

---

## Tracker Summary

- [x] Step 1 — GoogleTest scaffold + 1 test ✅
- [ ] Step 2 — RedisDatabase unit tests
- [ ] Step 3 — GitHub Actions CI + badge
- [ ] Step 4 — RESP parser integration tests
- [ ] Step 5 — Concurrency tests *(optional / decide first)*
- [ ] Step 6 — Coverage badge
