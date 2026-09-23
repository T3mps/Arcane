// The envelope-to-view model (crash window plan 2, task 6; spec §6 "Window"
// content). What the window shows and what Copy Details copies.
//
// PURE and std-only on purpose, exactly like ReporterArgs.hpp and
// SymbolizedText.hpp: this TU source-compiles into ArcaneTests
// (premake5.lua, ArcaneTests' `files` list) so the [reporter] units drive
// the wording and the thread ordering directly. That list is NOT gated on
// the target OS, so nothing here -- and nothing ReportView.cpp includes --
// may reach windows.h. LogTail.hpp/.cpp (the filesystem half) are NOT
// compiled into the tests and stay out of this header's includes.
#pragma once

#include "ReporterArgs.hpp"
#include "SymbolizedText.hpp"
#include <Arcane/Base/DiagEnvelope.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Reporter
{
    struct ThreadView { std::string label; std::string text; };   // "thread 4242 (faulting)" + its frames

    struct ReportView
    {
        std::string title;          // window title: "<product> -- crashed"
        std::string headline;       // "Arcane Editor crashed"
        std::string whenLine;       // "2026-09-23T10:11:12Z | phase: switch_plugin_load | build: Arcane 0.1 Debug"
        std::string reasonText;     // the envelope's reason, or "(no reason recorded)"
        std::string injectedText;   // one line per module; "" when none; "(not scanned)" is never claimed here
        std::vector<ThreadView> threads;   // faulting first; one thread from the portable stack when no symbolization
        std::string gpuText;        // queues / fault / layers when the envelope has any; "" otherwise
        std::string logTail;        // the last 200 lines
        std::string reportFolder;   // parent of the envelope
        std::string relaunchLine;   // Args::relaunch, else envelope.commandLine
        bool        isHang = false;         // hang | gpu-stall: Keep Waiting + Terminate and Collect, Relaunch disabled
        bool        canRelaunch = false;    // !relaunchLine.empty() && !isHang
        bool        isAbnormalExit = false; // monitor-synthesized: no stack, no threads selector
    };

    // "crashed" | "stopped responding" | "the GPU stopped responding" | "the GPU device was lost"
    // | "an assertion failed" | "terminated" | "ran out of memory" | "hit a recoverable check" | "exited abnormally"
    [[nodiscard]] std::string PlainWordsKind(std::string_view kind);

    [[nodiscard]] ReportView BuildReportView(const Diag::Envelope& e, const Args& a,
                                             const Symbolized* symbolized, std::string_view logTail);

    // The last `n` lines of `text` (pure; used by the log tail and the monitor).
    [[nodiscard]] std::string LastLines(std::string_view text, std::size_t n);

    // Everything, as text: header, reason, injected, the selected thread, GPU, log tail.
    [[nodiscard]] std::string DetailsText(const ReportView& v, std::size_t threadIndex);
}
