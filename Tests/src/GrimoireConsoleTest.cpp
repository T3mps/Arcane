// Grimoire console ring buffer: bounded FIFO of formatted log lines the Console
// panel renders. CPU-only ([grimoire]).

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ConsoleBuffer.hpp>

TEST_CASE("ConsoleBuffer keeps the newest N lines and drops the oldest", "[grimoire]")
{
    Grimoire::ConsoleBuffer buf(3);
    buf.Push("a"); buf.Push("b"); buf.Push("c");
    CHECK(buf.Size() == 3);
    buf.Push("d");                       // evicts "a"
    CHECK(buf.Size() == 3);

    std::vector<std::string> seen;
    buf.ForEach([&](const std::string& s) { seen.push_back(s); });
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "b");
    CHECK(seen[1] == "c");
    CHECK(seen[2] == "d");
}
