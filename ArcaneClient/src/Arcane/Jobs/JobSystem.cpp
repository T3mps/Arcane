#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Jobs/ArcaneWorkScheduler.hpp>   // presents the enki ITaskExecutor as a Mosaic::IWorkScheduler

#include <TaskScheduler.h>

#include <cassert>
#include <functional>
#include <limits>

namespace Arcane
{
    namespace
    {
        // Adapts enkiTS to Arcane::ITaskExecutor -- the worker-index-aware face.
        // Internal to this TU: consumers only see ITaskExecutor via JobSystem::TaskExecutor().
        class EnkiTaskExecutor final : public ITaskExecutor
        {
        public:
            explicit EnkiTaskExecutor(enki::TaskScheduler& ts) : m_ts(ts) {}

            void ParallelFor(std::size_t count, std::size_t minBatch,
                             FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
            {
                if (count == 0)
                    return;
                assert(count <= static_cast<std::size_t>((std::numeric_limits<uint32_t>::max)()));
                assert(minBatch <= static_cast<std::size_t>((std::numeric_limits<uint32_t>::max)()));

                // fn outlives the task: WaitforTask blocks until all partitions complete.
                // The enki `threadnum` is the worker index (the value the old
                // IWorkScheduler adapter discarded).
                enki::TaskSet task(
                    static_cast<uint32_t>(count),
                    [&fn](enki::TaskSetPartition range, uint32_t threadnum)
                    {
                        fn(range.start, range.end, threadnum);
                    });
                task.m_MinRange = static_cast<uint32_t>(minBatch == 0 ? 1 : minBatch);

                m_ts.AddTaskSetToPipe(&task);
                m_ts.WaitforTask(&task);   // calling thread participates -> nested-safe
            }

            std::uint32_t WorkerCount() const noexcept override
            {
                return static_cast<std::uint32_t>(m_ts.GetNumTaskThreads());
            }

        private:
            enki::TaskScheduler& m_ts;
        };
    }

    struct JobSystem::Impl
    {
        enki::TaskScheduler                     ts;
        std::unique_ptr<EnkiTaskExecutor>       taskExec;  // the sole enki adapter (worker-index face)
        std::shared_ptr<Mosaic::IWorkScheduler> adapter;   // ArcaneWorkScheduler over taskExec; destroyed first
    };

    JobSystem::JobSystem(uint32_t threads) : m_impl(std::make_unique<Impl>())
    {
        if (threads == 0)
            m_impl->ts.Initialize();
        else
            m_impl->ts.Initialize(threads);
        m_impl->taskExec = std::make_unique<EnkiTaskExecutor>(m_impl->ts);
        // WorkScheduler() presents the SAME enki pool as a Mosaic::IWorkScheduler by
        // wrapping the worker-index-aware ITaskExecutor -- one adapter, no duplicate,
        // and it now forwards the per-lane worker id the reconciled seam requires.
        m_impl->adapter  = std::make_shared<ArcaneWorkScheduler>(m_impl->taskExec.get());
    }

    JobSystem::~JobSystem()
    {
        // Drop adapters before the scheduler shuts down (both hold refs to ts).
        // adapter wraps taskExec -> release it first.
        m_impl->adapter.reset();
        m_impl->taskExec.reset();
        m_impl->ts.WaitforAllAndShutdown();
    }

    std::shared_ptr<Mosaic::IWorkScheduler> JobSystem::WorkScheduler() const
    {
        return m_impl->adapter;
    }

    ITaskExecutor* JobSystem::TaskExecutor() const noexcept
    {
        return m_impl->taskExec.get();
    }

    uint32_t JobSystem::WorkerCount() const noexcept
    {
        return m_impl->ts.GetNumTaskThreads();
    }
}
