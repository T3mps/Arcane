#pragma once

// Crash window plan 1 (Task 3): a lock-free snapshot of this process's
// loaded modules -- base, size, base name -- so the crash thread (Task 5)
// can resolve a raw address to "module + offset" without DbgHelp or the
// loader lock (both need locks a crashing thread may already hold).
//
// Refresh() runs OFF the crash path: ForeignModules::Scan() calls it (at
// device creation, when a verify report is written, and on the user-
// triggered path in EditorApp.cpp -- never per frame), and
// Diagnostics::Install (Task 5) calls it once at startup. Find() is what
// the crash path calls, from inside an exception filter.
//
// Double-buffered so a reader is never blocked by, and never observes a
// half-written, snapshot: Refresh fills the INACTIVE buffer under a mutex,
// stores its count, then publishes it with a release store of the active
// index; Find loads that index with acquire and scans only the buffer it
// names -- the acquire/release pair makes every write Refresh did before the
// store visible to Find after its load, with no lock on the read side.
//
// A double buffer alone only protects a reader against ONE subsequent
// flip. Find() hands back a raw ModuleEntry*, and its real caller
// (PortableStack's capture-then-format sequence, Task 5's report) holds and
// dereferences that pointer well after the Find() call returns -- across
// the whole capture, not just for the duration of one call. If two OTHER
// threads' Refresh() calls landed in that window (Scan() runs off the main
// thread too, e.g. EditorApp.cpp's user-triggered rescan), the second flip
// republishes the very buffer the reader's pointer still points into,
// overwriting it out from under a live read: a genuine data race, not just
// stale data. SetFrozen/Frozen close that window outright: a report FREEZES
// the table for its whole lifetime (Task 5's SubmitReport calls
// SetFrozen(true) beside Log::FreezeBacklog(), and unfreezes once the
// report is written), and Refresh() is a no-op -- no write, no flip -- for
// as long as the table is frozen. This is also the right STORY for a
// report to tell: it describes the module set as it stood at the fault, and
// a module loading or unloading mid-report is not part of that story.
//
// Fixed capacity (kMax) and no heap: ModuleEntry is a POD with a fixed
// name[64] buffer, so the table behaves identically on the crash path as
// anywhere else in the process. Portable: no OS calls live here (see
// PortableStack.hpp for the Windows-only stack walk that consumes Find()).

#include <Arcane/Base/ForeignModules.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Arcane::Diagnostics
{
    // One loaded module, fixed-size so the crash path never touches the
    // heap. `name` is the base name (e.g. "ArcaneCore.dll"), NUL-terminated,
    // truncated at 63 characters if the real name is longer. A namespace-
    // level POD with no out-of-line members -- it needs no ARCANE_CORE_API
    // (MSVC does not propagate dllexport to nested classes, but this is not
    // one, and there is nothing out-of-line to export either way).
    struct ModuleEntry
    {
        std::uint64_t base;
        std::uint64_t size;
        char name[64];
    };

    // Spec S5.2 step 1. See the file header for the double-buffering
    // discipline that makes Find() lock-free and crash-filter-safe, and for
    // why a report additionally freezes the table.
    class ARCANE_CORE_API ModuleTable
    {
    public:
        static constexpr std::size_t kMax = 512;

        // Replaces the snapshot from a fresh module enumeration. Takes the
        // table's write lock -- callable from any ordinary thread, never
        // from the crash thread itself. `modules` beyond kMax are dropped.
        // A no-op -- no write, no flip -- while Frozen() (see SetFrozen).
        static void Refresh(std::span<const ForeignModules::LoadedModule> modules) noexcept;

        // The entry whose [base, base + size) contains `address`, or
        // nullptr. Lock-free: safe to call from an exception filter.
        static const ModuleEntry* Find(std::uint64_t address) noexcept;

        // The module count from the most recently published snapshot.
        static std::size_t Count() noexcept;

        // Freezes (true) or unfreezes (false) the table against Refresh.
        // Task 5's SubmitReport sets this beside Log::FreezeBacklog() for
        // the lifetime of one report, so every Find() a report makes --
        // and every pointer it holds onto afterwards, e.g. into
        // FormatStackFrame -- resolves against the exact snapshot the fault
        // was taken against, with no other thread's Refresh() able to
        // rewrite it out from under a live read (see the file header).
        static void SetFrozen(bool frozen) noexcept;

        // Whether the table is currently frozen.
        [[nodiscard]] static bool Frozen() noexcept;
    };
}
