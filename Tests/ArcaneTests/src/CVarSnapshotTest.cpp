// The published snapshot (settings spec s4.6, O5): workers read an immutable
// CVarSnapshot that Publish swaps in atomically. [cvar]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Jobs/JobSystem.hpp>

#include "Helpers/CVarTestDesc.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

using namespace Arcane;

namespace S1SnapshotTest
{
    ARC_CVAR(cvar_snapshotProbe, "tests.snapshotProbe", std::int32_t, 7,
             .help = "CVarRef snapshot probe (CVarSnapshotTest).");
}

TEST_CASE("cvar snapshot: it changes only at Publish, callbacks already see the new snapshot, CVarRef::Get reads it", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle h = reg.Register(Test::Desc("test.snap", CVarValue::Int32(1)));
    const std::shared_ptr<const CVarSnapshot> before = reg.Snapshot();
    REQUIRE(before);
    CHECK(before->Get(h)->AsInt32() == 1);
    REQUIRE(reg.Set(h, CVarValue::Int32(2), SetBy::Code) == SetResult::Applied);
    CHECK(reg.Snapshot()->Get(h)->AsInt32() == 1);          // pending is not published
    struct Seen { CVarRegistry* reg; int value = 0; } seen{ &reg };
    reg.AddCallback(h, [](CVarHandle handle, void* u)
    {
        auto* s = static_cast<Seen*>(u);
        s->value = s->reg->Snapshot()->Get(handle)->AsInt32();
    }, &seen);
    reg.PublishImmediate();
    CHECK(reg.Snapshot()->Get(h)->AsInt32() == 2);
    CHECK(seen.value == 2);                                  // swapped before the callbacks ran
    CHECK(before->Get(h)->AsInt32() == 1);                   // a held snapshot never changes
    CHECK(reg.Snapshot()->serial > before->serial);

    CVarRegistry& process = CVarRegistry::Get();
    const CVarHandle probe = S1SnapshotTest::cvar_snapshotProbe.Handle();
    REQUIRE(process.Set(probe, CVarValue::Int32(8), SetBy::Code, "snapshot-test") == SetResult::Applied);
    CHECK(S1SnapshotTest::cvar_snapshotProbe.Get() == 7);    // still the published value
    process.PublishImmediate();
    CHECK(S1SnapshotTest::cvar_snapshotProbe.Get() == 8);
    process.UnregisterModule("snapshot-test");               // pops the Code record
    process.PublishImmediate();
    CHECK(S1SnapshotTest::cvar_snapshotProbe.Get() == 7);
}

TEST_CASE("cvar snapshot: job-system workers read whole values while the main thread publishes (no torn reads)", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle vec = reg.Register(Test::Desc("stress.vec", CVarValue::Vec4(CVarVec4{ 0, 0, 0, 0 })));
    const CVarHandle text = reg.Register(Test::Desc("stress.text", CVarValue::String("x")));
    REQUIRE_FALSE(vec.IsStale());
    REQUIRE_FALSE(text.IsStale());
    constexpr int kReaders = 4;
    constexpr int kPublishes = 2000;
    std::atomic<bool> stop{ false };
    std::atomic<int> started{ 0 };
    std::atomic<int> finished{ 0 };
    std::atomic<int> torn{ 0 };
    std::atomic<std::uint64_t> reads{ 0 };
    {
        // JobSystem's count is a TOTAL that includes this thread, and Submit
        // never runs on it: kReaders + 1 gives every reader its own worker,
        // so all of them spin at once while this thread publishes.
        JobSystem jobs(kReaders + 1);
        for (int r = 0; r < kReaders; ++r)
            jobs.Submit([&]
            {
                float last = -1.0f;
                std::uint64_t lastSerial = 0;
                ++started;
                while (!stop.load(std::memory_order_acquire))
                {
                    const std::shared_ptr<const CVarSnapshot> snap = reg.Snapshot();
                    if (snap->serial < lastSerial) ++torn;
                    lastSerial = snap->serial;
                    const auto v = snap->Get(vec);
                    const auto t = snap->Get(text);
                    if (!v || !t) { ++torn; continue; }
                    const CVarVec4 q = v->AsVec4();
                    if (q.x != q.y || q.y != q.z || q.z != q.w) ++torn;
                    if (q.x < last) ++torn;                     // a later snapshot never shows an older value
                    last = q.x;
                    const std::string& s = t->AsString();
                    if (s.empty() || !std::all_of(s.begin(), s.end(), [](char c) { return c == 'x'; })) ++torn;
                    ++reads;
                }
                ++finished;
            });
        // Every reader is spinning before the first publish, so the readers
        // and the publisher overlap for the whole run.
        while (started.load() < kReaders) std::this_thread::yield();
        for (int i = 1; i <= kPublishes; ++i)
        {
            const float f = static_cast<float>(i);
            REQUIRE(reg.Set(vec, CVarValue::Vec4(CVarVec4{ f, f, f, f }), SetBy::Code) == SetResult::Applied);
            REQUIRE(reg.Set(text, CVarValue::String(std::string(static_cast<std::size_t>(i % 64 + 1), 'x')), SetBy::Code) == SetResult::Applied);
            reg.Publish();
        }
        stop.store(true, std::memory_order_release);
        while (finished.load() < kReaders) std::this_thread::yield();
    }
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
    CHECK(reg.Snapshot()->Get(vec)->AsVec4().x == static_cast<float>(kPublishes));
}
