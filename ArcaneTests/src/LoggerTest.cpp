// Core Logger generic-surface coverage (game-agnostic purge, 2026-07-10):
// the string-keyed named-logger seam and the E01-4 JsonEscape kernel. The
// structured analytics-event escaping proof moved with the events to the
// Aphelyon facade (Server/Common/tests/AnalyticsEventShapeTest.cpp).
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
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

TEST_CASE("Logger: concurrent first Get of the same name is race-free", "[logger]")
{
    // Regression (branch review, Important #2): the create-on-miss path was an
    // unsynchronized check-then-register, and spdlog::register_logger throws
    // on a duplicate name -- two threads racing the first Get of a fresh
    // category could throw from inside a log macro. The miss path is now
    // serialized; all racers must get the same logger and none may throw.
    constexpr int kThreads = 8;
    std::array<spdlog::logger*, kThreads> results{};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&results, &ready, &go, i] {
            ready.fetch_add(1);
            while (!go.load()) { }
            results[static_cast<size_t>(i)] = Arcane::Logger::Get("RaceProbeCategory");
        });
    }
    while (ready.load() < kThreads) { }
    go.store(true);
    for (auto& t : threads)
        t.join();
    for (int i = 0; i < kThreads; ++i)
    {
        REQUIRE(results[static_cast<size_t>(i)] != nullptr);
        CHECK(results[static_cast<size_t>(i)] == results[0]);
    }
}

TEST_CASE("Logger: late Init with a file path upgrades a console-only auto-init", "[logger]")
{
    // Regression (branch review, Important #4): logging before the app's real
    // Init auto-initializes console-only and used to permanently block the
    // later Init's file sink (early-return on s_initialized). A later Init
    // with a path must now install the sink and attach it to loggers that
    // already exist. Keep this case LAST in the TU: it calls Shutdown, which
    // resets the logging singleton for whoever runs next.
    auto* early = Arcane::Logger::Get("LateUpgradeProbe");  // console-only auto-init
    REQUIRE(early != nullptr);

    auto path = std::filesystem::temp_directory_path() / "arcane_logger_late_upgrade.log";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    Arcane::Logger::Init(Arcane::Level::Info, Arcane::Level::Trace, path.string());
    early->info("late-upgrade probe message");
    Arcane::Logger::Shutdown();  // flushes + drops the registry

    REQUIRE(std::filesystem::exists(path));
    std::ifstream in(path);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(contents.find("late-upgrade probe message") != std::string::npos);
}
