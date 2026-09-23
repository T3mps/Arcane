// The symbolized report's MODEL and its TEXT (crash window plan 2, task 5;
// spec §6 "Symbolization").
//
// PURE and std-only on purpose, exactly like ReporterArgs.hpp: this TU
// source-compiles into ArcaneTests (premake5.lua, ArcaneTests' `files` list)
// so the [reporter] units drive the formatting and the thread ordering
// directly. That list is NOT gated on the target OS, so nothing here -- and
// nothing SymbolizedText.cpp includes -- may reach windows.h. Every dbgeng and
// Win32 dependency lives in Symbolizer.cpp, which is NOT compiled into the
// tests; Symbolizer.hpp is the seam between the two.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Reporter
{
    struct SymFrame
    {
        std::uint64_t address      = 0;
        std::string   module;             // "death_fixture" (dbgeng's name) or "death-fixture.exe" (image name, symbol-less fallback)
        std::string   function;           // empty when no symbol resolved
        std::uint64_t displacement = 0;   // from `function` when set, else from the module base
        std::string   file;               // empty when no line info
        std::uint32_t line         = 0;
    };

    struct SymThread
    {
        // 0 == the engine would not name this thread (R77). It prints as
        // "<unknown>", NEVER as "thread 0": a correct stack under a plausible
        // wrong id is worse than an obviously missing one.
        std::uint32_t         systemId = 0;
        bool                  faulting = false;
        std::vector<SymFrame> frames;
        // R78: the walk stopped at the reporter's per-thread cap, not at the
        // bottom of the stack. Appended LAST so the existing brace-init of
        // this aggregate -- { id, faulting, frames } -- keeps working.
        bool                  framesTruncated = false;
    };

    struct Symbolized
    {
        bool                   engineAvailable = false;   // DebugCreate + OpenDumpFile + WaitForEvent all succeeded
        std::string            engineError;               // why not, when not
        std::string            symbolPath;                // what the engine was actually given
        std::vector<SymThread> threads;                   // the faulting thread first
        bool                   threadsTruncated = false;  // R78: the dump held more threads than the cap
    };

    // "module!function+0x1a [file:line]"  |  "module!function+0x1a"  |  "module+0x1234"
    [[nodiscard]] std::string FormatFrame(const SymFrame& f);

    // The whole <stem>.symbolized.txt. `portableFallback` (the envelope's
    // cpuThreadSummary) is the body when the engine was unavailable.
    [[nodiscard]] std::string FormatSymbolized(const Symbolized& s, std::string_view buildInfo,
                                               std::string_view portableFallback);

    // The walked thread's id from the envelope's stack text ("--- thread 1234
    // (MAIN) ---" is the first line plan 1 writes); 0 when absent. Used to put
    // that thread first when the dump carries no exception event.
    [[nodiscard]] std::uint32_t ParseWalkedThreadId(std::string_view cpuThreadSummary);

    // Moves the thread whose systemId matches (or the one flagged faulting) to the front.
    void PutFaultingFirst(Symbolized& s, std::uint32_t walkedThreadId);
}
