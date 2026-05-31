# Bug Fix Log

> **Purpose:** Deep documentation of every non-trivial bug fix in this project — the *why*, not just the *what*. Each entry captures the bug, current behavior with a concrete example, root cause, the fix, after-fix behavior, and the concepts involved.
>
> **Audience:** Future-me reviewing the project, interviewers asking *"tell me about a bug you fixed,"* and anyone reading the codebase who wonders why something is the way it is.
>
> **Companion to:** [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md) (the tracker — what's done, what's open). This doc is the *story* behind the checkboxes.

---

## Table of Contents

- [Category B: Persistence Bugs](#category-b-persistence-bugs)
  - [B1. String values with spaces break on load](#b1-string-values-with-spaces-break-on-load)
  - [B2. List items with spaces break on load](#b2-list-items-with-spaces-break-on-load)
  - [B3. Hash values with spaces break on load](#b3-hash-values-with-spaces-break-on-load)
  - [B4. Hash values with colons break on load](#b4-hash-values-with-colons-break-on-load)
  - [B5. TTLs are not persisted](#b5-ttls-are-not-persisted)
  - [B6. No corruption detection](#b6-no-corruption-detection)
  - [B7. Dump blocks all reads/writes](#b7-dump-blocks-all-readswrites)
- [Cross-cutting design: the new persistence format](#cross-cutting-design-the-new-persistence-format)
- [Already-fixed bugs (brief log)](#already-fixed-bugs-brief-log)

---

## Category B: Persistence Bugs

All seven bugs in this category live in `dump()` and `load()` in `RedisDatabase.cpp`. Six of them (B1–B6) share a single root cause family — **using whitespace and colons as delimiters in a format that has to round-trip arbitrary user data**. They are fixed together by switching to a **length-prefixed binary-safe format** with a version header and CRC32 checksum. B7 is a separate performance concern and is deferred.

---

### B1. String values with spaces break on load

**Status:** ✅ Fixed (2026-05-31)
**Severity:** ⚠️ Silent data corruption
**Location:** `RedisDatabase.cpp:375` (dump), `:430` (load)

#### Current behavior

`dump()` writes each key/value record as space-separated tokens:
```cpp
ofs << "K " << kv.first << " " << kv.second << "\n";
```

`load()` reads them back with `>>`, which **stops at the first whitespace**:
```cpp
iss >> key >> value;
```

#### Example

```
Client:        SET greeting "hello world"
Memory:        kv_store["greeting"] = "hello world"

Dump file:     K greeting hello world

After restart, load() parses:
   iss >> key    → "greeting"
   iss >> value  → "hello"      ← stops at space; "world" is lost

Client:        GET greeting
Server:        "hello"          ❌ silent corruption
```

#### Root cause

The dump format uses **whitespace as a delimiter** but the values themselves can contain whitespace. There is no way for the parser to know whether a space in the byte stream is a delimiter or part of the data.

#### Fix

Switch to a **length-prefixed format**: write the byte count of each field before the field itself. The parser reads exactly that many bytes, no scanning for delimiters.

```
K <keylen> <key> <vallen> <value>\n
```

#### After-fix behavior

```
Dump file:     K 8 greeting 11 hello world

After restart, load() parses:
   read keylen=8, then 8 bytes → "greeting"
   read vallen=11, then 11 bytes → "hello world"

Client:        GET greeting
Server:        "hello world"    ✅
```

#### Concepts

- **Delimiter-based vs length-prefixed formats** (TLV — type/length/value). Length-prefixed formats are "binary safe": data can contain any bytes, including the delimiter characters, without ambiguity.
- This is the same reason RESP itself uses `$N\r\n` length prefixes for bulk strings — the Redis protocol designers solved this problem at the wire level; we need to do the same at the persistence level.

---

### B2. List items with spaces break on load

**Status:** ✅ Fixed (2026-05-31)
**Severity:** ⚠️ Silent data corruption
**Location:** `RedisDatabase.cpp:377-382` (dump), `:432-439` (load)
**Same root cause as B1.**

#### Current behavior

```cpp
// dump
ofs << "L " << kv.first;
for (const auto& item : kv.second)
    ofs << " " << item;
ofs << "\n";

// load
while (iss >> item) list.push_back(item);
```

#### Example

```
Client:        RPUSH messages "good morning" "see you"
Memory:        list_store["messages"] = ["good morning", "see you"]

Dump file:     L messages good morning see you

After restart, load() parses each whitespace-separated token:
   "good", "morning", "see", "you"

Client:        LGET messages
Server:        ["good", "morning", "see", "you"]   ❌
               4 items instead of 2; original structure destroyed
```

#### Fix

Length-prefixed list record. Element count up front, then each item with its own length prefix:
```
L <keylen> <key> <count> <len1> <item1> <len2> <item2> ...
```

#### After-fix behavior

```
Dump file:     L 8 messages 2 12 good morning 7 see you

After restart:
   key="messages", count=2
   item 1: read 12 bytes → "good morning"
   item 2: read 7 bytes  → "see you"

Client:        LGET messages
Server:        ["good morning", "see you"]   ✅
```

---

### B3. Hash values with spaces break on load

**Status:** ✅ Fixed (2026-05-31)
**Severity:** ⚠️ Silent data corruption (worst of the bunch)
**Location:** `RedisDatabase.cpp:383-388` (dump), `:440-453` (load)

#### Current behavior

```cpp
// dump — field/value joined by ':', pairs separated by ' '
ofs << "H " << kv.first;
for (const auto& fv : kv.second)
    ofs << " " << fv.first << ":" << fv.second;
ofs << "\n";

// load — split each whitespace token at the first ':'
while (iss >> pair) {
    auto pos = pair.find(':');
    if (pos != std::string::npos) {
        hash[pair.substr(0, pos)] = pair.substr(pos+1);
    }
    // else: silently skipped!
}
```

Note the silent-skip on tokens with no colon — this masks the bug as "missing entries" instead of "parse error."

#### Example

```
Client:        HSET user name "Alice Smith" city "New York"
Memory:        hash_store["user"] = {"name": "Alice Smith", "city": "New York"}

Dump file:     H user name:Alice Smith city:New York

After restart, load() tokenizes by whitespace:
   "name:Alice" → field="name", value="Alice"   ← lost "Smith"
   "Smith"      → no colon, SILENTLY SKIPPED
   "city:New"   → field="city", value="New"     ← lost "York"
   "York"       → no colon, SILENTLY SKIPPED

Client:        HGETALL user
Server:        {name: "Alice", city: "New"}    ❌
```

#### Fix

Same length-prefixed structure as lists, but with paired field/value lengths:
```
H <keylen> <key> <count> <flen1> <field1> <vlen1> <value1> ...
```

#### After-fix behavior

```
Dump file:     H 4 user 2 4 name 11 Alice Smith 4 city 8 New York

After restart:
   key="user", count=2
   pair 1: field (4 bytes)="name", value (11 bytes)="Alice Smith"
   pair 2: field (4 bytes)="city", value (8 bytes)="New York"

Client:        HGETALL user
Server:        {name: "Alice Smith", city: "New York"}   ✅
```

---

### B4. Hash values with colons break on load

**Status:** ✅ Fixed (2026-05-31)
**Severity:** ⚠️ Silent data corruption
**Location:** `RedisDatabase.cpp:446`

#### Current behavior

```cpp
auto pos = pair.find(':');  // first colon wins
```

#### Example

The break shows up clearly when a value contains both a colon and (combined with B3) whitespace:

```
Client:        HSET sensor time "12:34:56"
Memory:        hash_store["sensor"] = {"time": "12:34:56"}

Dump file:     H sensor time:12:34:56

After restart, load() splits at FIRST colon:
   pair = "time:12:34:56"
   field = "time", value = "12:34:56"   ← this one happens to work

But consider:
Client:        HSET site url "http://example.com/api:v1"
Dump file:     H site url:http://example.com/api:v1

   pair = "url:http://example.com/api:v1"
   first ':' is between "url" and "http" — happens to be correct here

The REAL break: any field that itself contains a colon:
Client:        HSET ns "user:42" "Alice"   (Redis-style namespaced field)
Dump file:     H ns user:42:Alice

After restart:
   pair = "user:42:Alice"
   field = "user", value = "42:Alice"   ❌ wrong field, wrong value
```

#### Root cause

Using **any character** as a delimiter inside user data is fragile. Colons appear in URLs, IPs, timestamps, namespaced keys — all common Redis use cases.

#### Fix

Length-prefixes eliminate the need for any in-band delimiter. The fix for B3 covers this by construction — we never search for `:` again.

#### After-fix behavior

```
Dump file:     H 2 ns 1 7 user:42 5 Alice

After restart:
   key="ns", count=1
   field (7 bytes)="user:42", value (5 bytes)="Alice"

Client:        HGET ns user:42
Server:        "Alice"   ✅
```

---

### B5. TTLs are not persisted

**Status:** ✅ Fixed (2026-05-31)
**Severity:** Correctness — expirations silently lost across restart
**Location:** `RedisDatabase.cpp:369-390` (dump never touches `expiry_map`)

#### Current behavior

`dump()` walks `kv_store`, `list_store`, `hash_store` — and ignores `expiry_map` entirely.

#### Example

```
Client:        SET session abc123
Client:        EXPIRE session 3600          (expire in 1 hour)
Memory:        kv_store["session"] = "abc123"
               expiry_map["session"] = now + 3600s

5 minutes later, dump() runs:
Dump file:     K 7 session 6 abc123          (no expiry recorded)

Server restarts. Load() reads:
   kv_store["session"] = "abc123"
   expiry_map["session"] = (nothing)

Client:  GET session   (24 hours later)
Server:  "abc123"                            ❌ should have expired
```

#### Fix

Add a new record type `E` for expirations, written after the data record. Store the expiry as an **absolute Unix timestamp (milliseconds)**, not a relative duration.

```
E <keylen> <key> <expiry_unix_ms>
```

#### After-fix behavior

```
Dump file:     K 7 session 6 abc123
               E 7 session 1748764800000

After restart:
   kv_store["session"] = "abc123"
   expiry_map["session"] = 1748764800000

Client:  GET session   (24 hours later)
Server:  (nil)                              ✅ correctly expired
```

#### Concepts

- **Absolute vs relative timestamps.** If we stored `3600 seconds from now`, a key set to expire in 10 seconds would survive forever if the server restarts 9 seconds in — the "now" anchor shifts. Storing the absolute target time makes TTLs survive restarts correctly.
- **`steady_clock` vs `system_clock`.** Our in-memory `expiry_map` uses `steady_clock` (monotonic — won't jump if NTP adjusts the wall clock). For persistence we have to convert to `system_clock` / Unix epoch because `steady_clock`'s epoch is process-local and meaningless across restarts. This conversion happens only at dump/load — runtime expiry checks still use `steady_clock`.

---

### B6. No corruption detection

**Status:** ✅ Fixed (2026-05-31)
**Severity:** Robustness — corrupted files silently produce wrong data
**Location:** `RedisDatabase.cpp:414-457` (no integrity checks anywhere)

#### Current behavior

The dump file has no magic header, no version field, no checksum. Any byte-level damage — disk error, partial write during a crash, manual edit, an older binary writing an incompatible format — produces silent corruption on load.

#### Example

```
Healthy dump file (with B1–B5 fixes):
   K 4 user 5 Alice

Disk flips one byte during write (vallen 5 → 9):
   K 4 user 9 Alice

On load:
   read keylen=4, "user"
   read vallen=9, then 9 bytes:  "Alice\nE 7"   ← reads past Alice
                                                  into the next record!

Result: cascading garbage for all subsequent records.
        Server starts up "successfully" and serves wrong data.
```

#### Fix

Two additions:

1. **Versioned magic header** — first line of the file identifies the format:
   ```
   REDIS_DUMP_V1
   ```
   On load, reject any file that doesn't start with this exact bytes. This also gives us forward/backward compatibility: future format changes bump to `V2` and old binaries fail loudly instead of corrupting.

2. **CRC32 checksum over the body**:
   ```
   REDIS_DUMP_V1
   <crc32_hex>
   <body>
   ```
   `dump()` computes CRC32 over the body and writes it after the header. `load()` recomputes CRC32 from the file and compares. Mismatch → refuse to load, return false (caller starts with an empty DB).

#### After-fix behavior

```
Dump file:
   REDIS_DUMP_V1
   a4f3c8b1
   K 4 user 5 Alice
   E 4 user 1748764800000
   L 6 colors 3 3 red 5 green 4 blue

On load:
   Header mismatch → "unsupported dump format" error, refuse to load.
   CRC mismatch    → "dump file corrupted" error, refuse to load.
   All good        → populate stores.

Result: corruption fails loudly instead of silently.   ✅
```

#### Concepts

- **CRC32 vs cryptographic hashes.** CRC32 is fast (a handful of cycles per byte with a precomputed table) and catches ~99.9999% of accidental corruption (bit flips, truncation, partial writes). It is **not** secure against intentional tampering — that would need SHA-256 or HMAC. For RDB-style snapshots, CRC is the right tradeoff: cheap, catches the realistic failure modes (storage media, crashes), no need for the cryptographic guarantees.
- **Fail-loud vs fail-quiet.** A core principle: when integrity checks fail, refuse to operate rather than silently produce wrong results. Starting with an empty database is recoverable; serving corrupt data is not.
- **Versioning.** A single version byte/string in the header is the cheapest insurance against future-self pain. The day we change the format, old files will fail with a clear error instead of being misinterpreted.

---

### B7. Dump blocks all reads/writes

**Status:** ⏸️ **Deferred** (to after Phase 3 epoll)
**Severity:** Performance / availability (not correctness)
**Location:** `RedisDatabase.cpp:370` — `db_mutex` held for the entire dump

#### Current behavior

```cpp
bool RedisDatabase::dump(...) {
    std::lock_guard<std::mutex> lock(db_mutex);   // held until function returns
    /* iterate every key, write to file */
}
```

#### Example

```
Database:      ~1M keys, ~100 MB serialized
Disk write:    ~1s

Timeline:
   T=0.000s   dump() acquires db_mutex
   T=0.000s   Client A: SET foo bar     → blocked
   T=0.200s   Client B: GET something   → blocked
   T=0.500s   Client C: LPUSH list x    → blocked
   T=1.000s   dump() releases mutex; everyone unblocks at once

Result: server appears dead for ~1s every 5 minutes.    ❌ for availability
        Data is correct, but tail latency is awful.
```

#### Fix (real Redis approach)

`fork()` + copy-on-write:
1. Briefly hold `db_mutex` (microseconds, just to make the snapshot atomic).
2. `fork()` — child inherits a snapshot of memory via copy-on-write.
3. Release `db_mutex` immediately — parent goes back to serving clients.
4. Child writes the snapshot to disk at its own pace, then exits.

Linux COW means the snapshot uses almost no extra RAM — only pages the parent *modifies during the dump* get copied. For a read-heavy workload, near-zero overhead.

#### Why deferred

- It's a performance fix, not correctness — the bugs above are higher priority.
- `fork()` semantics interact heavily with threading. Doing this in the thread-per-client model is awkward (fork() in a multithreaded program is fragile — only the calling thread survives in the child, locks held by other threads are frozen). Cleaner to do it after Phase 3 (epoll) when the I/O model is single-threaded by design.
- Real Redis does this *because* it's single-threaded — fork() is clean. We should match that ordering.

---

## Cross-cutting design: the new persistence format

All of B1–B6 are fixed by one format change. Documenting it here so it lives in one place instead of scattered across each entry.

```
┌─────────────────────────────────────────────────────────────────┐
│  REDIS_DUMP_V1\n                       ← magic + version        │
│  <crc32-of-body-as-8-hex-chars>\n      ← integrity              │
│  ─── body ───                                                   │
│  K <keylen> <key> <vallen> <value>\n   ← string record          │
│  L <keylen> <key> <count> <len> <item> <len> <item> ...\n       │
│  H <keylen> <key> <count> <flen> <field> <vlen> <value> ...\n   │
│  E <keylen> <key> <expiry_unix_ms>\n   ← expiry for prior key   │
└─────────────────────────────────────────────────────────────────┘
```

Design choices and why:

- **Text-ish, not pure binary.** Lengths and tags are ASCII; values are raw bytes after their length prefix. This makes the format human-eyeballable for debugging while still being binary-safe for the payload. Real Redis uses pure binary (RDB); we trade a bit of efficiency for readability — appropriate for a learning project.
- **One record per line is illusory** — once values contain `\n`, "lines" aren't real boundaries. The parser must use length prefixes to know where each record actually ends; the `\n` is just cosmetic.
- **`E` records come after their data record.** Load order: read the key/list/hash, then read its expiry if present. Keeps related data adjacent and the parser stateless.
- **CRC32 over the body only, not the header.** Header is small and self-describing; corruption there manifests as a magic-check failure. CRC protects the bulk of the file where corruption is more likely and harder to detect by inspection.

---

## Already-fixed bugs (brief log)

Captured here for completeness. These were fixed before this doc was started, so we only have the summary; deeper writeups can be backfilled if needed for interview prep.

### Category A (Logic)
- **`del()` returned `false` instead of `erased`** — `RedisDatabase.cpp:73`. Clients always saw `:0` even when keys were deleted.
- **`del()` didn't clear `expiry_map`** — `:72`. Re-adding a deleted key inherited the old TTL → silent expiration.
- **`flushAll()` didn't clear `expiry_map`** — `:21`. Same TTL-inheritance bug after FLUSHALL.
- **`HSET` only stored the first field/value pair** — `RedisCommandHandler.cpp:248-263`. Silent data loss; real Redis HSET takes `key f1 v1 f2 v2 ...`. Fix loops through pairs and returns the count of *new* fields (per spec).

### Category C (Validation)
- **`std::stoi` in RESP parser could crash on malformed input** — `RedisCommandHandler.cpp:30-61`. Wrapped parser body in try/catch.
- **`std::stoi` in handlers** — verified all four (`handleExpire`, `handleLrem`, `handleLindex`, `handleLset`) were already wrapped.
- **`keys()` returned duplicates when a key existed in multiple stores** — `RedisDatabase.cpp:42-51`. Switched to `unordered_set` to dedupe.

### Category D (Error messages)
- **`handleHget` said "HSET" in its error** — `:267`. Cosmetic but confusing.
- **`handleLset` said "LEST" instead of "LSET"** — `:235`. Typo.

### Category E (Resource / security)
- **No `SO_RCVTIMEO` → slow-loris vulnerability** — `RedisServer.cpp:86-89`. Idle clients held threads forever. Set 300s receive timeout.
- **No `SO_KEEPALIVE` → dead clients leaked threads** — `:92-93`. OS-level probes now detect silently-dead clients.
- **`purgeExpired()` was public** — `RedisDatabase.h:60`. Moved to private; callers must hold the mutex.
- **`purgeExpired()` not called by list/hash ops** — added to all 9 list ops + 9 hash ops, so TTLs apply uniformly.
- **Dead code in `RedisServer::run()` cleanup** — `:108-111`. Removed unreachable block (root cause — `exit()` in signal handler — deferred to Group B lifecycle cluster).

### Category F (Code smells)
- **Verbose `std::reverse_iterator<...>(fwd)` in `lrem`** — `:228`. Replaced with `std::make_reverse_iterator(fwd)`.
