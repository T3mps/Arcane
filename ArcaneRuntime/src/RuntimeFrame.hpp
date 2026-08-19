#pragma once
// RuntimeFrame: RuntimeApp::MainLoop's frame BODY, extracted verbatim at NRI
// Phase 3 Task 4 (mechanical, floor-preserving) and UNIFIED at Task 6 -- the
// landing, where `--nri-graph` became one device and one window. The task
// brief and report that carried the exact line-range mapping were EPHEMERAL
// SDD artifacts (gitignored, deleted at phase close, 2026-08-18); the phase's
// surviving account is the milestone record at the tail of
// docs/plans/2026-08-14-nri-phase3-host-integration.md.
//
// MainLoop keeps: the pre-loop setup (locals, the --nri-graph render-half
// boot + window reveal, the golden warm-up), the while/if-else SKELETON that
// calls the functions below in order, and the post-loop ShutdownGraphPath()
// call. Everything else lives here.
//
// WHAT TASK 6 CHANGED IN THIS FILE, so a reader can see the shape rather than
// reconstruct it from a diff:
//   * ONE WINDOW IS PUMPED, always the host's. The graph half no longer owns
//     one -- it borrows this exact window and presents into it.
//   * ONE RESIZE. `io.graph->Resize` alone on the graph path; the hidden-
//     window SetSize lockstep and the GpuContext::OnResize residual that used
//     to accompany it both described the two-window topology and are gone.
//   * BuildHud SPLIT IN TWO (it used to also acquire the NVRHI backbuffer,
//     which its name did not say): BuildHud is now the ImGui frame + the HUD +
//     DrawUIAll, and PrepareFrame is the NVRHI backbuffer acquisition, the
//     shader hot-reload poll and the scene resolver's Refresh. Called back to
//     back, in that order -- the statement order is exactly what it was.
//   * THE FRAME'S EXTENT comes from whichever surface the run has (see
//     FrameExtent in the .cpp). On the graph path there is no NVRHI canvas to
//     ask.
//   * THE GRAPH ARM'S ImGui CALLS GO THROUGH ImGuiLayer (RenderToDrawData /
//     EndFrameDiscard) instead of bare ImGui::Render()/EndFrame(), so the
//     context pin the layer owns covers them too -- the Phase-2 carry.
//
// NRI Phase 5a, Task 4: the DEFAULT NVRHI arm (RenderNvrhi), kept until now as
// the regression floor the whole phase was measured against, is DELETED --
// the frame graph is the only render path left, so there is nothing left to
// float a floor under. RenderGraph is the only per-frame render function in
// this file now.

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>       // dev-only --crash-gpu N (kPassName only; RenderGraph fires through NriDiagnostics::FireFault)
#include <Arcane/Render/Nri/NriGraphContext.hpp>    // the graph vehicle (RenderGraph); unconditional as of Phase 5a
#include <Arcane/Render/PickEmit.hpp>                // PickDrawable (--pick-probe)


#include <chrono>
#include <cstdint>
#include <vector>

// Forward declaration only: FrameIo just needs a reference to call the one
// private-turned-public RuntimeApp method (PushSceneCamera) RenderGraph calls
// (both render arms shared it, before NRI Phase 5a, Task 4 deleted
// RenderNvrhi). Full definition lives in RuntimeApp.hpp, included by
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
        Arcane::NriGraphContext*     graph;     // never null: m_graphContext is built unconditionally before the frame loop starts (Phase 5a, Task 2b's boot refusal)
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
        // "Did this golden run ever actually reach a capture" -- latched by
        // CaptureTail on the one frame that attempts one, read ONCE by MainLoop
        // after the loop (whole-branch review, I2). See RuntimeApp::
        // m_goldenCaptured for what the two facts are and why they are separate.
        bool&                  goldenCaptured;
        int&                   graphExit;
        Arcane::GlobalParams&  frameGlobals;
        double&                hostClock;
        double&                lastFrameDt;
        std::vector<Arcane::PickDrawable>& pickDrawables;
        std::vector<std::uint32_t>&        pickSelectedIds;

#if !defined(ARCANE_DIST)
        // --crash-gpu N. Same Dist guard as the RuntimeApp member this is bound
        // to -- see RuntimeApp.hpp. The `gpuFault` reference that used to sit
        // here went with the injector object itself at NRI Phase 5a Task 8b:
        // it had been unused since RenderNvrhi's deletion (Task 4), because
        // RenderGraph's FireFault is stateless and never built one.
        // `gpuFaultFired` stays live -- both arms shared the ONE fired-once
        // latch, and the surviving arm still needs it.
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
        // An `nvrhi::ITexture*& backbuffer` sat here until NRI Phase 5a,
        // Task 9.5a. PrepareFrame reset it to null every frame and nothing
        // read it: its last reader, CaptureTail's NVRHI arm, went at Task 4
        // and the acquire that filled it at Task 6.

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
        // skipFrame: set true by PumpAndResize on a minimized window (a bare
        // `continue;` back to the top of the original while loop). It used to
        // also be set by PrepareFrame on "no NVRHI backbuffer this frame";
        // that arm is gone (NRI Phase 5a, Task 6 deleted GpuContext::Swap()),
        // so PrepareFrame now only ever sets this false. MainLoop still
        // checks it after both calls and `continue`s in turn.
        bool skipFrame = false;
    };

    // Heartbeat + device-lost check + this frame's ONE window-event pump +
    // resize handling. Returns true when MainLoop must `break` (device lost,
    // or the window's close was requested) -- see FrameIo::quit's sibling
    // comment. A minimized window sleeps 1ms, sets io.skipFrame and returns
    // false (MainLoop `continue`s).
    //
    // ONE window since Task 6, on both paths: the host's. Its resize goes to
    // whichever presentation surface this run has and to nothing else.
    bool PumpAndResize(FrameIo& io);

    // Per-frame timing (wall dt for input, clamped sim dt), input sampling +
    // the reload_plugin/reload_plugin_fresh actions, and the fixed-step
    // RunLoop::Advance (FixedUpdateAll/UpdateAll) + the audio voice reap.
    // Sets io.quit true on the Pressed("quit") input action.
    void AdvanceSim(FrameIo& io);

    // ImGui BeginFrame + the host's own "ArcaneRuntime" HUD window + the
    // plugin's DrawUIAll. Nothing else -- the backbuffer acquisition this used
    // to carry (and not say so) is PrepareFrame's, below. Both arms render the
    // ImGui frame this begins, so every BeginFrame is still paired exactly
    // once whichever way the frame goes.
    void BuildHud(FrameIo& io);

    // What is left of the shared prep after NRI Phase 5a, Task 6: the scene
    // asset resolver's per-frame Refresh, which is what fills
    // io.frameGlobals. Used to also carry an NVRHI-only backbuffer acquire
    // and an NVRHI-only once-a-second shader hot-reload poll (the graph path
    // reads offline artifacts through NriGraphContext::ShaderBytecode and has
    // no ShaderLibrary to poll); both were already unreachable and are gone
    // now that GpuContext builds no Swapchain or ShaderLibrary at all.
    // io.backbuffer is unconditionally reset to null; io.skipFrame is
    // unconditionally cleared.
    void PrepareFrame(FrameIo& io);

    // The --nri-graph arm, which since Task 6 is the ONLY recorder in the
    // process when it runs: the HUD draw-data capture (same ImGui frame the
    // NVRHI arm would have rendered), the CPU-identical 2D submission, the
    // dev-only --crash-gpu fault and --pick-probe collection, the graph
    // FrameDesc, and one NriGraphContext::RenderFrame call. Returns the
    // outcome so MainLoop --
    // the only party that can actually `break`/`continue` its own loop --
    // performs Failed -> break / Skipped -> continue exactly as the original
    // inline arm did.
    Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io);

    // The shared tail (both arms reached it before NRI Phase 5a, Task 4
    // deleted RenderNvrhi; RenderGraph is the only caller left): the plugin's
    // debounced hot-reload Poll(), the perf Tick(), the frame counter, and
    // the capture/screenshot/golden bookkeeping on the final frame only. Returns
    // true (`stop`) on the final frame -- MainLoop does
    // `if (CaptureTail(io)) running = false;`, mirroring the original
    // `if (lastFrame) running = false;` exactly (the while condition, not an
    // explicit break, is what ends the loop).
    bool CaptureTail(FrameIo& io);
}
