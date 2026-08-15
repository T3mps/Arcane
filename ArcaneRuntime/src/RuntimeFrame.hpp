#pragma once
// RuntimeFrame: RuntimeApp::MainLoop's frame BODY, extracted verbatim (NRI
// Phase 3, Task 4 -- mechanical, floor-preserving; see
// docs/superpowers/sdd/2026-08-14-nri-phase3-host-integration/task-4-brief.md
// and this task's own task-4-report.md for the exact line-range mapping).
// Task 6 (the landing) rewrites the frame flow next and needs MainLoop
// reviewable first -- this file is that extraction, with ZERO behavior
// change: every statement below is the old MainLoop body, unchanged, except
// for the mechanical rename `m_foo` -> `io.foo` (these are FREE functions,
// not RuntimeApp members, so they cannot reach `this`) and the control-flow
// substitutions documented on FrameIo::quit / FrameIo::skipFrame (a called
// function cannot literally `break`/`continue` the caller's while loop).
//
// MainLoop keeps: the pre-loop setup (locals, the --nri-graph vehicle boot,
// the golden warm-up), the while/if-else SKELETON that calls these six
// functions in the original order, and the post-loop ShutdownGraphPath()
// call. Everything else lives here.

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>       // dev-only --crash-gpu N (RenderNvrhi)
#include <Arcane/Render/Nri/NriGraphContext.hpp>    // dev-only --nri-graph vehicle (RenderGraph)
#include <Arcane/Render/PickEmit.hpp>                // PickDrawable (--pick-probe)

#include <nvrhi/nvrhi.h>

#include <chrono>
#include <cstdint>
#include <vector>

// Forward declaration only: FrameIo just needs a reference to call the one
// private-turned-public RuntimeApp method (PushSceneCamera) both render arms
// share. Full definition lives in RuntimeApp.hpp, included by
// RuntimeFrame.cpp (and by RuntimeApp.cpp, which already needed it).
class RuntimeApp;

namespace Arcane::RuntimeFrame
{
    // Everything MainLoop's old body reached through `this` or a loop-local,
    // bundled by reference/pointer so the six functions below can read and
    // write it without becoming RuntimeApp members. NOTHING here is NEW
    // state: every field is bound, once, in MainLoop to a RuntimeApp member
    // or a MainLoop-scope local that already existed before this extraction
    // -- see the task-4 report for the one-to-one mapping. Pointer fields
    // mirror how MainLoop already reached the same object (m_gpu/m_graphContext
    // via `->`, m_resolver/m_runtime/m_plugin likewise) so translating a call
    // site is a pure identifier rename, `->` unchanged; reference fields
    // mirror members MainLoop accessed with `.` (m_config, m_perf, the
    // scalar/vector golden+perf locals) for the same reason.
    struct FrameIo
    {
        // ---- the pointers MainLoop already owned ----------------------------
        Arcane::GpuContext*          gpu;       // never null once MainLoop is running
        Arcane::NriGraphContext*     graph;     // null on every ordinary (NVRHI) run
        Arcane::SceneRenderResolver* resolver;  // null iff the boot scene published none
        Arcane::Runtime*             runtime;
        Arcane::PluginHost*          plugin;
        // PushSceneCamera is called from BOTH render arms; RuntimeApp made it
        // public for exactly this (see RuntimeApp.hpp's comment on it).
        RuntimeApp&                  app;

        // ---- HostConfig& + the perf/golden locals ---------------------------
        Arcane::HostConfig&    config;
        Arcane::FramePerf&     perf;
        std::uint64_t&         frameCount;
        int&                   goldenExit;
        int&                   graphExit;
        Arcane::GlobalParams&  frameGlobals;
        double&                hostClock;
        double&                lastFrameDt;
        std::vector<Arcane::PickDrawable>& pickDrawables;
        std::vector<std::uint32_t>&        pickSelectedIds;

#if !defined(ARCANE_DIST)
        // --crash-gpu N (RenderNvrhi only). Same Dist guard as the RuntimeApp
        // members these are bound to -- see RuntimeApp.hpp.
        std::unique_ptr<Arcane::GpuFaultInjector>& gpuFault;
        bool&                                      gpuFaultFired;
#endif

        // ---- MainLoop-scope locals, hoisted one level up (before the while
        // loop) so more than one extracted function can see them across a
        // single iteration -- each used to be re-declared/reset inside the
        // loop body every pass; the reset now happens at the same source
        // location it always did, just written as `io.x = ...` instead of a
        // fresh declaration. ------------------------------------------------
        std::chrono::steady_clock::time_point& simPrev;
        std::chrono::steady_clock::time_point& lastFrameTime;
        std::chrono::steady_clock::time_point& lastShaderPoll;
        nvrhi::ITexture*&                      backbuffer;

        // ---- control-flow outputs -------------------------------------------
        // A called free function cannot `break`/`continue` MainLoop's while
        // loop itself, so the two early-exit paths that used to be a bare
        // `break;`/`continue;` deep inside the old single function now set one
        // of these and return; MainLoop checks it right after the call.
        //
        // quit: set by AdvanceSim on the Pressed("quit") input action -- the
        // one quit path PumpAndResize's own bool return does not cover.
        // MainLoop: `AdvanceSim(io); if (io.quit) break;`.
        bool quit = false;
        // skipFrame: set by PumpAndResize (window minimized) or BuildHud (no
        // NVRHI backbuffer this frame) -- both were a bare `continue;` back to
        // the top of the original while loop. MainLoop checks it after each
        // of those two calls and `continue`s in turn.
        bool skipFrame = false;
    };

    // Heartbeat + device-lost check + this frame's ONE window-event pump +
    // resize handling. Returns true when MainLoop must `break` (device lost,
    // or the window's close was requested) -- see FrameIo::quit's sibling
    // comment. A minimized window sleeps 1ms, sets io.skipFrame and returns
    // false (MainLoop `continue`s).
    bool PumpAndResize(FrameIo& io);

    // Per-frame timing (wall dt for input, clamped sim dt), input sampling +
    // the reload_plugin/reload_plugin_fresh actions, and the fixed-step
    // RunLoop::Advance (FixedUpdateAll/UpdateAll) + the audio voice reap.
    // Sets io.quit true on the Pressed("quit") input action.
    void AdvanceSim(FrameIo& io);

    // ImGui BeginFrame + the host's own "ArcaneRuntime" HUD window + the
    // plugin's DrawUIAll; then (NVRHI-path only) backbuffer acquisition, the
    // once-a-second shader hot-reload poll, and the scene asset resolver's
    // per-frame Refresh -- shared prep BOTH render arms below depend on,
    // since it is what fills io.frameGlobals. Sets io.skipFrame true (and
    // balances the ImGui frame with EndFrame) when the NVRHI swapchain had no
    // backbuffer this frame.
    void BuildHud(FrameIo& io);

    // The NVRHI arm: canvas clear, the dev-only --crash-gpu fault, the
    // batcher recording (PushSceneCamera + SubmitRender + End), the scene
    // post chain + tonemap, the HUD render (or EndFrame on a non-Full golden
    // stage), close + execute + present, the GPU-progress heartbeat. Runs
    // only when io.graph is null (MainLoop's `if (!io.graph)`).
    void RenderNvrhi(FrameIo& io);

    // The --nri-graph arm: the HUD draw-data capture (same ImGui frame the
    // NVRHI arm would have rendered), the CPU-identical 2D submission, the
    // dev-only --pick-probe collection, the graph FrameDesc, and one
    // NriGraphContext::RenderFrame call. Returns the outcome so MainLoop --
    // the only party that can actually `break`/`continue` its own loop --
    // performs Failed -> break / Skipped -> continue exactly as the original
    // inline arm did.
    Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io);

    // The shared tail, reached by both arms: the plugin's debounced
    // hot-reload Poll(), the perf Tick(), the frame counter, and the
    // capture/screenshot/golden bookkeeping on the final frame only. Returns
    // true (`stop`) on the final frame -- MainLoop does
    // `if (CaptureTail(io)) running = false;`, mirroring the original
    // `if (lastFrame) running = false;` exactly (the while condition, not an
    // explicit break, is what ends the loop).
    bool CaptureTail(FrameIo& io);
}
