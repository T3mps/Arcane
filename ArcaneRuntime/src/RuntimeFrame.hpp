#pragma once
// RuntimeFrame: RuntimeApp::MainLoop's frame BODY, as free functions.
//
// MainLoop keeps: the pre-loop setup (locals, the render-half boot + window
// reveal), the while/if-else SKELETON that calls the functions below in
// order, and the post-loop ShutdownGraphPath() call. Everything else lives
// here.
//
// THE SHAPE, so a reader can see it rather than reconstruct it:
//   * ONE WINDOW IS PUMPED, always the host's. Windowed, the render half
//     borrows this exact window and presents into it; under --offscreen the
//     window exists but is never shown, and the pump keeps running because
//     ImGui and the input stack are wired to it either way.
//   * ONE RESIZE, `io.graph->Resize`, to that window's one presentation
//     surface and to nothing else -- and only in HOST-WINDOW MODE, because
//     that is the only mode with a surface to resize. See PumpAndResize.
//   * BuildHud IS ONLY THE HUD: the ImGui frame, the "ArcaneRuntime" window
//     and DrawUIAll. PrepareFrame, called straight after it, is the scene
//     resolver's Refresh.
//   * THE FRAME'S EXTENT comes from the vehicle's own surface -- the
//     swapchain's, or the offscreen output's, read mode-agnostically through
//     SurfaceWidth/SurfaceHeight (see FrameExtent in the .cpp). There is no
//     second surface that could disagree with it in either mode.
//   * ImGui CALLS GO THROUGH ImGuiLayer (RenderToDrawData / EndFrameDiscard)
//     instead of bare ImGui::Render()/EndFrame(), so the context pin the
//     layer owns covers them too.
//
// RenderGraph is the only per-frame render function here: the frame graph is
// the only render path. It is the ONE function that still names the mode, at
// exactly one line -- the vehicle's RenderFrame/RenderFrameOffscreen pair
// refuses each other's mode by design, so the frame driver has to pick.

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
    // every field is bound, once, in MainLoop to a RuntimeApp member, a
    // MainLoop-scope local, or -- for `graph` -- the vehicle
    // RuntimeApp::Graph() resolved out of the two mutually exclusive members
    // that could hold one. Pointer fields mirror how MainLoop reaches the
    // same object (m_gpu via `->`, the resolved graph likewise, m_resolver/
    // m_runtime/m_plugin the same); reference fields mirror members it
    // accesses with `.` (m_config, m_perf, the scalar/vector perf locals).
    struct FrameIo
    {
        // ---- the pointers MainLoop already owned ----------------------------
        Arcane::GpuContext*          gpu;       // never null once MainLoop is running
        // THE LIVE VEHICLE, and never null: MainLoop builds exactly one of
        // m_graphContext (windowed) / m_offscreen (--offscreen) and RETURNS
        // before the frame loop if that build failed, so by the time any
        // function below runs there is a vehicle. Which of the two it is, is
        // resolved once at the binding site (RuntimeApp::Graph()) -- the frame
        // body does not branch on --offscreen to find its graph, and asks
        // IsOffscreen() only where the vehicle's own API genuinely differs.
        Arcane::NriGraphContext*     graph;
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

        // THIS FRAME'S MESH INSTANCES (F2a Task 10) -- the same "member, not
        // a RenderGraph local" reasoning as pickDrawables above: MeshSceneDesc
        // ::instances is BORROWED for the duration of the RenderFrame call
        // (MeshNode.hpp), and RenderGraph is a free function with nothing of
        // its own to hold that storage, so it has to be a RuntimeApp member
        // reached through here. Rebuilt every frame by CollectMeshInstances,
        // which clears it on entry (MeshSubmissionSystem.hpp) -- so, again
        // like pickDrawables, a steady-state frame with a static scene
        // allocates nothing after the first.
        std::vector<Arcane::MeshInstance>& meshInstances;

        // ---- THE LAST-FRAME CAPTURE (Task 8: --report wiring) ---------------
        // CaptureTail fills these on the run's LAST frame whenever --screenshot
        // or --report was requested (see that function's own comment on the
        // widened read gate) -- the SAME readback --screenshot already takes,
        // just retained here instead of written straight to a PNG and
        // discarded, because a report's Brightness/Luma/Rgba probes need the
        // identical pixels. RuntimeApp::ShutdownGraphPath is where they are
        // consumed, once the loop has ended and backend/frameCount/exitReason
        // are all settled -- see that method's own comment for why the WRITE
        // happens there rather than here.
        //
        // captureRead stays at its bound member's default (false) on any run
        // that broke out early (device lost, render failure, an interactive
        // quit) or that asked for neither flag; a VerifyReport built from that
        // simply never calls SetCapture rather than reporting a phantom frame.
        bool&                       captureRead;
        std::uint32_t&              captureWidth;
        std::uint32_t&              captureHeight;
        std::vector<unsigned char>& captureRgba;

        // ---- --settle N (Task 10) --------------------------------------------
        // The PREVIOUS settle attempt's capture -- kept only long enough to
        // compare against the NEXT one (CaptureTail's byte-equal check) -- plus
        // the attempt budget's running state. Distinct storage from
        // captureRead/captureWidth/captureHeight/captureRgba just above: those
        // four hold the FINAL, agreed-on capture (only ever written once
        // convergence -- or the ordinary Task 8 single-shot capture -- has
        // actually happened); these hold the WORKING comparison baseline, which
        // churns every attempt until then and is never itself a claim about
        // anything converged.
        //
        // previousCaptureValid stays false until the FIRST settle attempt lands
        // a readback, so the very first attempt (which has nothing yet to
        // compare against) can never spuriously "match".
        std::vector<unsigned char>& previousCaptureRgba;
        std::uint32_t&              previousCaptureWidth;
        std::uint32_t&              previousCaptureHeight;
        bool&                       previousCaptureValid;
        // How many settle attempts have been taken so far (bounded by
        // HostConfig::settleAttempts) and whether one of them converged.
        // settleConverged is what RuntimeApp::ShutdownGraphPath reads to decide
        // exitReason "settle-not-converged" and the process exit code -- see
        // that method's comment. Both stay at their defaults (0/false) on a run
        // that never asked for --settle at all, which must never read as a
        // failure.
        std::uint32_t&              settleAttemptsUsed;
        bool&                       settleConverged;

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
    // presentation surface and to nothing else -- and under --offscreen there
    // IS no such surface, so the resize is dropped rather than forwarded.
    bool PumpAndResize(FrameIo& io);

    // Per-frame timing (wall dt for input, clamped sim dt), input sampling +
    // the reload_plugin/reload_plugin_fresh actions, and the fixed-step
    // RunLoop::Advance (FixedUpdateAll/UpdateAll) + the audio voice reap.
    // Sets io.quit true on the Pressed("quit") input action.
    //
    // --settle N (Task 10): once io.frameCount reaches the configured
    // --frames budget, this FREEZES frameDt/simDt at 0 rather than advancing
    // them -- see the .cpp's own comment for why that is load-bearing (a
    // Time-driven material would otherwise never let CaptureTail's
    // byte-equal comparison converge on anything).
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
    //
    // --settle N (Task 10): "the final frame only" above describes the
    // ordinary (non-settle) tail unchanged from Task 8. When
    // io.config.settleAttempts != 0, this instead keeps returning false
    // (keep going) once the ordinary --frames budget is reached, one more
    // settle attempt per call, until two consecutive captures compare
    // byte-equal (converged: true is returned, the captured pixels are
    // published the same way Task 8's tail does) or the attempt budget is
    // spent (non-convergence: true is returned too -- the loop MUST still
    // stop -- but io.captureRead is deliberately left false and no
    // --screenshot is written, so nothing downstream can mistake an
    // unconverged run for a converged one).
    bool CaptureTail(FrameIo& io);
}
