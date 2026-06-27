#include <catch2/catch_test_macros.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Jobs/JobSystem.hpp>

#include <cstdint>
#include <vector>

using Arcane::ITaskExecutor;
using Arcane::JobSystem;
using Arcane::SerialTaskExecutor;

// Fill out[i] = i*i over [0,count); verify disjoint full cover + worker range.
static void RunCoverWorkload(ITaskExecutor& exec, std::size_t count, std::size_t minBatch,
                             std::vector<int>& visited, std::vector<std::uint64_t>& out,
                             std::uint32_t& maxWorker)
{
    visited.assign(count, 0);
    out.assign(count, 0);
    maxWorker = 0;
    exec.ParallelFor(count, minBatch,
        [&](std::size_t b, std::size_t e, std::uint32_t worker)
        {
            if (worker > maxWorker) maxWorker = worker;   // serial: single worker
            for (std::size_t i = b; i < e; ++i)
            {
                visited[i] += 1;                           // disjoint => exactly 1
                out[i] = static_cast<std::uint64_t>(i) * i;
            }
        });
}

TEST_CASE("SerialTaskExecutor: disjoint full cover + per-element result", "[jobs]")
{
    SerialTaskExecutor exec;
    REQUIRE(exec.WorkerCount() == 1);

    std::vector<int> visited;
    std::vector<std::uint64_t> out;
    std::uint32_t maxWorker = 999;
    RunCoverWorkload(exec, 1000, 64, visited, out, maxWorker);

    REQUIRE(maxWorker < exec.WorkerCount());               // worker in [0,WorkerCount())
    for (std::size_t i = 0; i < 1000; ++i)
    {
        REQUIRE(visited[i] == 1);                          // covered exactly once
        REQUIRE(out[i] == static_cast<std::uint64_t>(i) * i);
    }
}

TEST_CASE("SerialTaskExecutor: edge counts", "[jobs]")
{
    SerialTaskExecutor exec;
    std::vector<int> visited; std::vector<std::uint64_t> out; std::uint32_t mw;

    RunCoverWorkload(exec, 0, 64, visited, out, mw);        // no-op
    REQUIRE(visited.empty());

    RunCoverWorkload(exec, 1, 64, visited, out, mw);        // count < minBatch
    REQUIRE(visited.size() == 1);
    REQUIRE(visited[0] == 1);

    RunCoverWorkload(exec, 7, 0, visited, out, mw);         // minBatch 0 (=>1)
    for (int v : visited) REQUIRE(v == 1);
}

TEST_CASE("JobSystem exposes an ITaskExecutor over the enki pool", "[jobs]")
{
    JobSystem jobs;                          // hardware default threads
    ITaskExecutor* exec = jobs.TaskExecutor();
    REQUIRE(exec != nullptr);
    REQUIRE(exec->WorkerCount() >= 1);
}

TEST_CASE("enki executor: disjoint full cover, worker index in range", "[jobs]")
{
    JobSystem jobs;
    ITaskExecutor* exec = jobs.TaskExecutor();

    const std::size_t count = 10000;
    std::vector<int> visited(count, 0);                  // disjoint ranges => no race per index
    std::vector<std::uint32_t> seenWorker(count, 0xFFFFFFFFu);
    exec->ParallelFor(count, 256,
        [&](std::size_t b, std::size_t e, std::uint32_t worker)
        {
            for (std::size_t i = b; i < e; ++i) { visited[i] += 1; seenWorker[i] = worker; }
        });

    for (std::size_t i = 0; i < count; ++i)
    {
        REQUIRE(visited[i] == 1);
        REQUIRE(seenWorker[i] < exec->WorkerCount());
    }
}

// Per-element independent fill -> output invariant to thread count + serial vs parallel.
static std::vector<std::uint64_t> FillSquares(ITaskExecutor& exec, std::size_t count)
{
    std::vector<std::uint64_t> out(count, 0);
    exec.ParallelFor(count, 128, [&](std::size_t b, std::size_t e, std::uint32_t)
    {
        for (std::size_t i = b; i < e; ++i) out[i] = static_cast<std::uint64_t>(i) * i + 7u;
    });
    return out;
}

TEST_CASE("thread-count invariance: serial == enki(1) == enki(N), byte-identical", "[jobs]")
{
    const std::size_t count = 50000;
    SerialTaskExecutor serial;
    JobSystem oneThread(1);
    JobSystem manyThreads(0);                            // hardware default

    auto resSerial  = FillSquares(serial,                      count);
    auto resOne     = FillSquares(*oneThread.TaskExecutor(),   count);
    auto resMany    = FillSquares(*manyThreads.TaskExecutor(), count);

    REQUIRE(resSerial == resOne);
    REQUIRE(resOne == resMany);                          // independent work => identical regardless of N
}

TEST_CASE("nested ParallelFor from within a worker completes correctly", "[jobs]")
{
    JobSystem jobs;
    ITaskExecutor* exec = jobs.TaskExecutor();

    const std::size_t outer = 64, inner = 64;
    std::vector<int> grid(outer * inner, 0);
    exec->ParallelFor(outer, 1, [&](std::size_t ob, std::size_t oe, std::uint32_t)
    {
        for (std::size_t o = ob; o < oe; ++o)
            exec->ParallelFor(inner, 1, [&](std::size_t ib, std::size_t ie, std::uint32_t)
            {
                for (std::size_t i = ib; i < ie; ++i) grid[o * inner + i] += 1;
            });
    });
    for (int v : grid) REQUIRE(v == 1);
}
