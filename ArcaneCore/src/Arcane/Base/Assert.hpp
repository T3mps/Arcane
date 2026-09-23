#pragma once

// Engine assert seam (Base module): Arcane's ergonomic ARC_ASSERT/VERIFY/ENSURE
// (thin wrappers over Mosaic's guards, the parallel to ARC_* in Log.hpp) plus the
// Mosaic assert HANDLER that routes a failure through the engine logger. The
// handler is defined in Assert.cpp (once, in Arcane.dll); the installer is inline
// so each module installs into its own per-module Mosaic storage.

#include <Arcane/Core/Api.hpp>

#include <Mosaic/Assert.hpp>

#include <atomic>            // ARC_ENSURE's per-call-site one-shot latch
#include <source_location>   // ...captured at the call site, not inside the lambda

namespace Arcane::Assert
{
    ARCANE_CORE_API Mosaic::AssertHandler MosaicHandler() noexcept;

    inline void InstallMosaicHandler() noexcept { Mosaic::SetAssertHandler(MosaicHandler(), nullptr); }

    // THE ENSURE/ASSERT DISCRIMINATOR. Mosaic hands a failing guard to the
    // handler through one AssertContext with no fatal/recoverable flag in it
    // (FailFatal and FailEnsure build the same struct), so the handler cannot
    // tell a survivable ARC_ENSURE from a fatal ARC_ASSERT -- and the two must
    // end very differently: a lightweight report that RETURNS versus a crash
    // report that terminates. ARC_ENSURE therefore raises this depth around
    // its own guard, and the handler reads it.
    //
    // An ACCESSOR over an exported thread_local, not an `inline thread_local`
    // in this header: the handler lives in ArcaneCore.dll (see MosaicHandler
    // above) while ARC_ENSURE expands in whatever module wrote it, and an
    // inline variable would give each module its own copy -- the host would
    // raise its depth and the DLL would read zero, turning every ensure in a
    // host or a game module into a process-killing assert. One definition,
    // in Assert.cpp, is what makes the depth mean the same thing on both
    // sides of the boundary. Still thread-local: two threads' guards must not
    // see each other's depth.
    ARCANE_CORE_API int& EnsureDepth() noexcept;

    // Raises EnsureDepth() around ONE ensure's FAILURE REPORT -- not around
    // its condition (see ARC_ENSURE below, and R22). Nested (a depth, not a
    // flag) because a report's own path may reach another guard.
    struct EnsureScope
    {
        EnsureScope() noexcept  { ++EnsureDepth(); }
        ~EnsureScope() noexcept { --EnsureDepth(); }

        EnsureScope(const EnsureScope&)            = delete;
        EnsureScope& operator=(const EnsureScope&) = delete;
    };
}

// Arcane engine asserts -> Mosaic guards (flow through the installed handler).
#define ARC_ASSERT(cond, msg)  MOSAIC_ASSERT(cond, msg)
#define ARC_VERIFY(cond, msg)  MOSAIC_VERIFY(cond, msg)

// ARC_ENSURE restates MOSAIC_ENSURE's shape (Mosaic/Assert.hpp:233-242) rather
// than wrapping it, for ONE reason: WHERE the EnsureScope may be raised.
//
// THE HAZARD THIS SHAPE AVOIDS. `ARC_ENSURE(SomeCall(), "...")` where SomeCall
// asserts internally is an ordinary shape. Wrapping the whole macro in a scope
// evaluates `cond` with the depth already raised, so that nested fatal
// ARC_ASSERT reaches the handler looking like an ensure: it would take the
// survivable arm, write a report of kind `ensure`, return Continue -- and
// Mosaic's FailFatal would return false WITHOUT aborting, leaving the process
// running past a violated fatal contract. A fatal assert inside an ensure's
// condition must stay fatal, so the condition is evaluated OUTSIDE the scope
// (as a call argument, at the call site) and the scope exists only around the
// failure report, where nothing of the caller's is still running.
//
// Everything else is Mosaic's own semantics, unchanged: `cond` is evaluated
// exactly once as an argument (so an exception from it propagates normally --
// it is never inside the noexcept lambda), `#cond` is stringized at the call
// site, the source_location is captured at the call site, and the failure is
// reported at most once per call site (each expansion is a distinct lambda
// type, hence its own static; the atomic exchange makes concurrent failures at
// one site report exactly once). A PASSING ensure now costs nothing at all --
// no scope, and so no cross-DLL EnsureDepth() call.
#define ARC_ENSURE(cond, msg)                                                          \
    ([](bool arcOk, const char* arcMsg, const std::source_location& arcLoc) noexcept -> bool { \
        if (arcOk) [[likely]] return true;                                             \
        static std::atomic<bool> arcFired{false};                                      \
        if (!arcFired.exchange(true, std::memory_order_relaxed))                       \
        {                                                                              \
            ::Arcane::Assert::EnsureScope arcScope;                                    \
            (void)::Mosaic::detail::FailEnsure(#cond, arcMsg, arcLoc);                 \
        }                                                                              \
        return false;                                                                  \
    }(static_cast<bool>(cond), (msg), std::source_location::current()))
