// Unit tests for RedisDatabase (Phase 2, Step 1 — scaffolding smoke test).
//
// RedisDatabase is a singleton (one shared instance via getInstance()), so each
// test must reset shared state first. We call flushAll() at the top of every
// test for now; Step 2 will move this into a test fixture's SetUp().
//
// Build & run:  make gtest   (once, fetches Google Test)
//               make test    (compiles + runs)

#include <gtest/gtest.h>
#include "../include/RedisDatabase.h"

// Smoke test: its only job is to prove the scaffold works end to end —
// Google Test is linked, the Makefile compiles test code against the real
// RedisDatabase, and assertions run. If this is green, the foundation holds.
TEST(SmokeTest, SetThenGetReturnsValue) {
    RedisDatabase& db = RedisDatabase::getInstance();
    db.flushAll();                      // singleton: reset shared state first

    db.set("foo", "bar");

    std::string value;
    ASSERT_TRUE(db.get("foo", value));  // key should exist after set()
    EXPECT_EQ(value, "bar");            // and hold the value we stored
}
