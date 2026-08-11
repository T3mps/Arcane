// Core wire oracle: characterization tests for the LENGTH:BODY\n framing,
// ported from the Lua threading_harness stream_buffer assertions
// (Client/src/tests/threading_harness/main.lua) plus the audit-derived
// edge cases baked into ExtractLengthFramed (M-V4-5 partial-parse prefix).
//
// Note: the Lua extractMessage returns the frame INCLUDING the length
// prefix; the C++ ExtractLengthFramed returns the body only. The oracle
// ports semantically (need_more / error / drain / glue / partial-completion
// behavior), not byte-for-byte.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Net/Protocol.hpp>

using Arcane::ExtractLengthFramed;
using Arcane::Message;

TEST_CASE("framing: empty buffer needs more data", "[wire]")
{
    auto r = ExtractLengthFramed("");
    REQUIRE(r.needMoreData);
    REQUIRE_FALSE(r.error);
}

TEST_CASE("framing: short prefix without colon needs more data", "[wire]")
{
    auto r = ExtractLengthFramed("12");
    REQUIRE(r.needMoreData);
    REQUIRE_FALSE(r.error);
}

TEST_CASE("framing: long garbage without colon is an error", "[wire]")
{
    auto r = ExtractLengthFramed("12345678901"); // > 10 bytes, no colon
    REQUIRE(r.error);
}

TEST_CASE("framing: one complete frame parses, body excludes newline", "[wire]")
{
    auto r = ExtractLengthFramed("8:1|tok|{}\n");
    REQUIRE_FALSE(r.error);
    REQUIRE_FALSE(r.needMoreData);
    REQUIRE(r.body == "1|tok|{}");
    REQUIRE(r.consumed == 11);
}

TEST_CASE("framing: two glued frames extract sequentially and drain", "[wire]")
{
    std::string buf = "8:1|tok|{}\n8:2|tok|{}\n";
    auto r1 = ExtractLengthFramed(buf);
    REQUIRE(r1.body == "1|tok|{}");
    buf.erase(0, r1.consumed);

    auto r2 = ExtractLengthFramed(buf);
    REQUIRE(r2.body == "2|tok|{}");
    buf.erase(0, r2.consumed);

    auto r3 = ExtractLengthFramed(buf);
    REQUIRE(r3.needMoreData); // drained
}

TEST_CASE("framing: non-numeric length prefix is an error", "[wire]")
{
    auto r = ExtractLengthFramed("notanum:bogus\n");
    REQUIRE(r.error);
}

TEST_CASE("framing: partially-numeric prefix is rejected (audit M-V4-5)", "[wire]")
{
    auto r = ExtractLengthFramed("12abc:somebodybytes\n");
    REQUIRE(r.error);
}

TEST_CASE("framing: partial frame stays buffered, completes on append", "[wire]")
{
    std::string buf = "8:1|tok|"; // 8 bytes promised, 5 of body present
    auto r1 = ExtractLengthFramed(buf);
    REQUIRE(r1.needMoreData);

    buf += "{}\n";
    auto r2 = ExtractLengthFramed(buf);
    REQUIRE(r2.body == "1|tok|{}");
}

TEST_CASE("framing: frame missing its '\\n' delimiter is a desync error (E01-3d)", "[wire]")
{
    // Length prefix promises 4 body bytes; the byte at the terminator offset is
    // 'X', not '\n'. Before E01-3d this silently accepted body "body" and
    // consumed on the wrong boundary; now it is flagged as a stream-desync error.
    auto r = ExtractLengthFramed("4:bodyX");
    REQUIRE(r.error);
    REQUIRE_FALSE(r.needMoreData);
}

TEST_CASE("framing: correct '\\n' delimiter still parses after E01-3d", "[wire]")
{
    // Positive control: a well-terminated frame is unaffected by the delimiter check.
    auto r = ExtractLengthFramed("4:body\n");
    REQUIRE_FALSE(r.error);
    REQUIRE_FALSE(r.needMoreData);
    REQUIRE(r.body == "body");
    REQUIRE(r.consumed == 7);
}

TEST_CASE("framing: length above maxBodySize is rejected", "[wire]")
{
    auto r = ExtractLengthFramed("99:abc\n", /*maxBodySize=*/16);
    REQUIRE(r.error);
}

TEST_CASE("message: serialize -> extract -> parse round-trips", "[wire]")
{
    Message m;
    m.type    = 42;
    m.token   = std::string(64, 'a');
    m.payload = R"({"key":"va|ue with pipe"})"; // '|' in payload is legal

    auto framed = ExtractLengthFramed(m.Serialize());
    REQUIRE_FALSE(framed.error);

    Message back = Message::ParseBody(framed.body);
    REQUIRE(back.type == m.type);
    REQUIRE(back.token == m.token);
    REQUIRE(back.payload == m.payload);
}
