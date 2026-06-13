#include <Arcane/Jobs/JobSystem.hpp>

#include <TaskScheduler.h>

#include <functional>

namespace Arcane
{
    namespace
    {
        // Adapts enkiTS to Astra::IWorkScheduler. Internal to this TU: consumers
        // only ever see Astra::IWorkScheduler via JobSystem::WorkScheduler().
        class EnkiWorkScheduler : public Astra::IWorkScheduler
        {
        public:
            explicit EnkiWorkScheduler(enki::TaskScheduler& ts) : m_ts(ts) {}

            void ParallelFor(size_t count, size_t minBatch,
                             const std::function<void(size_t, size_t)>& fn) override
            {
                if (count == 0)
                    return;

                enki::TaskSet task(
                    static_cast<uint32_t>(count),
                    [&fn](enki::TaskSetPartition range, uint32_t /*threadnum*/)
                    {
                        fn(range.start, range.end);
                    });
                task.m_MinRange = static_cast<uint32_t>(minBatch == 0 ? 1 : minBatch);

                m_ts.AddTaskSetToPipe(&task);
                m_ts.WaitforTask(&task);   // calling thread participates in the work
            }

            size_t WorkerCount() const noexcept override
            {
                return static_cast<size_t>(m_ts.GetNumTaskThreads());
            }

        private:
            enki::TaskScheduler& m_ts;
        };
    }

    struct JobSystem::Impl
    {
        enki::TaskScheduler ts;
        std::shared_ptr<Astra::IWorkScheduler> adapter;
    };

    JobSystem::JobSystem(uint32_t threads) : m_impl(std::make_unique<Impl>())
    {
        if (threads == 0)
            m_impl->ts.Initialize();
        else
            m_impl->ts.Initialize(threads);
        m_impl->adapter = std::make_shared<EnkiWorkScheduler>(m_impl->ts);
    }

    JobSystem::~JobSystem()
    {
        // Drop the adapter before the scheduler shuts down (adapter holds a ref).
        m_impl->adapter.reset();
        m_impl->ts.WaitforAll();
    }

    std::shared_ptr<Astra::IWorkScheduler> JobSystem::WorkScheduler() const
    {
        return m_impl->adapter;
    }

    uint32_t JobSystem::WorkerCount() const noexcept
    {
        return m_impl->ts.GetNumTaskThreads();
    }
}
