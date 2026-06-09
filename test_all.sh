#!/usr/bin/env bash
#
# Bug-fix regression harness.
#
# Runs one assertion per documented fix in BUG_FIXES.md (TC-* ids match that
# doc) and prints PASS/FAIL with a summary; exits non-zero if anything fails.
#
# It manages its OWN server instance:
#   - on port 6390 (override with PORT=...), so it won't touch a real Redis
#   - it backs up your dump.my_rdb and restores it on exit
#   - it starts/stops the server itself so it can do the restart cycles the
#     persistence bugs (B1-B6) require
#
# Prereqs: the binary must be built (`make`) and `redis-cli` must be installed.
# Usage:   ./test_all.sh
#
# NOTE: contains a few `sleep`s for the TTL tests, so a full run takes ~30-40s.

cd "$(dirname "$0")" || exit 1

PORT="${PORT:-6390}"
BIN=./my_redis_server
LOG="$(mktemp)"
GOOD_RDB="$(mktemp)"
SRV_PID=""
SHUTDOWN_SECS=0
SHUTDOWN_HUNG=0
PASS=0
FAIL=0

# ----- prereq checks ---------------------------------------------------------
command -v redis-cli >/dev/null 2>&1 || { echo "ERROR: redis-cli not found"; exit 1; }
[[ -x "$BIN" ]] || { echo "ERROR: $BIN not built — run 'make' first"; exit 1; }

# ----- cleanup: restore the user's dump, kill any stray server ---------------
cleanup() {
    stop_server
    rm -f dump.my_rdb
    [[ -f dump.my_rdb.testbak ]] && mv dump.my_rdb.testbak dump.my_rdb
    rm -f "$LOG" "$GOOD_RDB"
}
trap cleanup EXIT
[[ -f dump.my_rdb ]] && mv dump.my_rdb dump.my_rdb.testbak
rm -f dump.my_rdb

# ----- server lifecycle ------------------------------------------------------
start_server() {
    "$BIN" "$PORT" >"$LOG" 2>&1 &
    SRV_PID=$!
    local i
    for i in $(seq 1 50); do
        [[ "$(redis-cli -p "$PORT" PING 2>/dev/null)" == "PONG" ]] && return 0
        sleep 0.1
    done
    echo "ERROR: server failed to start on port $PORT"; cat "$LOG"; exit 1
}

# Stops the server via SIGINT (which triggers its clean dump). Bounded to ~10s
# so a shutdown regression can't hang the whole suite. Records elapsed seconds
# and whether it had to be force-killed.
stop_server() {
    [[ -n "$SRV_PID" ]] || { SHUTDOWN_SECS=0; SHUTDOWN_HUNG=0; return 0; }
    local t0 t1 i
    t0=$(date +%s)
    kill -INT "$SRV_PID" 2>/dev/null
    for i in $(seq 1 100); do
        kill -0 "$SRV_PID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$SRV_PID" 2>/dev/null; then
        kill -9 "$SRV_PID" 2>/dev/null
        SHUTDOWN_HUNG=1
    else
        SHUTDOWN_HUNG=0
    fi
    wait "$SRV_PID" 2>/dev/null
    t1=$(date +%s)
    SHUTDOWN_SECS=$((t1 - t0))
    SRV_PID=""
}

restart_server() { stop_server; start_server; }

# ----- assertion helpers -----------------------------------------------------
r() { redis-cli -p "$PORT" "$@" 2>&1; }   # send a command, capture reply (+errors)

pass() { printf '  %-36s PASS\n' "$1"; PASS=$((PASS+1)); }
fail() { printf '  %-36s FAIL — %s\n' "$1" "$2"; FAIL=$((FAIL+1)); }

check()          { [[ "$2" == "$3" ]]  && pass "$1" || fail "$1" "got [$2] want [$3]"; }
check_contains() { [[ "$2" == *"$3"* ]] && pass "$1" || fail "$1" "[$2] missing [$3]"; }

# send raw bytes on a fresh connection (for malformed-protocol tests), no read
send_raw() {
    exec 9<>"/dev/tcp/127.0.0.1/$PORT" || return 1
    printf '%b' "$1" >&9
    exec 9>&- 9<&-
}

echo "=== Redis bug regression (port $PORT) ==="

# =============================================================================
# Single-session tests (server stays up)
# =============================================================================
start_server
r FLUSHALL >/dev/null

echo "-- Category A: logic --"
r SET foo bar >/dev/null
check          "TC-A1 DEL returns 1"        "$(r DEL foo)" "1"
check          "TC-A1 DEL again returns 0"  "$(r DEL foo)" "0"

r SET k v >/dev/null; r EXPIRE k 3 >/dev/null; r DEL k >/dev/null; r SET k v2 >/dev/null
sleep 4
check          "TC-A2 DEL clears TTL"        "$(r GET k)" "v2"

r SET k v >/dev/null; r EXPIRE k 3 >/dev/null; r FLUSHALL >/dev/null; r SET k v2 >/dev/null
sleep 4
check          "TC-A3 FLUSHALL clears TTL"   "$(r GET k)" "v2"

r DEL user >/dev/null
check          "TC-A4 HSET multi-pair count" "$(r HSET user name Alice age 30 city Pune)" "3"
check          "TC-A4 HSET stored 'city'"    "$(r HGET user city)" "Pune"

echo "-- Category C: validation --"
send_raw '*abc\r\n$4\r\nPING\r\n'
check          "TC-C1 server alive after bad RESP" "$(r PING)" "PONG"

r SET k v >/dev/null
check_contains "TC-C2 EXPIRE bad number"     "$(r EXPIRE k abc)" "Invalid expiration time"
check_contains "TC-C2 LINDEX bad index"      "$(r LINDEX k xyz)" "Invalid index"
check          "TC-C2 server alive after"    "$(r PING)" "PONG"

r FLUSHALL >/dev/null; r SET dup bar >/dev/null; r LPUSH dup x >/dev/null
check          "TC-C3 KEYS dedup (1 line)"   "$(r KEYS '*' | grep -c .)" "1"

echo "-- Category D: error messages --"
check_contains "TC-D1 HGET names HGET"       "$(r HGET onlykey)" "HGET requires key and field"
check_contains "TC-D2 LSET names LSET"       "$(r LSET klist 0)" "LSET requires key, index and value"

echo "-- Category E: list/hash expiry --"
r DEL mylist myhash >/dev/null
r RPUSH mylist a b c >/dev/null; r HSET myhash f v >/dev/null
r EXPIRE mylist 3 >/dev/null;    r EXPIRE myhash 3 >/dev/null
sleep 4
check          "TC-E4 list expired (LLEN 0)" "$(r LLEN mylist)" "0"
check          "TC-E4 hash expired (HLEN 0)" "$(r HLEN myhash)" "0"

echo "-- Category F: code smell (behaviour) --"
r DEL l >/dev/null; r RPUSH l a b a c a >/dev/null
check          "TC-F1 LREM -2 removes 2"     "$(r LREM l -2 a)" "2"
check          "TC-F1 LREM result idx0"      "$(r LINDEX l 0)" "a"
check          "TC-F1 LREM result idx1"      "$(r LINDEX l 1)" "b"
check          "TC-F1 LREM result idx2"      "$(r LINDEX l 2)" "c"

stop_server

# =============================================================================
# Category B: persistence (write -> RESTART -> read)
# =============================================================================
echo "-- Category B: persistence (with restarts) --"
start_server
r FLUSHALL >/dev/null
r SET greeting "hello world" >/dev/null
r RPUSH messages "good morning" "see you" >/dev/null
r HSET u name "Alice Smith" city "New York" >/dev/null
r HSET ns "user:42" "Alice" >/dev/null
restart_server

check          "TC-B1 string+spaces"         "$(r GET greeting)" "hello world"
check          "TC-B2 list count"            "$(r LLEN messages)" "2"
msgs="$(r LGET messages)"
check_contains "TC-B2 list item 1"           "$msgs" "good morning"
check_contains "TC-B2 list item 2"           "$msgs" "see you"
check          "TC-B3 hash+spaces (name)"    "$(r HGET u name)" "Alice Smith"
check          "TC-B3 hash+spaces (city)"    "$(r HGET u city)" "New York"
check          "TC-B4 hash+colons"           "$(r HGET ns user:42)" "Alice"

# TC-B5: TTL persists across restart (absolute time)
r SET session abc123 >/dev/null; r EXPIRE session 10 >/dev/null
restart_server
check          "TC-B5 TTL present on reload" "$(r GET session)" "abc123"
sleep 10
check          "TC-B5 TTL still expires"     "$(r GET session)" ""

# TC-B6: corruption detection — server must refuse a bad dump and start empty
r FLUSHALL >/dev/null; r SET a 1 >/dev/null
stop_server                          # writes a clean, valid dump.my_rdb
cp dump.my_rdb "$GOOD_RDB"

cp "$GOOD_RDB" dump.my_rdb; sed -i '2s/.*/00000000/' dump.my_rdb   # break the CRC line
start_server
check          "TC-B6 bad CRC -> empty DB"   "$(r KEYS '*' | grep -c .)" "0"
stop_server

cp "$GOOD_RDB" dump.my_rdb; sed -i '1s/.*/REDIS_DUMP_V2/' dump.my_rdb  # break the magic header
start_server
check          "TC-B6 bad header -> empty DB" "$(r KEYS '*' | grep -c .)" "0"
stop_server

# =============================================================================
# Group B: lifecycle & clean shutdown
# =============================================================================
echo "-- Group B: lifecycle & shutdown --"

# TC-G1/G2: clean shutdown message + data persisted across the cycle
start_server
r FLUSHALL >/dev/null; r SET persisted yes >/dev/null
stop_server
check_contains "TC-G1 clean shutdown log"    "$(cat "$LOG")" "Server Shutdown Complete!"
start_server
check          "TC-G2 data persisted"        "$(r GET persisted)" "yes"

# TC-G3: two idle clients -> shutdown joins exactly 2
exec 7<>"/dev/tcp/127.0.0.1/$PORT"
exec 8<>"/dev/tcp/127.0.0.1/$PORT"
sleep 0.5
stop_server
exec 7>&- 7<&-; exec 8>&- 8<&-
check_contains "TC-G3 joins 2 connections"   "$(cat "$LOG")" "Joining 2 active client connection(s)"

# TC-G4: an idle client must not block shutdown (no 300s recv hang)
start_server
exec 7<>"/dev/tcp/127.0.0.1/$PORT"
sleep 0.5
stop_server
exec 7>&- 7<&-
if [[ "$SHUTDOWN_HUNG" == "0" && "$SHUTDOWN_SECS" -le 5 ]]; then
    pass "TC-G4 idle client unblocked (${SHUTDOWN_SECS}s)"
else
    fail "TC-G4 idle client unblocked" "hung=$SHUTDOWN_HUNG secs=$SHUTDOWN_SECS"
fi

echo "-- skipped (not auto-testable) --"
echo "  TC-E1 slow-loris timeout   SKIP (5-min wait; verify by inspection)"
echo "  TC-E2 SO_KEEPALIVE         SKIP (needs network failure; by inspection)"
echo "  TC-E3 purgeExpired private SKIP (compile-time; by inspection)"

# =============================================================================
echo "==========================================="
echo "  $PASS passed, $FAIL failed"
echo "==========================================="
[[ "$FAIL" -eq 0 ]]
