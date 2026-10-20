#include "Project/CookQueue.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/Constant.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace Arcane::Editor
{
    CookQueue::CookQueue(std::filesystem::path projectDir, SubmitFn submit,
                         const Arcane::AssetPipeline::TextureMetaSettings& textureDefaults)
        : m_projectDir(std::move(projectDir))
        , m_submit(std::move(submit))
        , m_textureDefaults(textureDefaults)
    {
    }

    CookQueue::~CookQueue()
    {
        // See the header's own comment: block until RunOnePass (if it is
        // mid-flight on a JobSystem worker) finishes, so it never touches
        // this object's members after they are destroyed. A manually-pumped
        // test's "worker" is the test's own thread and always finishes
        // BEFORE returning from whatever ran the stashed job, so this loop
        // never waits at all in that shape -- it only matters for the real
        // JobSystem-backed production path. The wait is bounded: a cook that
        // is still running after the deadline cannot be destroyed under, so
        // the process aborts loudly rather than hanging the editor's shutdown.
        ARC_CONSTANT("shutdown poll interval for a live cook pass; not a tunable")
        constexpr auto kPoll = std::chrono::milliseconds(50);
        ARC_CONSTANT("shutdown deadline for a live cook pass: past it the process aborts; a safety bound, not a tunable")
        constexpr auto kDeadline = std::chrono::seconds(120);
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_running)
                    return;
            }
            if (std::chrono::steady_clock::now() - start >= kDeadline)
            {
                ARC_ERROR("CookQueue: a cook pass did not finish within {} s of shutdown; aborting rather "
                          "than destroying the queue under a live pass", kDeadline.count());
                std::abort();
            }
            std::this_thread::sleep_for(kPoll);
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

    void CookQueue::SetTextureDefaults(const Arcane::AssetPipeline::TextureMetaSettings& defaults)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (defaults == m_textureDefaults)
                return;
            m_textureDefaults = defaults;
        }
        NoteChanged();
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
        // Clears m_running under the mutex on EVERY exit, so a throw (or the normal
        // return) can never leave the destructor waiting on a pass that is gone.
        struct RunningGuard
        {
            CookQueue& q;
            ~RunningGuard()
            {
                std::lock_guard<std::mutex> lock(q.m_mutex);
                q.m_running = false;
            }
        } guard{ *this };

        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_session.SetTextureDefaults(m_textureDefaults);   // S6-6: this pass's project defaults
            }

            Arcane::AssetPipeline::CookResult result;
            bool threw = false;
            try
            {
                result = m_session.CookProject(m_projectDir);
            }
            catch (const std::exception& ex)
            {
                threw = true;
                result.failed = 1;
                result.failures.emplace_back(Arcane::Guid{}, std::string("cook pass threw: ") + ex.what());
            }
            catch (...)
            {
                threw = true;
                result.failed = 1;
                result.failures.emplace_back(Arcane::Guid{}, std::string("cook pass threw"));
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            m_results.push_back(std::move(result));

            // A throw ends the pass: it does not start another one straight away
            // (the next NoteChanged does). The guard clears m_running after this.
            if (threw || !m_dirty)
                return;
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
