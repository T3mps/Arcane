// Asset-manager redesign, Plan 2 Task 5: AssetActivityLog -- the editor's
// pure, engine-facade-free session ring over "things that happened to
// assets" (spec s9.2). Headless: no registry, no project, no filesystem --
// every entry below is hand-built, and `when` is caller-stamped exactly as
// production sites stamp `std::chrono::steady_clock::now()`, just with a
// synthetic clock the test controls instead.
//
// Three cases pin the ring's whole contract: capacity wrap (150 pushes ->
// 100 held, the OLDEST 50 gone, newest-first walk starts at the 150th),
// ordering (three pushes replay newest-first), and Clear (empties).

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetActivityLog.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Arcane::Editor;

namespace
{
    // A fixed synthetic instant plus an integer offset -- tests control
    // ordering without depending on wall-clock resolution.
    std::chrono::steady_clock::time_point TickN(int n)
    {
        return std::chrono::steady_clock::time_point{} + std::chrono::seconds(n);
    }

    AssetActivityEntry MakeEntry(int n, AssetActivityKind kind = AssetActivityKind::SourceChanged)
    {
        AssetActivityEntry e;
        e.when  = TickN(n);
        e.guid  = Arcane::Guid::Generate();
        e.name  = "entry-" + std::to_string(n);
        e.kind  = kind;
        return e;
    }
}

TEST_CASE("AssetActivityLog wraps at capacity, oldest 50 of 150 gone", "[editor]")
{
    AssetActivityLog log;
    for (int i = 1; i <= 150; ++i)
        log.Push(MakeEntry(i));

    REQUIRE(log.Size() == AssetActivityLog::kCapacity);
    CHECK(log.Size() == 100);

    std::vector<std::string> namesNewestFirst;
    log.ForEachNewestFirst([&](const AssetActivityEntry& e)
    {
        namesNewestFirst.push_back(e.name);
    });

    REQUIRE(namesNewestFirst.size() == 100);
    // The 150th push is newest -- first out of ForEachNewestFirst.
    CHECK(namesNewestFirst.front() == "entry-150");
    // The 51st push is the oldest SURVIVOR -- last out.
    CHECK(namesNewestFirst.back() == "entry-51");
    // The first 50 pushes (entry-1..entry-50) are gone entirely.
    for (const std::string& name : namesNewestFirst)
        CHECK(name != "entry-1");
    CHECK(std::find(namesNewestFirst.begin(), namesNewestFirst.end(), "entry-50")
          == namesNewestFirst.end());
}

TEST_CASE("AssetActivityLog replays three pushes newest-first", "[editor]")
{
    AssetActivityLog log;
    log.Push(MakeEntry(1, AssetActivityKind::Created));
    log.Push(MakeEntry(2, AssetActivityKind::Cooked));
    log.Push(MakeEntry(3, AssetActivityKind::CookRefused));

    REQUIRE(log.Size() == 3);

    std::vector<std::string> order;
    log.ForEachNewestFirst([&](const AssetActivityEntry& e)
    {
        order.push_back(e.name);
    });

    REQUIRE(order.size() == 3);
    CHECK(order[0] == "entry-3");
    CHECK(order[1] == "entry-2");
    CHECK(order[2] == "entry-1");
}

TEST_CASE("AssetActivityLog Clear empties the ring", "[editor]")
{
    AssetActivityLog log;
    log.Push(MakeEntry(1));
    log.Push(MakeEntry(2));
    REQUIRE(log.Size() == 2);

    log.Clear();

    CHECK(log.Size() == 0);
    int calls = 0;
    log.ForEachNewestFirst([&](const AssetActivityEntry&) { ++calls; });
    CHECK(calls == 0);

    // Clear leaves the ring reusable -- a push right after lands as entry
    // zero again, not appended past a stale m_next.
    log.Push(MakeEntry(3));
    CHECK(log.Size() == 1);
}
