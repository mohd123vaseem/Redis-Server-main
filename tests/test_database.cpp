// Unit tests for RedisDatabase (Phase 2, Step 2).
//
// RedisDatabase is a singleton (one shared instance via getInstance()), so the
// RedisDatabaseTest fixture resets shared state in SetUp() before every test.
//
// Scope: deterministic, in-process tests of the data layer — no server, no
// sockets. Timing-based TTL *expiry* (key actually disappears after N seconds)
// is covered end-to-end by test_all.sh; here we instead assert the TTL-map
// *bookkeeping* deterministically by inspecting the dump output (see the del /
// flushAll tests), which keeps this suite fast (no sleeps).
//
// Build & run:  make gtest   (once, fetches Google Test)
//               make test    (compiles + runs)

#include <gtest/gtest.h>
#include "../include/RedisDatabase.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Resets the singleton before each test so tests don't contaminate each other.
class RedisDatabaseTest : public ::testing::Test {
protected:
    RedisDatabase& db = RedisDatabase::getInstance();
    void SetUp() override { db.flushAll(); }
};

// Reads an entire file into a string (used to assert on dump() output).
std::string slurp(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// Writes raw content to a file (used to craft corrupt dumps).
void writeFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(content.data(), content.size());
}

} // namespace

// ===========================================================================
// Key / Value
// ===========================================================================
TEST_F(RedisDatabaseTest, SetThenGetReturnsValue) {
    db.set("foo", "bar");
    std::string value;
    ASSERT_TRUE(db.get("foo", value));
    EXPECT_EQ(value, "bar");
}

TEST_F(RedisDatabaseTest, GetMissingKeyReturnsFalse) {
    std::string value = "untouched";
    EXPECT_FALSE(db.get("nope", value));
}

TEST_F(RedisDatabaseTest, SetOverwritesExistingValue) {
    db.set("k", "v1");
    db.set("k", "v2");
    std::string value;
    ASSERT_TRUE(db.get("k", value));
    EXPECT_EQ(value, "v2");
}

TEST_F(RedisDatabaseTest, TypeReportsCorrectStore) {
    db.set("s", "v");
    db.rpush("l", "a");
    db.hset("h", "f", "v");
    EXPECT_EQ(db.type("s"), "string");
    EXPECT_EQ(db.type("l"), "list");
    EXPECT_EQ(db.type("h"), "hash");
    EXPECT_EQ(db.type("missing"), "none");
}

TEST_F(RedisDatabaseTest, RenameMovesValue) {
    db.set("old", "v");
    ASSERT_TRUE(db.rename("old", "new"));
    std::string value;
    EXPECT_FALSE(db.get("old", value));
    ASSERT_TRUE(db.get("new", value));
    EXPECT_EQ(value, "v");
}

TEST_F(RedisDatabaseTest, RenameMissingKeyReturnsFalse) {
    EXPECT_FALSE(db.rename("ghost", "x"));
}

// ===========================================================================
// del() — bug A1 (return value) + bug A2 (clears TTL)
// ===========================================================================
TEST_F(RedisDatabaseTest, DelReturnsTrueWhenKeyExists) {
    db.set("foo", "bar");
    EXPECT_TRUE(db.del("foo"));    // A1: must report the key was erased
}

TEST_F(RedisDatabaseTest, DelReturnsFalseWhenKeyMissing) {
    EXPECT_FALSE(db.del("foo"));   // A1: nothing to erase
}

TEST_F(RedisDatabaseTest, DelRemovesTheKey) {
    db.set("foo", "bar");
    db.del("foo");
    std::string value;
    EXPECT_FALSE(db.get("foo", value));
}

// Bug A2: del() must also drop the TTL, so a re-added key does NOT inherit the
// old expiry. Verified deterministically: after del + re-set, the dump must
// contain the new value but NO expiry (E) record.
TEST_F(RedisDatabaseTest, DelClearsTtlSoReaddedKeyHasNoExpiry) {
    db.set("k", "v");
    ASSERT_TRUE(db.expire("k", 1000));   // give k a live TTL
    ASSERT_TRUE(db.del("k"));            // must also clear the TTL
    db.set("k", "v2");                   // re-add the same key

    const std::string f = "build/test_del_ttl.rdb";
    ASSERT_TRUE(db.dump(f));
    std::string body = slurp(f);
    std::remove(f.c_str());

    EXPECT_NE(body.find("v2"), std::string::npos);   // value persisted
    EXPECT_EQ(body.find("\nE "), std::string::npos); // but no TTL record
}

// ===========================================================================
// flushAll() — bug A3 (clears TTL)
// ===========================================================================
TEST_F(RedisDatabaseTest, FlushAllEmptiesAllStores) {
    db.set("s", "v");
    db.rpush("l", "a");
    db.hset("h", "f", "v");
    db.flushAll();
    EXPECT_TRUE(db.keys().empty());
}

// Bug A3: flushAll() must clear expiry_map too — otherwise a key re-created
// with the same name silently inherits the pre-flush TTL.
TEST_F(RedisDatabaseTest, FlushAllClearsTtl) {
    db.set("k", "v");
    ASSERT_TRUE(db.expire("k", 1000));
    db.flushAll();
    db.set("k", "v2");

    const std::string f = "build/test_flush_ttl.rdb";
    ASSERT_TRUE(db.dump(f));
    std::string body = slurp(f);
    std::remove(f.c_str());

    EXPECT_EQ(body.find("\nE "), std::string::npos); // no stale TTL survived
}

// ===========================================================================
// keys() — bug C3 (dedup across stores)
// ===========================================================================
TEST_F(RedisDatabaseTest, KeysDedupesAcrossStores) {
    db.set("dup", "x");      // same name in two stores
    db.rpush("dup", "y");
    auto keys = db.keys();
    EXPECT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.front(), "dup");
}

// ===========================================================================
// expire() — existence contract (timing covered by test_all.sh)
// ===========================================================================
TEST_F(RedisDatabaseTest, ExpireReturnsTrueForExistingKey) {
    db.set("k", "v");
    EXPECT_TRUE(db.expire("k", 100));
}

TEST_F(RedisDatabaseTest, ExpireReturnsFalseForMissingKey) {
    EXPECT_FALSE(db.expire("ghost", 100));
}

// ===========================================================================
// List operations
// ===========================================================================
TEST_F(RedisDatabaseTest, RpushAppendsLpushPrepends) {
    db.rpush("l", "b");
    db.rpush("l", "c");
    db.lpush("l", "a");          // -> a, b, c
    auto list = db.lget("l");
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0], "a");
    EXPECT_EQ(list[1], "b");
    EXPECT_EQ(list[2], "c");
}

TEST_F(RedisDatabaseTest, LlenCountsItems) {
    db.rpush("l", "a");
    db.rpush("l", "b");
    EXPECT_EQ(db.llen("l"), 2);
    EXPECT_EQ(db.llen("missing"), 0);
}

TEST_F(RedisDatabaseTest, LpopFromFrontRpopFromBack) {
    db.rpush("l", "a");
    db.rpush("l", "b");
    db.rpush("l", "c");
    std::string v;
    ASSERT_TRUE(db.lpop("l", v));
    EXPECT_EQ(v, "a");
    ASSERT_TRUE(db.rpop("l", v));
    EXPECT_EQ(v, "c");
    EXPECT_EQ(db.llen("l"), 1);
}

TEST_F(RedisDatabaseTest, PopFromEmptyOrMissingReturnsFalse) {
    std::string v;
    EXPECT_FALSE(db.lpop("missing", v));
    EXPECT_FALSE(db.rpop("missing", v));
}

TEST_F(RedisDatabaseTest, LindexSupportsNegativeAndOutOfRange) {
    db.rpush("l", "a");
    db.rpush("l", "b");
    db.rpush("l", "c");
    std::string v;
    ASSERT_TRUE(db.lindex("l", 0, v));
    EXPECT_EQ(v, "a");
    ASSERT_TRUE(db.lindex("l", -1, v));   // last element
    EXPECT_EQ(v, "c");
    EXPECT_FALSE(db.lindex("l", 5, v));   // out of range
    EXPECT_FALSE(db.lindex("l", -9, v));
}

TEST_F(RedisDatabaseTest, LsetUpdatesInPlace) {
    db.rpush("l", "a");
    db.rpush("l", "b");
    ASSERT_TRUE(db.lset("l", 1, "B"));
    ASSERT_TRUE(db.lset("l", -2, "A"));   // negative index
    std::string v;
    db.lindex("l", 0, v); EXPECT_EQ(v, "A");
    db.lindex("l", 1, v); EXPECT_EQ(v, "B");
    EXPECT_FALSE(db.lset("l", 9, "x"));   // out of range
}

// lrem modes — count>0 (head), count<0 (tail), count==0 (all).
TEST_F(RedisDatabaseTest, LremPositiveCountRemovesFromHead) {
    for (const auto& s : {"a", "b", "a", "c", "a"}) db.rpush("l", s);
    EXPECT_EQ(db.lrem("l", 2, "a"), 2);   // removes first two a's
    auto list = db.lget("l");             // -> b, c, a
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0], "b");
    EXPECT_EQ(list[1], "c");
    EXPECT_EQ(list[2], "a");
}

TEST_F(RedisDatabaseTest, LremNegativeCountRemovesFromTail) {
    for (const auto& s : {"a", "b", "a", "c", "a"}) db.rpush("l", s);
    EXPECT_EQ(db.lrem("l", -2, "a"), 2);  // removes last two a's
    auto list = db.lget("l");             // -> a, b, c
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0], "a");
    EXPECT_EQ(list[1], "b");
    EXPECT_EQ(list[2], "c");
}

TEST_F(RedisDatabaseTest, LremZeroCountRemovesAll) {
    for (const auto& s : {"a", "b", "a", "c", "a"}) db.rpush("l", s);
    EXPECT_EQ(db.lrem("l", 0, "a"), 3);   // removes all a's
    auto list = db.lget("l");             // -> b, c
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0], "b");
    EXPECT_EQ(list[1], "c");
}

// ===========================================================================
// Hash operations
// ===========================================================================
TEST_F(RedisDatabaseTest, HsetThenHget) {
    db.hset("h", "f", "v");
    std::string v;
    ASSERT_TRUE(db.hget("h", "f", v));
    EXPECT_EQ(v, "v");
}

TEST_F(RedisDatabaseTest, HsetOverwritesField) {
    db.hset("h", "f", "v1");
    db.hset("h", "f", "v2");
    std::string v;
    ASSERT_TRUE(db.hget("h", "f", v));
    EXPECT_EQ(v, "v2");
}

TEST_F(RedisDatabaseTest, HgetMissingFieldReturnsFalse) {
    db.hset("h", "f", "v");
    std::string v;
    EXPECT_FALSE(db.hget("h", "other", v));
    EXPECT_FALSE(db.hget("missing", "f", v));
}

TEST_F(RedisDatabaseTest, HexistsAndHdel) {
    db.hset("h", "f", "v");
    EXPECT_TRUE(db.hexists("h", "f"));
    EXPECT_TRUE(db.hdel("h", "f"));
    EXPECT_FALSE(db.hexists("h", "f"));
    EXPECT_FALSE(db.hdel("h", "f"));   // already gone
}

TEST_F(RedisDatabaseTest, HlenKeysValsAndGetall) {
    db.hset("h", "a", "1");
    db.hset("h", "b", "2");
    EXPECT_EQ(db.hlen("h"), 2);
    EXPECT_EQ(db.hkeys("h").size(), 2u);
    EXPECT_EQ(db.hvals("h").size(), 2u);
    auto all = db.hgetall("h");
    EXPECT_EQ(all.size(), 2u);
    EXPECT_EQ(all["a"], "1");
    EXPECT_EQ(all["b"], "2");
}

TEST_F(RedisDatabaseTest, HmsetStoresAllPairs) {
    db.hmset("user", {{"name", "Alice"}, {"age", "30"}, {"city", "Pune"}});
    EXPECT_EQ(db.hlen("user"), 3);
    std::string v;
    ASSERT_TRUE(db.hget("user", "city", v));
    EXPECT_EQ(v, "Pune");
}

// ===========================================================================
// Persistence round-trip — bugs B1-B5 (binary-safe values, TTL survival)
// ===========================================================================
TEST_F(RedisDatabaseTest, DumpLoadRoundTripStringWithSpaces) {
    db.set("greeting", "hello world");   // B1: space in value
    const std::string f = "build/test_rt_string.rdb";
    ASSERT_TRUE(db.dump(f));
    db.flushAll();
    ASSERT_TRUE(db.load(f));
    std::remove(f.c_str());
    std::string v;
    ASSERT_TRUE(db.get("greeting", v));
    EXPECT_EQ(v, "hello world");
}

TEST_F(RedisDatabaseTest, DumpLoadRoundTripListWithSpaces) {
    db.rpush("messages", "good morning");  // B2: spaces in list items
    db.rpush("messages", "see you");
    const std::string f = "build/test_rt_list.rdb";
    ASSERT_TRUE(db.dump(f));
    db.flushAll();
    ASSERT_TRUE(db.load(f));
    std::remove(f.c_str());
    auto list = db.lget("messages");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0], "good morning");
    EXPECT_EQ(list[1], "see you");
}

TEST_F(RedisDatabaseTest, DumpLoadRoundTripHashWithSpacesAndColons) {
    db.hset("u", "name", "Alice Smith");   // B3: spaces
    db.hset("u", "city", "New York");
    db.hset("ns", "user:42", "Alice");     // B4: colon in field
    const std::string f = "build/test_rt_hash.rdb";
    ASSERT_TRUE(db.dump(f));
    db.flushAll();
    ASSERT_TRUE(db.load(f));
    std::remove(f.c_str());
    std::string v;
    ASSERT_TRUE(db.hget("u", "name", v));     EXPECT_EQ(v, "Alice Smith");
    ASSERT_TRUE(db.hget("u", "city", v));     EXPECT_EQ(v, "New York");
    ASSERT_TRUE(db.hget("ns", "user:42", v)); EXPECT_EQ(v, "Alice");
}

TEST_F(RedisDatabaseTest, DumpLoadPreservesTtlKey) {
    db.set("session", "abc123");
    ASSERT_TRUE(db.expire("session", 1000));   // long TTL — still live after reload
    const std::string f = "build/test_rt_ttl.rdb";
    ASSERT_TRUE(db.dump(f));
    db.flushAll();
    ASSERT_TRUE(db.load(f));
    std::remove(f.c_str());
    std::string v;
    EXPECT_TRUE(db.get("session", v));   // B5: key survived with its TTL
    EXPECT_EQ(v, "abc123");
}

// ===========================================================================
// Corruption detection — bug B6 (fail loud, don't wipe good in-memory data)
// ===========================================================================
TEST_F(RedisDatabaseTest, LoadRejectsMissingFile) {
    EXPECT_FALSE(db.load("build/does_not_exist.rdb"));
}

TEST_F(RedisDatabaseTest, LoadRejectsBadMagicHeaderAndKeepsMemory) {
    db.set("keep", "safe");
    const std::string f = "build/test_bad_magic.rdb";
    writeFile(f, "REDIS_DUMP_V2\n00000000\n");   // wrong magic version
    EXPECT_FALSE(db.load(f));
    std::remove(f.c_str());
    std::string v;
    EXPECT_TRUE(db.get("keep", v));   // in-memory data must be untouched
    EXPECT_EQ(v, "safe");
}

TEST_F(RedisDatabaseTest, LoadRejectsBadCrcAndKeepsMemory) {
    db.set("keep", "safe");
    const std::string good = "build/test_good.rdb";
    ASSERT_TRUE(db.dump(good));
    std::string contents = slurp(good);
    std::remove(good.c_str());

    // Corrupt the CRC line (line 2) while leaving the body intact.
    std::istringstream in(contents);
    std::string header, crc, rest;
    std::getline(in, header);
    std::getline(in, crc);
    std::ostringstream restss;
    restss << in.rdbuf();
    rest = restss.str();
    const std::string f = "build/test_bad_crc.rdb";
    writeFile(f, header + "\n" + "deadbeef\n" + rest);

    EXPECT_FALSE(db.load(f));
    std::remove(f.c_str());
    std::string v;
    EXPECT_TRUE(db.get("keep", v));   // good in-memory state preserved
    EXPECT_EQ(v, "safe");
}
