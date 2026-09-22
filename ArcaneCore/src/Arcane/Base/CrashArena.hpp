#pragma once

// Crash window plan 1 (Task 2): a fixed-size bump arena that is the ONLY
// allocator the crash path (Task 5's report steps) and the fail-fast
// handlers (Task 7) may use once a fault has already happened -- the
// process's heap may itself be the thing that just faulted, so nothing
// downstream of a crash may call new/malloc, and nothing here may throw.
//
// Instance() is one static 256 KiB block, reset at the top of each report
// (Reset()) and bump-allocated for the report's lifetime: the module table,
// the unwound stack, the text report and the envelope JSON all carve their
// scratch space out of it via Alloc/Format/OpenBuilder. Exhaustion -- the
// report turned out to need more than 256 KiB -- is a flag (Exhausted()),
// never a null deref or a thrown allocator failure; a truncated report beats
// no report. Never a general allocator: nothing outside the crash path may
// reach for this.
//
// The sized constructor and the destructor's owned-buffer free exist for
// tests only (a small, private, deterministic arena per test case); the
// crash path only ever touches Instance().

#include <Arcane/Core/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Arcane::Diagnostics
{
    // Spec §5.5. One static 256 KiB block, bump-allocated by the crash path
    // only. Never a general allocator.
    class ARCANE_CORE_API CrashArena
    {
    public:
        static constexpr std::size_t kCapacity = 256 * 1024;

        // The one static instance -- a function-local static over a static
        // 256 KiB block, never freed. Never null, never throws.
        static CrashArena& Instance() noexcept;

        // Back to empty for a fresh report. Does not clear Exhausted() from
        // a PRIOR report until this is called -- exhaustion is only ever
        // reported for the report that caused it.
        void Reset() noexcept;

        // Bump-allocates `bytes` aligned to `align` (must be a power of two).
        // nullptr, and Exhausted() becomes true, when the arena has no room
        // left -- never a throw, never a null deref by the caller's fault.
        void* Alloc(std::size_t bytes, std::size_t align = 16) noexcept;

        // snprintf's `fmt` into a 512-byte slot carved with Alloc. Returns a
        // NUL-terminated view into the arena (possibly truncated at 511
        // characters), or "" (a static empty string, NOT nullptr) when the
        // arena has no room for the slot -- the arena is already marked
        // exhausted in that case.
        const char* Format(const char* fmt, ...) noexcept;

        // Bounded append builder for the text report and the envelope JSON.
        // `owner` lets Append() mark the arena exhausted when it overruns
        // the reserved span, without the caller having to check every call.
        //
        // A nested class does not inherit the outer class's dllexport --
        // mark it explicitly (Core-DLL split, spec 2026-09-15 s8; see
        // Cli.hpp's Result for the same fix).
        struct ARCANE_CORE_API Builder
        {
            char* begin;
            char* cursor;
            char* end;
            CrashArena* owner;

            // Copies min(remaining space, s.size()) bytes of `s` and advances
            // cursor. Marks owner->m_exhausted when `s` does not fully fit --
            // never writes past `end`, never throws.
            void Append(std::string_view s) noexcept;

            // Everything copied into the reserved span so far ([begin, cursor)).
            [[nodiscard]] std::string_view View() const noexcept;
        };

        // Carves `reserveBytes` from the arena right now (Alloc under the
        // hood) and returns a Builder over that span. A Builder from an
        // exhausted carve has begin == cursor == end, so every Append on it
        // is a no-op overrun (View() stays empty; the arena is already
        // marked exhausted by the failed Alloc).
        Builder OpenBuilder(std::size_t reserveBytes) noexcept;

        [[nodiscard]] bool Exhausted() const noexcept;
        [[nodiscard]] std::size_t Used() const noexcept;

        // For tests: a private, small arena (mallocs; never used on the
        // crash path itself). The static instance is Instance().
        explicit CrashArena(std::size_t capacityBytes);

        CrashArena();
        ~CrashArena();

        CrashArena(const CrashArena&) = delete;
        CrashArena& operator=(const CrashArena&) = delete;

    private:
        char* m_buffer;
        std::size_t m_capacity;
        std::size_t m_used;
        bool m_exhausted;
        bool m_owned;
    };
}
