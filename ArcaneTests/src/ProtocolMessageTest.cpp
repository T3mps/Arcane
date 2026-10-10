// Message::ToString (settings inventory R4, 2026-10-03): a log line must never
// carry a payload -- Login/Register hold a password, the Login response a
// session token. Only the payload's byte count is printed.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Net/Protocol.hpp>

#include <string>

TEST_CASE("Message::ToString prints the payload's size, never its bytes", "[net]")
{
    Arcane::Message login;
    login.type = 1;
    login.payload = R"({"username":"ethan","password":"hunter2-correct-horse"})";
    const std::string text = login.ToString();
    INFO(text);
    CHECK(text.find("hunter2") == std::string::npos);
    CHECK(text.find("password") == std::string::npos);
    CHECK(text.find("ethan") == std::string::npos);
    CHECK(text.find("payload=" + std::to_string(login.payload.size()) + " bytes") != std::string::npos);

    Arcane::Message big;
    big.type = 2;
    big.payload.assign(500, 'x');
    CHECK(big.ToString().find("xxxx") == std::string::npos);
    CHECK(big.ToString().find("payload=500 bytes") != std::string::npos);

    Arcane::Message empty;
    CHECK(empty.ToString().find("payload=0 bytes") != std::string::npos);
}

TEST_CASE("Message::ToString keeps the token preview to its first 8 and last 4 characters", "[net]")
{
    Arcane::Message m;
    m.type = 3;
    m.token = std::string(28, 'a') + "SECRETMIDDLE" + std::string(24, 'b');   // 64 characters
    const std::string text = m.ToString();
    CHECK(text.find("token=aaaaaaaa...bbbb") != std::string::npos);
    CHECK(text.find("SECRETMIDDLE") == std::string::npos);

    Arcane::Message none;
    none.type = 3;
    CHECK(none.ToString().find("token=(no token)") != std::string::npos);
}
