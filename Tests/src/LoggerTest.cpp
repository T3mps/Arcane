// Core Logger generic-surface coverage (game-agnostic purge, 2026-07-10):
// the string-keyed named-logger seam and the E01-4 JsonEscape kernel. The
// structured analytics-event escaping proof moved with the events to the
// Aphelyon facade (Server/Common/tests/AnalyticsEventShapeTest.cpp).
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Util/Logger.hpp>

TEST_CASE("Logger: JsonEscape escapes quotes, backslashes, and control chars", "[logger]")
{
    using Arcane::Logger;
    CHECK(Logger::JsonEscape("plain") == "plain");
    CHECK(Logger::JsonEscape("a\"b") == "a\\\"b");
    CHECK(Logger::JsonEscape("a\\b") == "a\\\\b");
    CHECK(Logger::JsonEscape("a\nb") == "a\\nb");
}

TEST_CASE("Logger: Get by name returns a usable named logger", "[logger]")
{
    auto* lg = Arcane::Logger::Get("Core");
    REQUIRE(lg != nullptr);
    CHECK(lg->name() == "Core");
    // Same name twice = same logger (registry, not a fresh sink stack per call).
    CHECK(Arcane::Logger::Get("Core") == lg);
    // A second, arbitrary name also works (lazy creation).
    auto* other = Arcane::Logger::Get("PurgeSeamProbe");
    REQUIRE(other != nullptr);
    CHECK(other != lg);
}
