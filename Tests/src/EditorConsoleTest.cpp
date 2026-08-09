// Arcane Editor console ring buffer: bounded FIFO of structured log entries the
// Console panel renders. CPU-only ([editor]).

#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ConsoleBuffer.hpp>

namespace
{
    Arcane::Editor::ConsoleEntry Line(std::string message)
    {
        Arcane::Editor::ConsoleEntry e;
        e.level   = Arcane::DiagSeverity::Info;
        e.message = std::move(message);
        return e;
    }
}

TEST_CASE("ConsoleBuffer keeps the newest N entries and drops the oldest", "[editor]")
{
    Arcane::Editor::ConsoleBuffer buf(3);
    buf.Push(Line("a")); buf.Push(Line("b")); buf.Push(Line("c"));
    CHECK(buf.Size() == 3);
    buf.Push(Line("d"));                 // evicts "a"
    CHECK(buf.Size() == 3);

    std::vector<std::string> seen;
    buf.ForEach([&](const Arcane::Editor::ConsoleEntry& e) { seen.push_back(e.message); });
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "b");
    CHECK(seen[1] == "c");
    CHECK(seen[2] == "d");
}

TEST_CASE("ConsoleBuffer::Clear empties it", "[editor]")
{
    Arcane::Editor::ConsoleBuffer buf(8);
    buf.Push(Line("a"));
    buf.Clear();
    CHECK(buf.Size() == 0);
}

TEST_CASE("SetCapacity trims immediately to the new cap", "[editor]")
{
    Arcane::Editor::ConsoleBuffer buf(8);
    for (int i = 0; i < 8; ++i) buf.Push(Line(std::to_string(i)));
    buf.SetCapacity(3);
    CHECK(buf.Size() == 3);

    std::vector<std::string> seen;
    buf.ForEach([&](const Arcane::Editor::ConsoleEntry& e) { seen.push_back(e.message); });
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "5");
}

TEST_CASE("ConsoleBuffer tolerates a worker pushing while the main thread reads", "[editor]")
{
    // The async-boot arc logs from a worker thread; ConsoleBuffer.hpp's original
    // comment called this out as the case that needs the lock.
    Arcane::Editor::ConsoleBuffer buf(256);

    std::thread worker([&]
    {
        for (int i = 0; i < 500; ++i)
            buf.Push(Line("worker " + std::to_string(i)));
    });

    for (int i = 0; i < 500; ++i)
    {
        std::size_t n = 0;
        buf.ForEach([&](const Arcane::Editor::ConsoleEntry&) { ++n; });
        CHECK(n <= 256);
    }

    worker.join();
    CHECK(buf.Size() == 256);
}
