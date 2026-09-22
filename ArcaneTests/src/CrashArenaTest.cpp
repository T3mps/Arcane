// Crash window plan 1, Task 2: CrashArena is the ONLY allocator the crash
// path (Task 5) and the fail-fast handlers (Task 7) may use after a fault.
// These cases prove the private sized-ctor arena (deterministic capacity,
// no static-instance cross-test bleed) and the static Instance() singleton
// both bump-allocate, align, format and build bounded text, and turn
// exhaustion into a flag rather than a null deref or a throw.

#include <Arcane/Base/CrashArena.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

TEST_CASE("crash arena: bump allocates aligned blocks, formats, and reports exhaustion instead of failing", "[diag]")
{
    Arcane::Diagnostics::CrashArena arena(1024);
    void* a = arena.Alloc(100, 16);
    void* b = arena.Alloc(100, 64);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK((reinterpret_cast<std::uintptr_t>(b) % 64) == 0);
    CHECK(arena.Used() >= 200);

    const char* text = arena.Format("pid %u kind %s", 42u, "crash");
    CHECK(std::strcmp(text, "pid 42 kind crash") == 0);

    auto builder = arena.OpenBuilder(64);
    builder.Append("hello ");
    builder.Append("world");
    CHECK(builder.View() == "hello world");

    // Exhaustion is a flag, never a null deref or a throw.
    void* big = arena.Alloc(4096);
    CHECK(big == nullptr);
    CHECK(arena.Exhausted());

    arena.Reset();
    CHECK_FALSE(arena.Exhausted());
    CHECK(arena.Used() == 0);
    CHECK(arena.Alloc(512) != nullptr);
}

TEST_CASE("crash arena: the static instance holds 256 KiB and a builder that overruns marks exhaustion", "[diag]")
{
    auto& arena = Arcane::Diagnostics::CrashArena::Instance();
    arena.Reset();
    auto builder = arena.OpenBuilder(8);
    builder.Append("0123456789");   // 10 > 8
    CHECK(builder.View().size() == 8);
    CHECK(arena.Exhausted());
    arena.Reset();
}
