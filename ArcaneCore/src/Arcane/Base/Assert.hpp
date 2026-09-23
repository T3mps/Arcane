#pragma once

// Engine assert seam (Base module): Arcane's ergonomic ARC_ASSERT/VERIFY/ENSURE
// (thin wrappers over Mosaic's guards, the parallel to ARC_* in Log.hpp) plus the
// Mosaic assert HANDLER that routes a failure through the engine logger. The
// handler is defined in Assert.cpp (once, in Arcane.dll); the installer is inline
// so each module installs into its own per-module Mosaic storage.

#include <Arcane/Core/Api.hpp>

#include <Mosaic/Assert.hpp>

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

    // Raises EnsureDepth() for the duration of one ARC_ENSURE. Nested (a
    // depth, not a flag) because an ensure's condition may itself call
    // something that ensures.
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

// ARC_ENSURE is MOSAIC_ENSURE inside an EnsureScope -- otherwise identical:
// `cond` is evaluated exactly once (MOSAIC_ENSURE takes it as an ARGUMENT),
// the result is forwarded, and the per-call-site one-shot latch is unchanged.
// noexcept because the handler it may reach runs on a thread that is about to
// write a crash report; a guard is no place to start unwinding.
#define ARC_ENSURE(cond, msg)                                                  \
    ([&]() noexcept -> bool {                                                  \
        ::Arcane::Assert::EnsureScope arcEnsureScope_;                         \
        return MOSAIC_ENSURE(cond, msg);                                       \
    }())
