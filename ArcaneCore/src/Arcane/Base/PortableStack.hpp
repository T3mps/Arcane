#pragma once

// Crash window plan 1 (Task 3): an RtlVirtualUnwind stack walk over a copy
// of a CONTEXT, resolving each address against ModuleTable::Find -- no
// DbgHelp, no loader lock, safe to call from the crash thread's exception
// filter (Task 5). DbgHelp's StackWalk64/SymFromAddr take the process
// symbol-handler lock and may load symbols from disk; a thread that just
// faulted may already hold locks DbgHelp needs, so nothing on the crash
// path may reach for it. RtlLookupFunctionEntry/RtlVirtualUnwind walk the
// PE's own unwind metadata instead and take no such lock.
//
// Windows-only body (Win64 unwind metadata is a Windows concept):
// CaptureStackFromContext/CaptureCurrentStack return 0 on every other
// platform. FormatStackFrame is portable -- no OS calls -- and lives here
// because it is the stack's own presentation, not the module table's.

#include <Arcane/Base/ModuleTable.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Arcane::Diagnostics
{
    // One resolved frame: the raw return address, and the module it falls
    // inside -- nullptr if it resolves to nothing in the current snapshot
    // (an unloaded module, JIT/generated code, or a walk that ran off the
    // stack). A namespace-level POD with no out-of-line members: no
    // ARCANE_CORE_API needed (see ModuleEntry's comment).
    struct StackFrame
    {
        std::uint64_t address = 0;
        const ModuleEntry* module = nullptr;
    };

    // Spec S5.2 step 2. Walks `nativeContext` (a `const CONTEXT*` on
    // Windows) via RtlLookupFunctionEntry + RtlVirtualUnwind over a COPY of
    // it -- unwinding mutates the context in place -- resolving each frame
    // against ModuleTable::Find. Runs inside its own SEH guard: a walk that
    // itself faults (a corrupted frame, a stack that ran off the end) stops
    // where it faulted and returns the frames already written, never
    // propagates the fault. Returns 0 for a null `nativeContext` or an
    // empty `out`, and on every non-Windows platform.
    ARCANE_CORE_API std::size_t CaptureStackFromContext(const void* nativeContext /*CONTEXT**/, std::span<StackFrame> out) noexcept;

    // RtlCaptureContext for the calling thread, then CaptureStackFromContext
    // over it -- the resulting first frame is this function's own caller.
    ARCANE_CORE_API std::size_t CaptureCurrentStack(std::span<StackFrame> out) noexcept;

    // Renders one frame into `buffer` (no heap, no throw):
    //   "  00  ArcaneClient.dll + 0x1234 (base 0x00007ff6...)"  when f.module is known
    //   "  00  0x... <unloaded or unknown module>"              otherwise
    // Returns a view into `buffer`, truncated to fit if `buffer` is too
    // small; empty if `buffer` is empty.
    ARCANE_CORE_API std::string_view FormatStackFrame(std::size_t index, const StackFrame& f, std::span<char> buffer) noexcept;
}
