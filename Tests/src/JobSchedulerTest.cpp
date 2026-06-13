// JobSystem hands Astra an enkiTS-backed IWorkScheduler. A registry parallel
// pass must visit every entity exactly once across worker threads.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Jobs/JobSystem.hpp>

#include <Astra/Registry/Registry.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace
{
    struct Counter { int value = 0; };
}

TEST_CASE("enkiTS work scheduler drives Astra parallel iteration", "[jobs]")
{
    Arcane::JobSystem jobs;
    std::shared_ptr<Astra::IWorkScheduler> sched = jobs.WorkScheduler();
    REQUIRE(sched != nullptr);
    REQUIRE(sched->WorkerCount() >= 1);

    Astra::Registry::Config cfg;
    cfg.workScheduler = sched;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<Counter>();

    constexpr int kN = 4096;
    for (int i = 0; i < kN; ++i)
        reg.CreateEntityWith(Counter{i});

    std::atomic<int> visited{0};
    auto view = reg.CreateView<Counter>();
    view.ParallelForEach([&](Astra::Entity, Counter& c)
    {
        c.value += 1;
        visited.fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(visited.load() == kN);
}
