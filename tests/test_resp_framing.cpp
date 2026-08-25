// Unit tests for respFrameLength() — the Phase 3 (epoll) RESP framing helper.
//
// Framing answers one question for the event loop: "given the bytes I've read
// so far, how long is the first complete command?" The contract is:
//    > 0 : that many bytes form the first complete command
//   == 0 : incomplete — wait for more bytes (partial read)
//    < 0 : malformed framing — drop the client
//
// This is the riskiest new logic in Phase 3, and it's pure (no sockets), so we
// pin it down with tests BEFORE wiring it into the epoll loop.

#include <gtest/gtest.h>
#include "../include/RedisCommandHandler.h"

#include <string>
#include <vector>

namespace {
// Builds a RESP array frame: *N\r\n$len\r\narg\r\n...
std::string resp(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args)
        out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return out;
}
} // namespace

// ---- complete commands -----------------------------------------------------
TEST(RespFraming, CompleteSingleArgCommand) {
    std::string frame = resp({"PING"});
    EXPECT_EQ(respFrameLength(frame), static_cast<ssize_t>(frame.size()));
}

TEST(RespFraming, CompleteMultiArgCommand) {
    std::string frame = resp({"SET", "foo", "bar"});
    EXPECT_EQ(respFrameLength(frame), static_cast<ssize_t>(frame.size()));
}

TEST(RespFraming, EmptyArrayIsComplete) {
    EXPECT_EQ(respFrameLength("*0\r\n"), 4);   // valid empty command, 4 bytes
}

// ---- incomplete (need more bytes) -> 0 -------------------------------------
TEST(RespFraming, EmptyBufferNeedsMore) {
    EXPECT_EQ(respFrameLength(""), 0);
}

TEST(RespFraming, PartialCountLineNeedsMore) {
    EXPECT_EQ(respFrameLength("*3"), 0);        // no CRLF after count yet
    EXPECT_EQ(respFrameLength("*1\r\n"), 0);    // header done, no bulk string yet
}

TEST(RespFraming, PartialBulkHeaderNeedsMore) {
    EXPECT_EQ(respFrameLength("*1\r\n$4"), 0);  // length line not terminated
}

TEST(RespFraming, PartialBulkDataNeedsMore) {
    EXPECT_EQ(respFrameLength("*1\r\n$4\r\nPI"), 0);       // only 2 of 4 bytes
    EXPECT_EQ(respFrameLength("*1\r\n$4\r\nPING"), 0);     // data here, trailing CRLF not yet
    EXPECT_EQ(respFrameLength("*1\r\n$4\r\nPING\r"), 0);   // half the trailing CRLF
}

// ---- pipelining: only the FIRST command's length is reported ---------------
TEST(RespFraming, PipelinedReturnsFirstCommandOnly) {
    std::string first = resp({"PING"});
    std::string second = resp({"SET", "a", "b"});
    EXPECT_EQ(respFrameLength(first + second), static_cast<ssize_t>(first.size()));
}

// ---- inline commands -------------------------------------------------------
TEST(RespFraming, InlineCommandCompleteAtNewline) {
    EXPECT_EQ(respFrameLength("PING\n"), 5);
    EXPECT_EQ(respFrameLength("PING\r\n"), 6);          // includes the \r
    EXPECT_EQ(respFrameLength("SET foo bar\n"), 12);
}

TEST(RespFraming, InlineCommandWithoutNewlineNeedsMore) {
    EXPECT_EQ(respFrameLength("PING"), 0);
}

TEST(RespFraming, InlineReturnsFirstLineOnly) {
    EXPECT_EQ(respFrameLength("PING\nEXTRA"), 5);       // stop at first newline
}

// ---- malformed -> negative -------------------------------------------------
TEST(RespFraming, NonNumericCountIsMalformed) {
    EXPECT_LT(respFrameLength("*abc\r\n"), 0);
}

TEST(RespFraming, NegativeCountIsMalformed) {
    EXPECT_LT(respFrameLength("*-1\r\n"), 0);
}

TEST(RespFraming, MissingBulkDollarIsMalformed) {
    EXPECT_LT(respFrameLength("*1\r\n%4\r\nPING\r\n"), 0);  // '%' where '$' expected
}

TEST(RespFraming, NonNumericBulkLengthIsMalformed) {
    EXPECT_LT(respFrameLength("*1\r\n$xy\r\nPING\r\n"), 0);
}

TEST(RespFraming, WrongTerminatorIsMalformed) {
    // 4 bytes "PING" followed by "XY" instead of "\r\n".
    EXPECT_LT(respFrameLength("*1\r\n$4\r\nPINGXY"), 0);
}

TEST(RespFraming, OversizedBulkLengthIsMalformed) {
    EXPECT_LT(respFrameLength("*1\r\n$999999999\r\n"), 0);  // exceeds MAX_BULK_LEN
}

// ---- large payload (the deferred 1024-byte recv-buffer scenario) -----------
TEST(RespFraming, LargePayloadOver1024Bytes) {
    std::string big(2000, 'x');
    std::string frame = resp({"SET", "k", big});
    EXPECT_EQ(respFrameLength(frame), static_cast<ssize_t>(frame.size()));
    // Truncated mid-payload -> incomplete, not malformed.
    EXPECT_EQ(respFrameLength(frame.substr(0, frame.size() - 100)), 0);
}
