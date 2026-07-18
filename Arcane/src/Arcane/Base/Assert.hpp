#pragma once

// Engine assert seam (Base module): Arcane's ergonomic ARC_ASSERT/VERIFY/ENSURE
// (thin wrappers over Mosaic's guards, the parallel to ARC_* in Log.hpp) plus the
// Mosaic assert HANDLER that routes a failure through the engine logger. The
// handler is defined in Assert.cpp (once, in Arcane.dll); the installer is inline
// so each module installs into its own per-module Mosaic storage.

#include <Arcane/Base/Api.hpp>

#include <Mosaic/Assert.hpp>

namespace Arcane::Assert
{
    ARCANE_API Mosaic::AssertHandler MosaicHandler() noexcept;

    inline void InstallMosaicHandler() noexcept { Mosaic::SetAssertHandler(MosaicHandler(), nullptr); }
}

// Arcane engine asserts -> Mosaic guards (flow through the installed handler).
#define ARC_ASSERT(cond, msg)  MOSAIC_ASSERT(cond, msg)
#define ARC_VERIFY(cond, msg)  MOSAIC_VERIFY(cond, msg)
#define ARC_ENSURE(cond, msg)  MOSAIC_ENSURE(cond, msg)
