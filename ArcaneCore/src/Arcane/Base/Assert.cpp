#include <Arcane/Base/Assert.hpp>

#include <Arcane/Base/CrashArena.hpp>    // the reason slot -- the crash path's only allocator
#include <Arcane/Base/Diagnostics.hpp>   // SubmitReport: a failing guard IS a report
#include <Arcane/Base/Log.hpp>           // Arcane::Log::Engine()

#include <spdlog/spdlog.h>

namespace
{
    // noexcept: log the failure Critical through the engine logger with the
    // stringized condition + file:line, then decide how this guard ends. No
    // "Mosaic" logger -- the engine logger owns it.
    //
    // THREE ENDINGS, in the order they are tested (spec S5.3):
    //  1. a debugger is attached      -> Break, exactly as before, so Mosaic's
    //     FailFatal breaks INTO the debugger at the failing guard. Nothing
    //     about a debugged session should be diverted into a crash report.
    //  2. an ARC_ENSURE (depth > 0)   -> a LIGHTWEIGHT report (envelope +
    //     portable stack, no minidump) and Continue: SubmitReport returns for
    //     exitCode 0, FailEnsure never aborts, and the caller runs its
    //     recovery. UE's continuable-report shape.
    //  3. anything else (ARC_ASSERT/ARC_VERIFY) -> a fatal report of kind
    //     `assert`; SubmitReport does not return, so Mosaic's abort() is
    //     never reached and the process dies with ExitCode::kCrashed.
    //
    // The reason strings are the kind: DeriveKindCStr classifies on the
    // "assert: " / "ensure: " prefix, so those prefixes are load-bearing.
    Mosaic::AssertAction MosaicAssertHandlerImpl(const Mosaic::AssertContext& c, void* /*user*/) noexcept
    {
        try
        {
            Arcane::Log::Engine()->log(
                spdlog::source_loc{c.location.file_name(),
                                   static_cast<int>(c.location.line()),
                                   c.location.function_name()},
                spdlog::level::critical, "assertion failed: {}{}",
                c.expression ? c.expression : "<expr>",
                c.message ? std::string(" - ") + c.message : std::string());
        }
        catch (...) {}

#if defined(_WIN32)
        if (::IsDebuggerPresent() != 0)
            return Mosaic::AssertAction::Break;
#endif

        // Everything below runs on a thread that is already in trouble: no
        // heap, no std::string -- the arena, and nothing else (R3).
        const char* const expr = c.expression ? c.expression : "<expr>";
        const char* const msg  = c.message    ? c.message    : "";
        const char* const file = c.location.file_name();
        const unsigned    line = static_cast<unsigned>(c.location.line());

        if (Arcane::Assert::EnsureDepth() > 0)
        {
            const char* const reason = Arcane::Diagnostics::CrashArena::Instance().Format(
                "ensure: %s -- %s (%s:%u)", expr, msg, file, line);
            Arcane::Diagnostics::SubmitReport({ reason, nullptr, /*lightweight*/true, 0 });
            return Mosaic::AssertAction::Continue;
        }

        const char* const reason = Arcane::Diagnostics::CrashArena::Instance().Format(
            "assert: %s -- %s (%s:%u)", expr, msg, file, line);
        Arcane::Diagnostics::SubmitReport({ reason, nullptr, /*lightweight*/false,
                                            Arcane::Diagnostics::ExitCode::kCrashed });

        // Unreachable: a fatal SubmitReport terminates the process. It exists
        // because the compiler needs a return, and because a Diagnostics that
        // could not write anything at all must still end the guard the way it
        // always did.
        return Mosaic::AssertAction::Break;
    }
}

namespace Arcane::Assert
{
    Mosaic::AssertHandler MosaicHandler() noexcept { return &MosaicAssertHandlerImpl; }

    // The ONE definition of the depth, in the DLL, so every module's
    // ARC_ENSURE and this file's handler agree (see the header's note).
    int& EnsureDepth() noexcept
    {
        static thread_local int s_depth = 0;
        return s_depth;
    }
}
