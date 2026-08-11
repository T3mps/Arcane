#pragma once

// GPU crash diagnostics arc (Task 3): a pure CPU-side breadcrumb ring for
// ONE GPU queue's scope timeline. A backend (Task 5 = D3D12, Task 6 =
// Vulkan; see IGpuCrashBackend.hpp) calls BeginScope/EndScope around each
// render pass on the CPU-recording timeline (RAII-friendly via the
// returned token) and reports what the GPU actually reached via
// OnMarkerWritten as it observes marker-buffer/DRED state (typically once,
// at crash time). Capture() derives a Snapshot of the queue's state from
// that marker evidence -- feeds Diag::Envelope::Queue (Task 4; one
// GpuBreadcrumbs per queue, its name known only to the caller).
//
// Bounded ring: the most recent kRingCapacity scopes are kept, oldest
// evicted first. Pure -- no GPU/OS dependency, no allocation beyond the
// ring/open-stack themselves. Every method is safe to call with a
// token/id that was never issued or has since been evicted (never
// crashes; see Capture()'s zero-marker case).

#include <Arcane/Base/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    class ARCANE_API GpuBreadcrumbs
    {
    public:
        // Most recent scopes kept per queue. A BeginScope past this evicts
        // the oldest ring entry (its marker/open state is discarded with
        // it) to make room.
        static constexpr std::size_t kRingCapacity = 256;

        // Opens a new scope named `name`, nested inside whatever scope (if
        // any) is currently open on this queue (per prior BeginScope calls
        // not yet EndScope'd). Returns a monotonically increasing token --
        // pass it to EndScope and to OnMarkerWritten as the marker id.
        // Never reused, even across ring eviction.
        std::uint32_t BeginScope(std::string_view name);

        // Closes the scope `token` refers to: CPU-side bookkeeping only --
        // pops the open-scope stack so later BeginScope calls compute the
        // right depth/ancestor. Does NOT imply the scope's end marker was
        // ever observed on the GPU (see OnMarkerWritten) -- a scope can be
        // EndScope'd here yet still show up in a later Capture()'s inFlight
        // if its end marker never arrived (the GPU died before reaching
        // it). A stale/unknown/already-evicted token is a safe no-op.
        void EndScope(std::uint32_t token);

        // Reported by a backend as it observes GPU marker state: `begin`
        // true records the scope's begin marker as reached; false records
        // its end marker. This is the ONLY input Capture() trusts for
        // "did the GPU get there" -- EndScope is CPU-side intent, this is
        // GPU-side fact. A stale/unknown/already-evicted id is a safe
        // no-op.
        void OnMarkerWritten(std::uint32_t id, bool begin);

        // Derived queue-timeline state. lastCompleted names the
        // highest-id scope whose end marker was observed ("" if none).
        // inFlight names every scope whose begin marker was observed but
        // not its end, plus -- for each -- its still-open ancestors (an
        // ancestor with no EndScope call of its own yet, regardless of
        // whether the ancestor ever got its own marker report), emitted
        // oldest-first.
        struct Snapshot
        {
            std::string lastCompleted;
            std::vector<std::string> inFlight;
        };

        // Derives a Snapshot from the current ring + marker state. Pure,
        // const, never throws: an empty ring, or a ring with no marker
        // reports at all, yields an all-empty Snapshot.
        //
        // Named `Capture` rather than `Snapshot` even though it returns a
        // `Snapshot` -- a member function can't share its own nested
        // return type's name and still be definable/usable (MSVC C2761 on
        // the out-of-line definition, C2371 on constructing the return
        // value inside the body); see GpuBreadcrumbsTest.cpp's header
        // comment and task-3-report.md for the compiled proof.
        Snapshot Capture() const;

    private:
        struct Entry
        {
            std::uint32_t id = 0;
            std::string name;
            std::uint32_t depth = 0;
            std::uint32_t parentId = 0; // meaningful only if hasParent
            bool hasParent = false;
            bool closed = false;        // EndScope called (CPU-side intent)
            bool beginWritten = false;  // OnMarkerWritten(id, true) observed
            bool endWritten = false;    // OnMarkerWritten(id, false) observed
        };

        // Ring index of `id`, or m_ring.size() if it is not currently in
        // the ring (never issued, or evicted).
        std::size_t FindIndex(std::uint32_t id) const;

        std::vector<Entry> m_ring;              // ascending id order; front = oldest
        std::vector<std::uint32_t> m_openStack;  // currently-open tokens, CPU side
        std::uint32_t m_nextId = 0;
    };
}
