// Property: any (type, token, payload) survives Serialize -> frame-extract
// -> ParseBody. Token is pipe-free by protocol construction (64 hex chars
// in production); payload may contain pipes and newlines -- the length
// prefix disambiguates.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck/catch.h>
#include <string>
#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Net/Protocol.hpp>

using Arcane::ExtractLengthFramed;
using Arcane::Message;

TEST_CASE("wire round-trip property", "[wire][property]")
{
    rc::prop("serialize/extract/parse preserves all fields",
             [](uint16_t type, const std::string& payload) {
        RC_PRE(type > 0);
        RC_PRE(payload.size() < 4096);

        Message m;
        m.type    = type;
        m.token   = std::string(64, 'f');
        m.payload = payload;

        auto framed = ExtractLengthFramed(m.Serialize());
        RC_ASSERT(!framed.error);
        RC_ASSERT(!framed.needMoreData);

        Message back = Message::ParseBody(framed.body);
        RC_ASSERT(back.type == m.type);
        RC_ASSERT(back.token == m.token);
        RC_ASSERT(back.payload == m.payload);
    });
}
