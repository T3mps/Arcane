#include <Arcane/Host/BootSequence.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ServiceThread.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Arcane
{
    void BootStageDetail::Set(std::string text)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_text = std::move(text);
    }

    std::string BootStageDetail::Get() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_text;
    }

    struct BootSequence::Impl
    {
        std::vector<BootStage> stages;
    };

    BootSequence::BootSequence(std::vector<BootStage> stages)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->stages = std::move(stages);
    }

    BootSequence::~BootSequence() = default;

    BootResult BootSequence::Run(IBootPresenter* presenter)
    {
        Impl& im = *m_impl;
        BootResult result;

        // ---- validate: unique ids -------------------------------------------
        std::unordered_map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < im.stages.size(); ++i)
        {
            if (!index.emplace(im.stages[i].id, i).second)
            {
                result.failedStage = "duplicate stage id '" + im.stages[i].id + "'";
                ARC_ERROR("BootSequence: {}", result.failedStage);
                return result;
            }
        }

        // ---- validate: dependencies resolve ---------------------------------
        for (const BootStage& s : im.stages)
            for (const std::string& d : s.dependsOn)
                if (!index.count(d))
                {
                    result.failedStage = "stage '" + s.id + "' depends on unknown '" + d + "'";
                    ARC_ERROR("BootSequence: {}", result.failedStage);
                    return result;
                }

        const std::size_t n = im.stages.size();
        std::vector<int>  remaining(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            remaining[i] = static_cast<int>(im.stages[i].dependsOn.size());

        std::vector<bool> done(n, false), skipped(n, false), started(n, false);

        std::uint64_t totalWeight = 0;
        for (const BootStage& s : im.stages) totalWeight += s.weight;
        if (totalWeight == 0) totalWeight = 1;
        std::uint64_t doneWeight = 0;

        // Worker handoff. Deliberately a Run()-local (not an Impl/BootSequence
        // -lifetime) piece of state: every field here, including stopWorker,
        // starts fresh on every call. Putting it on Impl would let a stale
        // stopWorker == true from a PRIOR Run() make a freshly spawned worker
        // exit after servicing exactly one job on a later Run() -- the reap
        // would then wait on a thread that already exited, forever. Scoping
        // the handoff to Run() makes that carry-over structurally impossible
        // rather than relying on a reset call someone has to remember.
        // One worker stage runs at a time by construction.
        struct WorkerHandoff
        {
            std::mutex              mx;
            std::condition_variable cv;
            int                     pending    = -1;    // index queued for the worker
            int                     finished   = -1;    // index the worker completed
            bool                    workerOk   = true;
            // Distinct from `pending == -1` (which also means "idle, no job
            // yet"). The wait predicate below is `pending >= 0 || stopWorker`;
            // folding stop into the same -1 sentinel as "no job" would leave
            // the predicate with nothing that ever becomes true on
            // RequestStop(), so the worker parks in cv.wait() forever and the
            // ServiceThread destructor's join() hangs.
            bool                    stopWorker = false;
        } wh;

        std::optional<ServiceThread> worker;
        int workerRunning = -1;

        auto ready = [&](std::size_t i)
        {
            return !done[i] && !skipped[i] && !started[i] && remaining[i] == 0;
        };

        auto complete = [&](std::size_t i, bool ok)
        {
            done[i] = true;
            doneWeight += im.stages[i].weight;
            if (!ok && im.stages[i].policy == BootPolicy::Fatal)
                return false;
            if (!ok)
                ARC_WARN("BootSequence: optional stage '{}' failed; continuing", im.stages[i].id);
            // Release dependents regardless: an Optional failure must not strand
            // them (OpenProject failing still lets the host boot project-less).
            for (std::size_t j = 0; j < n; ++j)
                for (const std::string& d : im.stages[j].dependsOn)
                    if (d == im.stages[i].id) --remaining[j];
            return true;
        };

        // `stageId` is a real BootStage::id (see the header's BootProgress
        // doc) -- callers pass the stage that just completed, or the stage
        // presently running on the worker; an empty string only at the
        // terminal "done" tick where no single stage owns the update.
        auto present = [&](const std::string& stageId)
        {
            if (!presenter) return true;
            BootProgress p;
            p.fraction = static_cast<float>(static_cast<double>(doneWeight) /
                                            static_cast<double>(totalWeight));
            p.stageId  = stageId;
            // `index` (built above, id -> position) doubles as the lookup this
            // needs: the stage this update is ABOUT, if any (empty stageId only
            // at the terminal "done" tick, which owns no single stage and so
            // finds nothing here -- p.detail stays empty, exactly as before this
            // field had a producer). A stage with no attached BootStageDetail
            // (the common case) also leaves p.detail empty -- this is purely
            // additive over the old always-empty behaviour.
            if (auto it = index.find(stageId); it != index.end())
                if (const auto& d = im.stages[it->second].detail)
                    p.detail = d->Get();
            return presenter->Present(p);
        };

        std::size_t completed = 0;
        while (completed < n)
        {
            // 1. Dispatch a worker-eligible stage FIRST so the worker is never
            //    idle while main chews on a long stage. This is what produces
            //    the project_open / gpu_core overlap.
            if (workerRunning < 0)
            {
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (ready(i) && im.stages[i].thread == BootThread::Worker)
                    {
                        started[i]    = true;
                        workerRunning = static_cast<int>(i);
                        {
                            std::lock_guard lk(wh.mx);
                            wh.pending  = workerRunning;
                            wh.finished = -1;
                        }
                        if (!worker)
                        {
                            worker.emplace("boot.worker",
                                [&wh, &im]
                                {
                                    for (;;)
                                    {
                                        int job = -1;
                                        {
                                            std::unique_lock lk(wh.mx);
                                            wh.cv.wait(lk, [&] { return wh.pending >= 0 || wh.stopWorker; });
                                            if (wh.pending < 0)
                                                return;   // stop requested, no job queued
                                            job = wh.pending;
                                            wh.pending = -1;
                                        }
                                        bool ok = true;
                                        try { ok = im.stages[static_cast<std::size_t>(job)].run(); }
                                        catch (...) { ok = false; }
                                        {
                                            std::lock_guard lk(wh.mx);
                                            wh.workerOk = ok;
                                            wh.finished = job;
                                        }
                                        wh.cv.notify_all();
                                    }
                                },
                                [&wh]
                                {
                                    std::lock_guard lk(wh.mx);
                                    wh.stopWorker = true;   // wake the worker's cv.wait
                                    wh.cv.notify_all();
                                });
                        }
                        wh.cv.notify_all();
                        break;
                    }
                }
            }

            // 2. Run ONE ready main stage, in registration order.
            bool didMain = false;
            std::string ranStageId;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (ready(i) && im.stages[i].thread == BootThread::Main)
                {
                    started[i] = true;
                    ranStageId = im.stages[i].id;
                    bool ok = true;
                    try { ok = im.stages[i].run(); }
                    catch (...) { ok = false; }
                    if (!complete(i, ok))
                    {
                        result.failedStage = im.stages[i].id;
                        ARC_ERROR("BootSequence: fatal stage '{}' failed", result.failedStage);
                        return result;
                    }
                    ++completed;
                    didMain = true;
                    break;
                }
            }

            if (didMain && !present(ranStageId))
            {
                result.quitRequested = true;
                result.failedStage   = "quit requested";
                return result;
            }

            // 3. Reap the worker if it finished.
            if (workerRunning >= 0)
            {
                const std::string workerStageId = im.stages[static_cast<std::size_t>(workerRunning)].id;
                int fin = -1; bool ok = true;
                if (didMain)
                {
                    // Main had useful work this iteration -- just peek, do not block.
                    std::lock_guard lk(wh.mx);
                    fin = wh.finished;
                    ok  = wh.workerOk;
                    if (fin >= 0) wh.finished = -1;
                }
                else
                {
                    // Nothing else to do on main: park on the worker, but keep
                    // the presenter pumping at roughly display cadence rather
                    // than blocking silently for the whole overlap. This DAG
                    // exists for exactly one overlap (see the header comment);
                    // an unpumped main thread stops redrawing the loading
                    // screen for as long as that overlap runs, and Windows
                    // marks the window "Not Responding" -- the precise
                    // failure this arc exists to prevent.
                    std::unique_lock lk(wh.mx);
                    for (;;)
                    {
                        wh.cv.wait_for(lk, std::chrono::milliseconds(8), [&] { return wh.finished >= 0; });
                        if (wh.finished >= 0) break;
                        lk.unlock();
                        if (!present(workerStageId))
                        {
                            result.quitRequested = true;
                            result.failedStage   = "quit requested";
                            return result;
                        }
                        lk.lock();
                    }
                    fin = wh.finished;
                    ok  = wh.workerOk;
                    wh.finished = -1;
                }

                if (fin >= 0)
                {
                    if (!complete(static_cast<std::size_t>(fin), ok))
                    {
                        result.failedStage = im.stages[static_cast<std::size_t>(fin)].id;
                        ARC_ERROR("BootSequence: fatal worker stage '{}' failed", result.failedStage);
                        return result;
                    }
                    ++completed;
                    workerRunning = -1;
                }
            }
            else if (!didMain && completed < n)
            {
                // Nothing ready and nothing running: a cycle.
                std::string names;
                for (std::size_t i = 0; i < n; ++i)
                    if (!done[i]) { if (!names.empty()) names += ", "; names += im.stages[i].id; }
                result.failedStage = "dependency cycle among: " + names;
                ARC_ERROR("BootSequence: {}", result.failedStage);
                return result;
            }
        }

        present(std::string());   // boot complete -- no single stage owns this update
        result.ok = true;
        return result;
    }
}
