#include "Project/CookQueue.hpp"

#include <thread>
#include <utility>

namespace Arcane::Editor
{
    CookQueue::CookQueue(std::filesystem::path projectDir, SubmitFn submit)
        : m_projectDir(std::move(projectDir))
        , m_submit(std::move(submit))
    {
    }

    CookQueue::~CookQueue()
    {
        // See the header's own comment: block until RunOnePass (if it is
        // mid-flight on a JobSystem worker) finishes, so it never touches
        // this object's members after they are destroyed. A manually-pumped
        // test's "worker" is the test's own thread and always finishes
        // BEFORE returning from whatever ran the stashed job, so this loop
        // never spins at all in that shape -- it only matters for the real
        // JobSystem-backed production path.
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_running)
                    return;
            }
            std::this_thread::yield();
        }
    }

    void CookQueue::SetImporterForTesting(Arcane::AssetPipeline::CookSession::ImporterFn fn)
    {
        // F2c Task 8 renamed CookSession's own seam to SetTextureImporterForTesting (now
        // that there are two importers to inject) -- this wrapper's own name and public
        // surface are unchanged; only the forwarded call follows the rename.
        m_session.SetTextureImporterForTesting(std::move(fn));
    }

    void CookQueue::SetOnCookComplete(CompletionFn fn)
    {
        m_onComplete = std::move(fn);
    }

    void CookQueue::NoteChanged()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running)
        {
            // Already running (or about to run again from RunOnePass's own
            // loop) -- ask it to do one more pass before it goes idle,
            // rather than submitting a second, concurrent CookProject over
            // the same CookSession.
            m_dirty = true;
            return;
        }
        m_running = true;
        m_dirty = false;   // this call is what's driving the pass about to start
        m_submit([this] { RunOnePass(); });
    }

    bool CookQueue::CookPending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_running;
    }

    void CookQueue::RunOnePass()
    {
        // Runs on whichever thread m_submit actually executes the job on --
        // a JobSystem worker in production, the test's own thread when it
        // manually pumps the stashed job. Loops rather than resubmitting
        // through m_submit for a coalesced follow-up pass (NoteChanged's own
        // comment): a burst of changes while this is running collapses into
        // extra iterations of the SAME job, never one extra Submit() per
        // change.
        for (;;)
        {
            Arcane::AssetPipeline::CookResult result = m_session.CookProject(m_projectDir);

            std::lock_guard<std::mutex> lock(m_mutex);
            m_results.push_back(std::move(result));

            if (!m_dirty)
            {
                m_running = false;
                return;
            }
            m_dirty = false;   // consumed -- one more pass, still under m_running == true
        }
    }

    void CookQueue::Pump()
    {
        std::vector<Arcane::AssetPipeline::CookResult> drained;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            drained.swap(m_results);
        }
        if (!m_onComplete)
            return;
        for (const Arcane::AssetPipeline::CookResult& r : drained)
            m_onComplete(r);
    }
}
