#pragma once
// RuntimeFrame: RuntimeApp::MainLoop's frame BODY, as free functions.
//
// MainLoop keeps: the pre-loop setup (locals, the render-half boot + window
// reveal), the while/if-else SKELETON that calls the functions below in
// order, and the post-loop ShutdownGraphPath() call. Everything else lives
// here.
//
// THE SHAPE, so a reader can see it rather than reconstruct it:
//   * ONE WINDOW IS PUMPED, always the host's. The render half does not own
//     one -- it borrows this exact window and presents into it.
//   * ONE RESIZE, `io.graph->Resize`, to that window's one presentation
//     surface and to nothing else.
//   * BuildHud IS ONLY THE HUD: the ImGui frame, the "ArcaneRuntime" window
//     and DrawUIAll. PrepareFrame, called straight after it, is the scene
//     resolver's Refresh.
//   * THE FRAME'S EXTENT comes from the graph swapchain (see FrameExtent in
//     the .cpp) -- there is no second surface that could disagree with it.
//   * ImGui CALLS GO THROUGH ImGuiLayer (RenderToDrawData / EndFrameDiscard)
//     instead of bare ImGui::Render()/EndFrame(), so the context pin the
//     layer owns covers them too.
//
// RenderGraph is the only per-frame render function here: the frame graph is
// the only render path.

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>       // dev-only --crash-gpu N (kPassName only; RenderGraph fires through NriDiagnostics::FireFault)
#include <Arcane/Render/Nri/NriGraphContext.hpp>    // the graph vehicle (RenderGraph); unconditional
#include <Arcane/Render/PickEmit.hpp>                // PickDrawable (--pick-probe)


#include <chrono>
#include <cstdint>
#include <vector>

// Forward declaration only: FrameIo just needs a reference to call the one
// public RuntimeApp method RenderGraph reaches (PushSceneCamera). Full
// definition lives in RuntimeApp.hpp, included by RuntimeFrame.cpp (and by
// RuntimeApp.cpp, which already needed it).
class RuntimeApp;

namespace Arcane::RuntimeFrame
{
    // Everything MainLoop's body reaches through `this` or a loop-local,
    // bundled by reference/pointer so the functions below can read and write
    // it without becoming RuntimeApp members. NOTHING here is NEW state:
    // every field is bound, once, in MainLoop to a RuntimeApp member or a
    // MainLoop-scope local. Pointer fields mirror how MainLoop reaches the
    // same object (m_gpu/m_graphContext via `->`, m_resolver/m_runtime/
    // m_plugin likewise); reference fields mirror members it accesses with
    // `.` (m_config, m_perf, the scalar/vector perf locals).
    struct FrameIo
    {
        // ---- the pointers MainLoop already owned ----------------------------
        Arcane::GpuContext*          gpu;       // never null once MainLoop is running
        Arcane::NriGraphContext*     graph;     // never null: m_graphContext is built unconditionally, and MainLoop refuses to enter the frame loop if that failed
        Arcane::SceneRenderResolver* resolver;  // null iff the boot scene published none
        Arcane::Runtime*             runtime;
        Arcane::PluginHost*          plugin;
        // PushSceneCamera is RuntimeApp's own, made public for exactly this
        // call (see RuntimeApp.hpp's comment on it).
        RuntimeApp&                  app;

        // ---- HostConfig& + the perf locals ----------------------------------
        Arcane::HostConfig&    config;
        Arcane::FramePerf&     perf;
        std::uint64_t&         frameCount;
        int&                   graphExit;
        Arcane::GlobalParams&  frameGlobals;
        double&                hostClock;
        double&                lastFrameDt;
        std::vector<Arcane::PickDrawable>& pickDrawables;
        std::vector<std::uint32_t>&        pickSelectedIds;

#if !defined(ARCANE_DIST)
        // --crash-gpu N. Same Dist guard as the RuntimeApp member this is
        // bound to -- see RuntimeApp.hpp. The fired-once latch is the only
        // state needed: RenderGraph fires through the stateless
        // NriDiagnostics::FireFault, which holds nothing between frames.
        bool&                                      gpuFaultFired;
#endif

        // ---- MainLoop-scope locals, hoisted one level up (before the while
        // loop) so more than one of the functions below can see them across a
        // single iteration. ------------------------------------------------
        std::chrono::steady_clock::time_point& simPrev;
        std::chrono::steady_clock::time_point& lastFrameTime;
        std::chrono::steady_clock::time_point& lastShaderPoll;

        // ---- control-flow outputs -------------------------------------------
        // A called free function cannot `break`/`continue` MainLoop's while
        // loop itself, so the two early-exit paths set one of these and
        // return; MainLoop checks it right after the call.
        //
        // quit: set by AdvanceSim on the Pressed("quit") input action -- the
        // one quit path PumpAndResize's own bool return does not cover.
        // MainLoop: `AdvanceSim(io); if (io.quit) break;`.
        bool quit = false;
        // skipFrame: set true by PumpAndResize on a minimized window, which
        // sends MainLoop back to the top of its loop. PrepareFrame only ever
        // clears it. MainLoop checks it after both calls and `continue`s in
        // turn.
        bool skipFrame = false;
    };

    // Heartbeat + device-lost check + this frame's ONE window-event pump +
    // resize handling. Returns true when MainLoop must `break` (device lost,
    // or the window's close was requested) -- see FrameIo::quit's sibling
    // comment. A minimized window sleeps 1ms, sets io.skipFrame and returns
    // false (MainLoop `continue`s).
    //
    // ONE window: the host's. Its resize goes to that window's one
    // presentation surface and to nothing else.
    bool PumpAndResize(FrameIo& io);

    // Per-frame timing (wall dt for input, clamped sim dt), input sampling +
    // the reload_plugin/reload_plugin_fresh actions, and the fixed-step
    // RunLoop::Advance (FixedUpdateAll/UpdateAll) + the audio voice reap.
    // Sets io.quit true on the Pressed("quit") input action.
    void AdvanceSim(FrameIo& io);

    // ImGui BeginFrame + the host's own "ArcaneRuntime" HUD window + the
    // plugin's DrawUIAll. Nothing else. RenderGraph renders the ImGui frame
    // this begins, so every BeginFrame is paired exactly once.
    void BuildHud(FrameIo& io);

    // The scene asset resolver's per-frame Refresh, which is what fills
    // io.frameGlobals. There is no shader hot-reload poll beside it: the
    // render path reads offline artifacts through
    // NriGraphContext::ShaderBytecode and has no ShaderLibrary to poll.
    // io.skipFrame is unconditionally cleared.
    void PrepareFrame(FrameIo& io);

    // The render arm, and the ONLY recorder in the process: the HUD
    // draw-data capture, the 2D submission, the dev-only --crash-gpu fault
    // and --pick-probe collection, the graph FrameDesc, and one
    // NriGraphContext::RenderFrame call. Returns the outcome so MainLoop --
    // the only party that can actually `break`/`continue` its own loop --
    // performs Failed -> break / Skipped -> continue.
    Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io);

    // The frame's tail, reached from RenderGraph: the plugin's debounced
    // hot-reload Poll(), the perf Tick(), the frame counter, and the
    // capture/screenshot bookkeeping on the final frame only. Returns true
    // (`stop`) on the final frame -- MainLoop does
    // `if (CaptureTail(io)) running = false;`, so the while condition, not an
    // explicit break, is what ends the loop.
    bool CaptureTail(FrameIo& io);
}
