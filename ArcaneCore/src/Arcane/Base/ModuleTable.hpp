#pragma once

// Crash window plan 1 (Task 3): a lock-free snapshot of this process's
// loaded modules -- base, size, base name -- so the crash thread (Task 5)
// can resolve a raw address to "module + offset" without DbgHelp or the
// loader lock (both need locks a crashing thread may already hold).
//
// Refresh() runs OFF the crash path: ForeignModules::Scan() calls it (at
// device creation and when a verify report is written -- never per frame),
// and Diagnostics::Install (Task 5) calls it once at startup. Find() is what
// the crash path calls, from inside an exception filter.
//
// Double-buffered so a reader is never blocked by, and never observes a
// half-written, snapshot: Refresh fills the INACTIVE buffer under a mutex,
// stores its count, then publishes it with a release store of the active
// index; Find loads that index with acquire and scans only the buffer it
// names -- the acquire/release pair makes every write Refresh did before the
// store visible to Find after its load, with no lock on the read side.
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
    // discipline that makes Find() lock-free and crash-filter-safe.
    class ARCANE_CORE_API ModuleTable
    {
    public:
        static constexpr std::size_t kMax = 512;

        // Replaces the snapshot from a fresh module enumeration. Takes the
        // table's write lock -- callable from any ordinary thread, never
        // from the crash thread itself. `modules` beyond kMax are dropped.
        static void Refresh(std::span<const ForeignModules::LoadedModule> modules) noexcept;

        // The entry whose [base, base + size) contains `address`, or
        // nullptr. Lock-free: safe to call from an exception filter.
        static const ModuleEntry* Find(std::uint64_t address) noexcept;

        // The module count from the most recently published snapshot.
        static std::size_t Count() noexcept;
    };
}
