#include <catch2/catch_test_macros.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>

#include <cstdint>
#include <vector>

using Arcane::ITaskExecutor;
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
