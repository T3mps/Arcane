// Settings blocks over time (settings arc S2, spec s4.4, s4.6): a module unload
// drops its blocks and Settings<T>() falls back to T{}; a reference survives two
// publishes; workers reading through SettingsShared<T>() never see a torn struct.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>

#include "Helpers/SettingsProbe.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Arcane;
using SettingsProbe::ProbeSettings;

namespace
{
    std::string Name(std::string_view field) { return "tests.settingsProbe." + std::string(field); }
    constexpr std::uint64_t kHash = Astra::TypeID<ProbeSettings>::Hash();
}

TEST_CASE("UnregisterModule drops a module's settings: cvars, block, and Settings<T>() falls back to T{}", "[settings]")
{
    CVarRegistry reg;
    REQUIRE(RegisterSettings<ProbeSettings>(reg, "unload-me"));
    REQUIRE(reg.Set(reg.Find(Name("count")), CVarValue::Int32(9), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    REQUIRE(Settings<ProbeSettings>(reg).count == 9);
    const std::shared_ptr<const ProbeSettings> held = SettingsShared<ProbeSettings>(reg);

    reg.UnregisterModule("unload-me");
    CHECK(reg.Find(Name("count")).IsStale());
    CHECK(reg.SettingsBlock(kHash) == nullptr);
    CHECK(Settings<ProbeSettings>(reg).count == 7);
    CHECK(held->count == 9);

    REQUIRE(RegisterSettings<ProbeSettings>(reg, "unload-me"));
    reg.Publish();
    CHECK(Settings<ProbeSettings>(reg).count == 7);
    CHECK(reg.SettingsBlock(kHash) != nullptr);
}

TEST_CASE("UnregisterModule Debug-guards a SettingsShared held from a twice-retired snapshot", "[settings]")
{
    // RebuildSnapshot's StoreSnapshot evicts retired[1]. Capture the dropped
    // blocks before that shuffle so a holder of the twice-retired unique block
    // still trips the Debug guard and keeps its snapshot.
    CVarRegistry reg;
    REQUIRE(RegisterSettings<ProbeSettings>(reg, "retire-hold"));
    reg.Publish();
    const std::shared_ptr<const ProbeSettings> held = SettingsShared<ProbeSettings>(reg);
    const CVarHandle count = reg.Find(Name("count"));
    for (std::int32_t i = 1; i <= 2; ++i)
    {
        REQUIRE(reg.Set(count, CVarValue::Int32(10 + i), SetBy::Code) == SetResult::Applied);
        reg.Publish();
        CHECK(held->count == 7);
    }
    CHECK(Settings<ProbeSettings>(reg).count == 12);
    reg.UnregisterModule("retire-hold");
    CHECK(reg.SettingsBlock(kHash) == nullptr);
    CHECK(held->count == 7);
}

TEST_CASE("A Settings<T>() reference stays readable for two more publishes", "[settings]")
{
    CVarRegistry reg;
    REQUIRE(RegisterSettings<ProbeSettings>(reg, "retire-test"));
    reg.Publish();
    const ProbeSettings& first = Settings<ProbeSettings>(reg);
    const CVarHandle count = reg.Find(Name("count"));
    for (std::int32_t i = 1; i <= 2; ++i)
    {
        REQUIRE(reg.Set(count, CVarValue::Int32(10 + i), SetBy::Code) == SetResult::Applied);
        reg.Publish();
        CHECK(first.count == 7);
    }
    CHECK(Settings<ProbeSettings>(reg).count == 12);
}

TEST_CASE("Workers reading through SettingsShared<T>() never see a torn struct while the main thread publishes", "[settings]")
{
    CVarRegistry reg;
    REQUIRE(RegisterSettings<ProbeSettings>(reg, "thread-test"));
    const CVarHandle count = reg.Find(Name("count"));
    const CVarHandle mask = reg.Find(Name("mask"));
    REQUIRE(reg.Set(count, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    REQUIRE(reg.Set(mask, CVarValue::UInt32(1u), SetBy::Code) == SetResult::Applied);
    reg.Publish();

    std::atomic<bool> stop{ false };
    std::atomic<int> torn{ 0 };
    std::atomic<int> reads{ 0 };
    bool allApplied = true;
    {
        std::vector<std::jthread> workers;
        for (int w = 0; w < 4; ++w)
            workers.emplace_back([&] {
                while (!stop.load(std::memory_order_acquire))
                {
                    const std::shared_ptr<const ProbeSettings> s = SettingsShared<ProbeSettings>(reg);
                    if (static_cast<std::uint32_t>(s->count) != s->mask) torn.fetch_add(1);
                    reads.fetch_add(1);
                }
            });
        for (std::int32_t i = 1; i <= 100; ++i)
        {
            allApplied = reg.Set(count, CVarValue::Int32(i), SetBy::Code) == SetResult::Applied && allApplied;
            allApplied = reg.Set(mask, CVarValue::UInt32(static_cast<std::uint32_t>(i)), SetBy::Code) == SetResult::Applied && allApplied;
            reg.Publish();
        }
        stop.store(true, std::memory_order_release);
    }
    CHECK(allApplied);
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
}
