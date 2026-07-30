#pragma once

// BootSequence: the boot-stage DAG both hosts run. A pure scheduler over
// callables plus a progress model -- ZERO GPU, window, and ImGui dependency, by
// design (all presentation lives behind IBootPresenter). Keeping it that way is
// what makes it headless-testable and promotable to Core later; do not add an
// NVRHI, SDL, or ImGui include to this header.
//
// Ordering is a stable Kahn topological sort, the same shape as TopoSortPasses
// in the material pass chain.
//
// The DAG exists for exactly ONE overlap: the filesystem-bound project open
// against GPU device creation. Sequential-and-join is the safer default (it is
// what Unreal does); every new Worker stage owes its own disjoint-ownership
// proof before it is added.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Arcane
{
    enum class BootThread : std::uint8_t { Main, Worker };

    // Fatal: failure aborts the boot and skips dependents.
    // Optional: failure is recorded, dependents still run. This exists because
    // OpenProject failing is NOT fatal today -- the host warns and continues
    // with the data/ + --plugin fallback. Preserve that, do not tighten it.
    enum class BootPolicy : std::uint8_t { Fatal, Optional };

    struct BootStage
    {
        std::string              id;
        std::vector<std::string> dependsOn;
        BootThread                thread = BootThread::Main;
        BootPolicy                policy = BootPolicy::Fatal;
        std::uint32_t             weight = 1;      // share of the progress bar
        std::function<bool()>    run;             // false == this stage failed
    };

    struct BootProgress
    {
        float fraction = 0.0f;   // 0..1, monotonic

        // The BootStage::id this update is about: the stage that just
        // completed on the main thread, or the stage currently running on
        // the worker thread while the main thread has nothing else ready.
        // Empty only at the final "boot complete" tick, where no single
        // stage owns the update -- a presenter can treat empty as "no
        // per-stage caption, just show the fraction".
        std::string stageId;

        // Reserved for a stage's own sub-progress (e.g. "412 / 1180").
        // BootSequence never populates this itself today -- no stage in the
        // current DAG has sub-progress to report -- it exists for a future
        // stage callable to fill in, not something the scheduler fabricates.
        std::string detail;
    };

    // The presentation seam. Return false to request an abort (window closed).
    // During a Worker-stage overlap with nothing left to run on the main
    // thread, Present() is called repeatedly at roughly display cadence
    // (see BootSequence.cpp) instead of the main thread blocking silently
    // for the whole overlap -- implementations must stay cheap and must not
    // block, the same expectation as a window message pump.
    struct ARCANE_API IBootPresenter
    {
        virtual ~IBootPresenter() = default;
        virtual bool Present(const BootProgress& progress) = 0;
    };

    struct BootResult
    {
        bool        ok            = false;
        std::string failedStage;          // empty when ok
        bool        quitRequested = false;
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::unique_ptr<Impl> on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API BootSequence
    {
    public:
        explicit BootSequence(std::vector<BootStage> stages);
        ~BootSequence();

        BootSequence(const BootSequence&)            = delete;
        BootSequence& operator=(const BootSequence&) = delete;

        // Drives every stage to completion. `presenter` may be null (headless).
        BootResult Run(IBootPresenter* presenter);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
