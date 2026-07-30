// ServiceThread: lifetime for long-lived BLOCKING workers (compile, build,
// file IO). Not for fork-join compute -- that is JobSystem. CPU-only.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/ServiceThread.hpp>

TEST_CASE("ServiceThread runs its body on another thread", "[threading]")
{
    const std::thread::id callerId = std::this_thread::get_id();
    std::atomic<bool> ran{false};
    std::atomic<bool> sameThread{true};
    {
        Arcane::ServiceThread svc("test.runs", [&]
        {
            sameThread = (std::this_thread::get_id() == callerId);
            ran = true;
        });
        // Destructor joins, so by the closing brace the body has completed.
    }
    CHECK(ran.load());
    CHECK_FALSE(sameThread.load());
}

TEST_CASE("StopRequested is observable from the body and the destructor joins", "[threading]")
{
    std::atomic<int> ticks{0};
    std::atomic<const Arcane::ServiceThread*> self{nullptr};
    {
        Arcane::ServiceThread svc("test.stopflag", [&]
        {
            while (!self.load()) std::this_thread::yield();   // wait for construction to publish
            while (!self.load()->StopRequested())
            {
                ++ticks;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        self = &svc;
        while (ticks.load() < 3) std::this_thread::yield();
    }   // dtor: RequestStop + join
    const int settled = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ticks.load() == settled);   // truly joined, not detached
}

TEST_CASE("wake unblocks a body sleeping on its own condition variable", "[threading]")
{
    // This is the ShaderCompiler shape: the consumer owns the mutex/cv/queue,
    // ServiceThread owns only the stop flag and the join.
    std::mutex mx;
    std::condition_variable cv;
    std::atomic<bool> finished{false};
    std::atomic<const Arcane::ServiceThread*> self{nullptr};

    {
        Arcane::ServiceThread svc("test.wake",
            [&]
            {
                while (!self.load()) std::this_thread::yield();
                std::unique_lock lk(mx);
                cv.wait(lk, [&] { return self.load()->StopRequested(); });
                finished = true;
            },
            [&]
            {
                std::lock_guard lk(mx);   // lock before notify: the waiter
                cv.notify_all();          // re-checks the predicate under it
            });
        self = &svc;
    }   // dtor: RequestStop -> wake -> join. Hangs forever if wake is broken.

    CHECK(finished.load());
}

TEST_CASE("ServiceThread with no wake callback still destructs cleanly", "[threading]")
{
    Arcane::ServiceThread svc("test.nowake", [] {});
    CHECK(svc.DebugName() == "test.nowake");
}
