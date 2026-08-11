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
    // NAME must be unique across the whole test exe, anonymous namespace or not:
    // Astra identifies a type by its UNQUALIFIED name hash, and refuses the second
    // type that hashes the same (TypeContext.hpp -- "do not place two same-named
    // types in anonymous namespaces across translation units"). This was plain
    // `Counter`, byte-identical to RuntimeTest.cpp's own anonymous `Counter`; the
    // older Astra silently ALIASED them onto one ComponentID, so these two suites
    // were quietly sharing a component. The vendor sync turned that into a loud
    // refusal, which is how it was finally noticed.
    struct JobCounter { int value = 0; };
}

TEST_CASE("enkiTS work scheduler drives Astra parallel iteration", "[jobs]")
{
    Arcane::JobSystem jobs;
    std::shared_ptr<Mosaic::IWorkScheduler> sched = jobs.WorkScheduler();
    REQUIRE(sched != nullptr);
    REQUIRE(sched->WorkerCount() >= 1);

    Astra::Registry::Config cfg;
    cfg.workScheduler = sched;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<JobCounter>();

    constexpr int kN = 4096;
    for (int i = 0; i < kN; ++i)
        reg.CreateEntityWith(JobCounter{i});

    std::atomic<int> visited{0};
    auto view = reg.CreateView<JobCounter>();
    view.ParallelForEach([&](Astra::Entity, JobCounter& c)
    {
        c.value += 1;
        visited.fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(visited.load() == kN);
}
