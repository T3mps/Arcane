#pragma once

// =========================================================================
// SCAFFOLDING -- NRI substrate (Phase 1, Task 9): the triangle smoke.
//
// THIS FILE AND ITS .cpp ARE DELETED IN PHASE 2, when the frame graph renders
// real content through the wrapped device. Nothing here is a seam anyone is
// meant to build on: it is a straight-line proof that Tasks 4-8's pieces
// (NriCommon's result discipline, the Graveyard, the creation halves, the
// NriDevice wrap, NriSwapChain's acquire/present/pacing) hold hands and put
// pixels on a screen. It deliberately builds NO reusable abstraction over
// barriers, pipelines or descriptors -- Phase 2's frame graph owns all three,
// and a Phase-1 abstraction would only be something to unbuild.
//
// The two desk commands (Task 10's milestone; GPU/windowed runs are desk-only
// on the dev box, so nothing below has ever executed):
//
//   ArcaneRuntime --nri-smoke --backend dx12   --frames 120 --screenshot nri-tri-dx12.png
//   ArcaneRuntime --nri-smoke --backend vulkan --frames 120 --screenshot nri-tri-vulkan.png
//
// Both should show a clear-coloured window with one vertex-coloured triangle,
// exit 0, and leave a PNG of the last presented frame beside the exe. Any
// validation error on either backend -- D3D12 debug layer through the
// InfoQueue1 callback, VK core + SYNCHRONIZATION validation through the debug
// messenger, or NRI's own validation layer through MakeNriCallbacks -- lands
// in RenderErrorCount and makes the process exit NONZERO. A clean exit code
// is the machine-checkable half of "the substrate works"; the PNG is the
// human half.
//
// Adapted from .example/NRISamples (MIT -- see that tree's LICENSE.txt):
// Source/Triangle.cpp (per-frame acquire -> barrier -> rendering -> draw ->
// barrier -> submit -> present -> trailing signal) and Source/Readback.cpp
// (the COPY_SOURCE barrier pair around CmdReadbackTextureToBuffer, and the
// BGRA-vs-RGBA channel-order check on the mapped result).
// =========================================================================

#include <Arcane/Base/Api.hpp>
#include <Arcane/Host/HostConfig.hpp>

namespace Arcane
{
    namespace NriSmoke
    {
        // Owns the whole process for the duration: its own window, its own
        // native device + NRI wrap, its own swapchain and frame loop. Called
        // from RuntimeApp::Run BEFORE any NVRHI boot -- there is no engine
        // runtime, no plugin, no project and no ImGui in here.
        //
        // Honours `config.backend`, `config.vsync`, `config.maxFrames`
        // (--frames N; 0 = run until the window closes) and
        // `config.screenshotPath` (--screenshot <png>, written from the LAST
        // rendered frame, exactly like the NVRHI host's own --screenshot).
        //
        // Exit codes (documented because a scripted desk run reads them):
        //   0 -- ran to completion with RenderErrorCount unchanged
        //   1 -- setup failed (window/device/wrap/swapchain/shader/pipeline)
        //   2 -- ran, but RenderErrorCount GREW: a validation error fired
        //   3 -- --screenshot was requested and the readback or write failed
        // Precedence 1 > 2 > 3: a setup failure says WHERE the run died, which
        // outranks the errors it produced on the way out, and a validation
        // error explains a bad capture rather than the reverse.
        //
        // The latch is read AFTER every NRI object has been destroyed, so a
        // teardown-only validation error still fails the run.
        ARCANE_API int Run(const HostConfig& config);
    }
}
