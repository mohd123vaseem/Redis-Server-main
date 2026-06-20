// Integration tests for RedisCommandHandler (Phase 2, Step 4).
//
// processCommand(string) -> string is a pure seam: it takes a raw client
// request (RESP-framed or inline) and returns the RESP-formatted reply. No
// socket needed, so we can test the parser + every handler directly.
//
// These cover the bug fixes that live in the command layer (not the DB):
//   - C1: malformed RESP must not crash the parser
//   - D1/D2: error messages name the right command (HGET, LSET)
//   - A4: multi-field HSET stores every pair and counts only NEW fields
//
// The handler talks to the RedisDatabase singleton, so the fixture resets it
// in SetUp(), same pattern as test_database.cpp.

#include <gtest/gtest.h>
#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

#include <string>
#include <vector>

namespace {

class CommandHandlerTest : public ::testing::Test {
protected:
    RedisCommandHandler handler;
    RedisDatabase& db = RedisDatabase::getInstance();
    void SetUp() override { db.flushAll(); }
};

// Builds a RESP array frame: *N\r\n$len\r\narg\r\n...  e.g. resp({"SET","k","v"}).
std::string resp(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args)
        out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return out;
}

} // namespace

// ===========================================================================
// Parser: framing, inline fallback, malformed input (bug C1)
// ===========================================================================
TEST_F(CommandHandlerTest, ParsesRespArrayFrame) {
    EXPECT_EQ(handler.processCommand(resp({"PING"})), "+PONG\r\n");
}

TEST_F(CommandHandlerTest, ParsesInlineCommand) {
    // Input not starting with '*' falls back to whitespace splitting.
    EXPECT_EQ(handler.processCommand("PING"), "+PONG\r\n");
    EXPECT_EQ(handler.processCommand("SET foo bar"), "+OK\r\n");
}

TEST_F(CommandHandlerTest, CommandNameIsCaseInsensitive) {
    EXPECT_EQ(handler.processCommand(resp({"ping"})), "+PONG\r\n");
    EXPECT_EQ(handler.processCommand(resp({"PiNg"})), "+PONG\r\n");
}

TEST_F(CommandHandlerTest, EmptyInputReturnsError) {
    EXPECT_EQ(handler.processCommand(""), "-Error: Empty command\r\n");
}

// Bug C1: a malformed length (`*abc`) used to throw std::invalid_argument and
// crash the worker thread. It must now be swallowed and reported, not thrown.
TEST_F(CommandHandlerTest, MalformedRespDoesNotCrash) {
    std::string reply;
    EXPECT_NO_THROW(reply = handler.processCommand("*abc\r\n$4\r\nPING\r\n"));
    EXPECT_EQ(reply, "-Error: Empty command\r\n");
}

TEST_F(CommandHandlerTest, UnknownCommandReturnsError) {
    EXPECT_EQ(handler.processCommand(resp({"NOTACMD"})), "-Error: Unknown command\r\n");
}

// ===========================================================================
// RESP reply formatting
// ===========================================================================
TEST_F(CommandHandlerTest, EchoReturnsMessage) {
    EXPECT_EQ(handler.processCommand(resp({"ECHO", "hello"})), "+hello\r\n");
}

TEST_F(CommandHandlerTest, GetReturnsBulkStringOrNil) {
    handler.processCommand(resp({"SET", "foo", "bar"}));
    EXPECT_EQ(handler.processCommand(resp({"GET", "foo"})), "$3\r\nbar\r\n");
    EXPECT_EQ(handler.processCommand(resp({"GET", "missing"})), "$-1\r\n");  // nil
}

TEST_F(CommandHandlerTest, DelReturnsIntegerReply) {
    handler.processCommand(resp({"SET", "foo", "bar"}));
    EXPECT_EQ(handler.processCommand(resp({"DEL", "foo"})), ":1\r\n");   // erased
    EXPECT_EQ(handler.processCommand(resp({"DEL", "foo"})), ":0\r\n");   // already gone
}

TEST_F(CommandHandlerTest, KeysReturnsArray) {
    handler.processCommand(resp({"SET", "foo", "bar"}));
    EXPECT_EQ(handler.processCommand(resp({"KEYS", "*"})), "*1\r\n$3\r\nfoo\r\n");
}

TEST_F(CommandHandlerTest, RpushReturnsNewLength) {
    EXPECT_EQ(handler.processCommand(resp({"RPUSH", "l", "a", "b", "c"})), ":3\r\n");
}

// ===========================================================================
// Arity / error messages — bugs D1 (HGET) and D2 (LSET)
// ===========================================================================
TEST_F(CommandHandlerTest, SetRequiresKeyAndValue) {
    EXPECT_EQ(handler.processCommand(resp({"SET", "onlykey"})),
              "-Error: SET requires key and value\r\n");
}

// Bug D1: the error message used to say "HSET" inside HGET. It must name HGET.
TEST_F(CommandHandlerTest, HgetErrorNamesHget) {
    std::string reply = handler.processCommand(resp({"HGET", "key"}));
    EXPECT_EQ(reply, "-Error: HGET requires key and field\r\n");
    EXPECT_NE(reply.find("HGET"), std::string::npos);
    EXPECT_EQ(reply.find("HSET"), std::string::npos);   // must NOT mention HSET
}

// Bug D2: the error message used to say "LEST". It must name LSET.
TEST_F(CommandHandlerTest, LsetErrorNamesLset) {
    std::string reply = handler.processCommand(resp({"LSET", "key", "0"}));
    EXPECT_EQ(reply, "-Error: LSET requires key, index and value\r\n");
    EXPECT_NE(reply.find("LSET"), std::string::npos);
    EXPECT_EQ(reply.find("LEST"), std::string::npos);   // the old typo
}

TEST_F(CommandHandlerTest, ExpireRejectsNonNumericTime) {
    handler.processCommand(resp({"SET", "k", "v"}));
    EXPECT_EQ(handler.processCommand(resp({"EXPIRE", "k", "abc"})),
              "-Error: Invalid expiration time\r\n");
}

TEST_F(CommandHandlerTest, LindexRejectsNonNumericIndex) {
    handler.processCommand(resp({"RPUSH", "l", "a"}));
    EXPECT_EQ(handler.processCommand(resp({"LINDEX", "l", "xyz"})),
              "-Error: Invalid index\r\n");
}

// ===========================================================================
// Multi-field HSET — bug A4 (silent data loss: only first pair was stored)
// ===========================================================================
TEST_F(CommandHandlerTest, HsetStoresEveryFieldAndCountsNewOnes) {
    // All three fields are new -> reply :3
    EXPECT_EQ(
        handler.processCommand(resp({"HSET", "user", "name", "Alice", "age", "30", "city", "Pune"})),
        ":3\r\n");
    // Bug A4 was: only the first pair stored. Confirm the LAST pair survived.
    EXPECT_EQ(handler.processCommand(resp({"HGET", "user", "city"})), "$4\r\nPune\r\n");
}

TEST_F(CommandHandlerTest, HsetCountsOnlyNewFieldsOnUpdate) {
    handler.processCommand(resp({"HSET", "user", "name", "Alice"}));
    // name already exists (update), email is new -> only 1 new field
    EXPECT_EQ(
        handler.processCommand(resp({"HSET", "user", "name", "Bob", "email", "b@x.com"})),
        ":1\r\n");
}

TEST_F(CommandHandlerTest, HsetRejectsOddFieldValueArgs) {
    // Trailing field with no value -> rejected before any partial write.
    EXPECT_EQ(handler.processCommand(resp({"HSET", "user", "name", "Alice", "age"})),
              "-Error: HSET requires key followed by one or more field value pairs\r\n");
}

// ===========================================================================
// Remaining handlers — drive each command through processCommand so the
// command layer (not just the DB) is exercised. (Step 6: raise coverage.)
// ===========================================================================
TEST_F(CommandHandlerTest, FlushAllClearsEverything) {
    handler.processCommand(resp({"SET", "foo", "bar"}));
    EXPECT_EQ(handler.processCommand(resp({"FLUSHALL"})), "+OK\r\n");
    EXPECT_EQ(handler.processCommand(resp({"KEYS", "*"})), "*0\r\n");
}

TEST_F(CommandHandlerTest, TypeReportsEachStore) {
    handler.processCommand(resp({"SET", "s", "v"}));
    handler.processCommand(resp({"RPUSH", "l", "a"}));
    handler.processCommand(resp({"HSET", "h", "f", "v"}));
    EXPECT_EQ(handler.processCommand(resp({"TYPE", "s"})), "+string\r\n");
    EXPECT_EQ(handler.processCommand(resp({"TYPE", "l"})), "+list\r\n");
    EXPECT_EQ(handler.processCommand(resp({"TYPE", "h"})), "+hash\r\n");
    EXPECT_EQ(handler.processCommand(resp({"TYPE", "missing"})), "+none\r\n");
}

TEST_F(CommandHandlerTest, RenameSucceedsAndFails) {
    handler.processCommand(resp({"SET", "a", "1"}));
    EXPECT_EQ(handler.processCommand(resp({"RENAME", "a", "b"})), "+OK\r\n");
    EXPECT_EQ(handler.processCommand(resp({"GET", "b"})), "$1\r\n1\r\n");
    EXPECT_EQ(handler.processCommand(resp({"RENAME", "ghost", "x"})),
              "-Error: Key not found or rename failed\r\n");
}

TEST_F(CommandHandlerTest, ExpireSuccessAndMissingKey) {
    handler.processCommand(resp({"SET", "k", "v"}));
    EXPECT_EQ(handler.processCommand(resp({"EXPIRE", "k", "100"})), "+OK\r\n");
    EXPECT_EQ(handler.processCommand(resp({"EXPIRE", "ghost", "100"})),
              "-Error: Key not found\r\n");
}

// ---- List handlers ----
TEST_F(CommandHandlerTest, LpushReturnsLengthAndLgetReturnsItems) {
    EXPECT_EQ(handler.processCommand(resp({"LPUSH", "l", "a", "b"})), ":2\r\n");
    // lpush a then b -> [b, a]
    EXPECT_EQ(handler.processCommand(resp({"LGET", "l"})),
              "*2\r\n$1\r\nb\r\n$1\r\na\r\n");
}

TEST_F(CommandHandlerTest, LlenReturnsCount) {
    handler.processCommand(resp({"RPUSH", "l", "a", "b", "c"}));
    EXPECT_EQ(handler.processCommand(resp({"LLEN", "l"})), ":3\r\n");
}

TEST_F(CommandHandlerTest, LpopAndRpopAndNil) {
    handler.processCommand(resp({"RPUSH", "l", "a", "b", "c"}));
    EXPECT_EQ(handler.processCommand(resp({"LPOP", "l"})), "$1\r\na\r\n");
    EXPECT_EQ(handler.processCommand(resp({"RPOP", "l"})), "$1\r\nc\r\n");
    handler.processCommand(resp({"FLUSHALL"}));
    EXPECT_EQ(handler.processCommand(resp({"LPOP", "missing"})), "$-1\r\n");
}

TEST_F(CommandHandlerTest, LremCountAndInvalidCount) {
    handler.processCommand(resp({"RPUSH", "l", "a", "b", "a"}));
    EXPECT_EQ(handler.processCommand(resp({"LREM", "l", "0", "a"})), ":2\r\n");
    EXPECT_EQ(handler.processCommand(resp({"LREM", "l", "xx", "a"})),
              "-Error: Invalid count\r\n");
}

TEST_F(CommandHandlerTest, LindexAndLsetSuccessAndOutOfRange) {
    handler.processCommand(resp({"RPUSH", "l", "a", "b"}));
    EXPECT_EQ(handler.processCommand(resp({"LINDEX", "l", "0"})), "$1\r\na\r\n");
    EXPECT_EQ(handler.processCommand(resp({"LINDEX", "l", "9"})), "$-1\r\n");      // out of range
    EXPECT_EQ(handler.processCommand(resp({"LSET", "l", "0", "X"})), "+OK\r\n");
    EXPECT_EQ(handler.processCommand(resp({"LSET", "l", "9", "X"})),
              "-Error: Index out of range\r\n");
}

// ---- Hash handlers ----
TEST_F(CommandHandlerTest, HexistsAndHdelReplies) {
    handler.processCommand(resp({"HSET", "h", "f", "v"}));
    EXPECT_EQ(handler.processCommand(resp({"HEXISTS", "h", "f"})), ":1\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HEXISTS", "h", "x"})), ":0\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HDEL", "h", "f"})), ":1\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HDEL", "h", "f"})), ":0\r\n");   // already gone
}

TEST_F(CommandHandlerTest, HgetallHkeysHvalsHlen) {
    handler.processCommand(resp({"HSET", "h", "field", "value"}));   // single field -> deterministic
    EXPECT_EQ(handler.processCommand(resp({"HGETALL", "h"})),
              "*2\r\n$5\r\nfield\r\n$5\r\nvalue\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HKEYS", "h"})), "*1\r\n$5\r\nfield\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HVALS", "h"})), "*1\r\n$5\r\nvalue\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HLEN", "h"})), ":1\r\n");
}

TEST_F(CommandHandlerTest, HmsetStoresPairsAndRejectsBadArity) {
    EXPECT_EQ(handler.processCommand(resp({"HMSET", "h", "a", "1", "b", "2"})), "+OK\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HGET", "h", "b"})), "$1\r\n2\r\n");
    EXPECT_EQ(handler.processCommand(resp({"HMSET", "h", "a"})),
              "-Error: HMSET requires key followed by field value pairs\r\n");
}

TEST_F(CommandHandlerTest, GetAndEchoArityErrors) {
    EXPECT_EQ(handler.processCommand(resp({"GET"})), "-Error: GET requires key\r\n");
    EXPECT_EQ(handler.processCommand(resp({"ECHO"})), "-Error: ECHO requires a message\r\n");
}
