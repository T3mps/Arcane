#include <Arcane/Host/BootSequence.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ServiceThread.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Arcane
{
    struct BootSequence::Impl
    {
        std::vector<BootStage> stages;

        // Worker handoff. One worker stage runs at a time by construction.
        std::mutex              mx;
        std::condition_variable cv;
        int                     pending    = -1;    // index queued for the worker
        int                     finished   = -1;    // index the worker completed
        bool                    workerOk   = true;
        // Distinct from `pending == -1` (which also means "idle, no job yet").
        // The wait predicate below is `pending >= 0 || stopWorker`; folding stop
        // into the same -1 sentinel as "no job" would leave the predicate with
        // nothing that ever becomes true on RequestStop(), so the worker parks
        // in cv.wait() forever and the ServiceThread destructor's join() hangs.
        bool                    stopWorker = false;
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

        auto present = [&](const std::string& stageId)
        {
            if (!presenter) return true;
            BootProgress p;
            p.fraction = static_cast<float>(static_cast<double>(doneWeight) /
                                            static_cast<double>(totalWeight));
            p.stageId  = stageId;
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
                            std::lock_guard lk(im.mx);
                            im.pending  = workerRunning;
                            im.finished = -1;
                        }
                        if (!worker)
                        {
                            worker.emplace("boot.worker",
                                [&im]
                                {
                                    for (;;)
                                    {
                                        int job = -1;
                                        {
                                            std::unique_lock lk(im.mx);
                                            im.cv.wait(lk, [&] { return im.pending >= 0 || im.stopWorker; });
                                            if (im.pending < 0)
                                                return;   // stop requested, no job queued
                                            job = im.pending;
                                            im.pending = -1;
                                        }
                                        bool ok = true;
                                        try { ok = im.stages[static_cast<std::size_t>(job)].run(); }
                                        catch (...) { ok = false; }
                                        {
                                            std::lock_guard lk(im.mx);
                                            im.workerOk = ok;
                                            im.finished = job;
                                        }
                                        im.cv.notify_all();
                                    }
                                },
                                [&im]
                                {
                                    std::lock_guard lk(im.mx);
                                    im.stopWorker = true;   // wake the worker's cv.wait
                                    im.cv.notify_all();
                                });
                        }
                        im.cv.notify_all();
                        break;
                    }
                }
            }

            // 2. Run ONE ready main stage, in registration order.
            bool didMain = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (ready(i) && im.stages[i].thread == BootThread::Main)
                {
                    started[i] = true;
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

            if (!present(didMain ? std::string("main") : std::string("waiting")))
            {
                result.quitRequested = true;
                result.failedStage   = "quit requested";
                return result;
            }

            // 3. Reap the worker if it finished.
            if (workerRunning >= 0)
            {
                int fin = -1; bool ok = true;
                {
                    std::unique_lock lk(im.mx);
                    if (!didMain)
                        im.cv.wait(lk, [&] { return im.finished >= 0; });   // nothing else to do
                    fin = im.finished;
                    ok  = im.workerOk;
                    if (fin >= 0) im.finished = -1;
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

        present("done");
        result.ok = true;
        return result;
    }
}
