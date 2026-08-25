# Test Strategy — Phase 2 of the Improvement Plan

> **Scope:** This document is the working tracker for **Phase 2 (Add a Test Suite)** from
> [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md). It records *what* we test, *in what order*,
> and *why that order* — so we don't lose the thread across sessions.

**Created:** 2026-06-19
**Status:** Phase 2 COMPLETE ✅ — Steps 1–4 + 6 done; Step 5 (concurrency) deliberately
deferred past Phase 3 (epoll). 72 tests green locally + in CI; Codecov badge live.
Local coverage: **97.9% lines, 100% functions** (branch 60% — remaining branches are
exception/library/defensive paths, intentionally not chased).

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

### Step 2 — Unit tests for `RedisDatabase` (the data layer) ⭐ ✅ DONE

37 tests in `tests/test_database.cpp`, fixture resets the singleton via `flushAll()` in
`SetUp()`. **Verified green 2026-06-19** (`make test`, 37 passing, 2 ms).

- [x] Key/Value: `set`/`get`, `get` of missing key, `type`, `rename`
- [x] `del` — returns `true` on hit, `false` on miss (Phase 1 bug A1)
- [x] `del` — clears the TTL so a re-added key does not inherit old expiry (bug A2) — verified deterministically via dump inspection (no sleeps)
- [x] `flushAll` — clears the TTL map too (bug A3)
- [x] `keys()` — no duplicates when a name exists in multiple stores (bug C3)
- [x] List: `lpush`/`rpush`/`lpop`/`rpop`/`llen`/`lindex`/`lset`
- [x] `lrem` — all three modes (count > 0, count < 0, count == 0)
- [x] Hash: `hget`, `hdel`, `hexists`, `hlen`, `hgetall`, `hkeys`, `hvals`, `hmset` (multi-field at DB layer)
- [x] Persistence round-trip: `dump()` then `load()` preserves values with **spaces and colons** (bugs B1–B4) and TTLs (B5)
- [x] Persistence: `load()` rejects a corrupted dump — missing file / bad magic header / bad CRC (bug B6), without wiping good in-memory data

**Note — moved to Step 4 (integration):** multi-field `HSET` (bug A4) lives in
`RedisCommandHandler::handleHset`, not the DB layer (`db.hset` is single-field; `db.hmset`
covers multi-field and is tested here). Timing-based TTL *expiry* stays in `test_all.sh`.

**Why second:** pure logic, easiest to isolate; **survives the epoll rewrite untouched**;
locks in the Phase 1 bug fixes at the unit level instead of only end-to-end.

---

### Step 3 — GitHub Actions CI + badge ✅ DONE

- [x] Add `.github/workflows/ci.yml`
- [x] CI steps: check out → install `redis-cli` → `make gtest` → `make` → `make test` → `./test_all.sh`
- [x] Confirm the workflow goes green on a push — **first run passed in 55s (2026-06-19)**
- [x] Add the `![CI](...)` "passing" badge to `README.md`

**Why here, not last:** CI's value compounds the longer it runs; the green badge is
motivating; and it catches breakage *while* the rest of the tests are still being written.

---

### Step 4 — Integration tests for the RESP parser (`RedisCommandHandler`) ✅ DONE

19 tests in `tests/test_command_handler.cpp`, driving `processCommand(string) -> string`
directly (no socket). Fixture resets the DB singleton in `SetUp()`. **Verified green
2026-06-20** (`make test`, 56 total passing).

- [x] Feed raw RESP frames + inline fallback, assert the RESP reply
- [x] Malformed input does not crash (`*abc\r\n...`, via `EXPECT_NO_THROW`) — bug C1
- [x] Correct error messages: HGET names "HGET" not "HSET" (D1); LSET names "LSET" not "LEST" (D2)
- [x] Multi-field `HSET` stores every pair + counts only new fields (A4); rejects odd args
- [x] Edge cases: empty input, unknown command, case-insensitivity, arity errors, reply formatting (bulk/nil/integer/array)

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

**Decision (2026-06-20): DEFERRED past Phase 3.** Concurrency tests validate the
thread-per-client `db_mutex` model, which the epoll rewrite removes — writing them now
would mean testing code about to be deleted. Revisit after epoll with tests that match
the single-threaded event-loop architecture.

---

### Step 6 — Coverage reporting (polish) ✅ DONE

- [x] `make coverage` target — instruments `src/RedisDatabase.cpp` + `src/RedisCommandHandler.cpp` with `--coverage`, runs the tests, summarises with `gcovr`
- [x] Generate a coverage report (gcovr → `build/cov/coverage.xml` + `coverage.html`)
- [x] Codecov upload wired into CI (`coverage` job) + badge in `README.md`

**Result (2026-06-20):** local gcovr **97.9% lines, 100% functions** (branch 60%); Codecov
badge live (blends partial/branch coverage, so it reads a bit lower than pure line coverage
— both correct). **72 tests total.** Coverage gaps found via the HTML report were closed:
13 handler tests to drive every command through `processCommand`, then a batch covering all
arity guards, the `purgeExpired` removal branch (via a past-dated TTL), and `rename`'s
list/hash/TTL branches.

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
- [x] Step 2 — RedisDatabase unit tests ✅ (37 tests)
- [x] Step 3 — GitHub Actions CI + badge ✅ (first run green, 55s)
- [x] Step 4 — RESP parser integration tests ✅ (19 tests, 56 total)
- [⏸] Step 5 — Concurrency tests — **DEFERRED past Phase 3** (decided 2026-06-20)
- [x] Step 6 — Coverage badge ✅ (Codecov live, ~86% / 93.6% local)
