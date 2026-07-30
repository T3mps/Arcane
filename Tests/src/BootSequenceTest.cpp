// BootSequence: the boot-stage DAG. Pure scheduler, no GPU/window/ImGui.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/BootSequence.hpp>

namespace
{
    Arcane::BootStage Stage(std::string id, std::vector<std::string> deps,
                            std::function<bool()> run,
                            Arcane::BootThread thread = Arcane::BootThread::Main,
                            Arcane::BootPolicy policy = Arcane::BootPolicy::Fatal)
    {
        Arcane::BootStage s;
        s.id = std::move(id);
        s.dependsOn = std::move(deps);
        s.thread = thread;
        s.policy = policy;
        s.weight = 1;
        s.run = std::move(run);
        return s;
    }
}

TEST_CASE("stages run in dependency order", "[boot]")
{
    std::vector<std::string> order;
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("c", {"b"}, [&] { order.push_back("c"); return true; }));
    stages.push_back(Stage("a", {},    [&] { order.push_back("a"); return true; }));
    stages.push_back(Stage("b", {"a"}, [&] { order.push_back("b"); return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    REQUIRE(r.ok);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "a");
    CHECK(order[1] == "b");
    CHECK(order[2] == "c");
}

TEST_CASE("a worker stage genuinely overlaps a main stage", "[boot]")
{
    // Proof, not assumption: the worker blocks until the main stage signals.
    // Bounded, not infinite: Catch2 v3 has no built-in per-test timeout, so an
    // unbounded spin here would wedge the whole process instead of failing if
    // a future change ever serialised worker stages onto the main thread.
    // The deadline turns that regression into a red CHECK -- workerSawOverlap
    // is only set when mainRan flips WITHIN the deadline, so a serialised
    // (worker-runs-to-completion-first) regression times out with
    // workerSawOverlap left false, not a hang.
    std::atomic<bool> mainRan{false};
    std::atomic<bool> workerSawOverlap{false};

    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("worker", {}, [&]
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!mainRan.load())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return true;   // timed out: workerSawOverlap stays false
            std::this_thread::yield();
        }
        workerSawOverlap = true;
        return true;
    }, Arcane::BootThread::Worker));
    stages.push_back(Stage("main", {}, [&]
    {
        mainRan = true;
        return true;
    }, Arcane::BootThread::Main));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK(mainRan.load());
    CHECK(r.ok);
    CHECK(workerSawOverlap.load());
}

TEST_CASE("the presenter keeps ticking while a worker stage overlaps", "[boot]")
{
    // Count ticks observed WHILE THE WORKER STAGE IS ACTUALLY IN FLIGHT, not a
    // raw total. A raw total does not discriminate: the pre-fix scheduler
    // called present() unconditionally once per loop iteration plus once more
    // at the terminal "done" tick, which is already 2 calls for a single
    // worker stage even with zero pumping while parked -- so a bare
    // `ticks > 1` (or even a naive `>= 2`) stays green on the starving
    // scheduler this test exists to catch. Flagging "in flight" from inside
    // the worker stage itself and counting only ticks observed while that
    // flag is set measures the property directly, independent of how many
    // ticks happen to land outside the overlap.
    std::atomic<bool> workerInFlight{false};
    struct Counter final : Arcane::IBootPresenter
    {
        std::atomic<bool>* inFlight = nullptr;
        int overlapTicks = 0;
        bool Present(const Arcane::BootProgress&) override
        {
            if (inFlight->load()) ++overlapTicks;
            return true;
        }
    } counter;
    counter.inFlight = &workerInFlight;

    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("slow_worker", {}, [&]
    {
        workerInFlight = true;
        // 60ms against the scheduler's 8ms pump cadence (BootSequence.cpp)
        // yields roughly 7 in-flight ticks -- a >=3 floor leaves about 2x
        // margin under the observed count even before accounting for a slow
        // CI box, while still being unreachable by the starving scheduler
        // (which produces 0 in-flight ticks: its one "waiting" tick lands
        // before the freshly-dispatched worker thread has set the flag, and
        // its one "done" tick lands after the worker already cleared it).
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        workerInFlight = false;
        return true;
    }, Arcane::BootThread::Worker));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&counter);

    CHECK(r.ok);
    CHECK(counter.overlapTicks >= 3);
}

TEST_CASE("a dependency cycle is refused and names the offenders", "[boot]")
{
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("x", {"y"}, [] { return true; }));
    stages.push_back(Stage("y", {"x"}, [] { return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK_FALSE(r.ok);
    CHECK(r.failedStage.find("x") != std::string::npos);
}

TEST_CASE("an Optional stage's failure lets dependents run", "[boot]")
{
    bool dependentRan = false;
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("opt", {}, [] { return false; },
                           Arcane::BootThread::Main, Arcane::BootPolicy::Optional));
    stages.push_back(Stage("dep", {"opt"}, [&] { dependentRan = true; return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK(r.ok);              // Optional failure does not fail the boot
    CHECK(dependentRan);
}

TEST_CASE("a Fatal stage's failure aborts and skips dependents", "[boot]")
{
    bool dependentRan = false;
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("bad", {}, [] { return false; }));
    stages.push_back(Stage("dep", {"bad"}, [&] { dependentRan = true; return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK_FALSE(r.ok);
    CHECK(r.failedStage == "bad");
    CHECK_FALSE(dependentRan);
}

TEST_CASE("duplicate stage ids are refused", "[boot]")
{
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("dup", {}, [] { return true; }));
    stages.push_back(Stage("dup", {}, [] { return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(nullptr);

    CHECK_FALSE(r.ok);
    CHECK(r.failedStage.find("dup") != std::string::npos);
}

TEST_CASE("progress reaches 1.0 and never goes backwards", "[boot]")
{
    struct Recorder final : Arcane::IBootPresenter
    {
        float last = 0.0f;
        bool monotonic = true;
        bool Present(const Arcane::BootProgress& p) override
        {
            if (p.fraction < last) monotonic = false;
            last = p.fraction;
            return true;   // do not request quit
        }
    } rec;

    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("a", {},    [] { return true; }));
    stages.push_back(Stage("b", {"a"}, [] { return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&rec);

    CHECK(r.ok);
    CHECK(rec.monotonic);
    CHECK(rec.last == 1.0f);
}

TEST_CASE("a presenter requesting quit aborts the boot cleanly", "[boot]")
{
    struct Quitter final : Arcane::IBootPresenter
    {
        bool Present(const Arcane::BootProgress&) override { return false; }
    } quitter;

    std::atomic<int> ran{0};
    std::vector<Arcane::BootStage> stages;
    for (int i = 0; i < 8; ++i)
        stages.push_back(Stage("s" + std::to_string(i), {}, [&] { ++ran; return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&quitter);

    CHECK_FALSE(r.ok);
    CHECK(r.quitRequested);
    CHECK(ran.load() < 8);   // stopped early
}
