// RuntimeFrame: RuntimeApp::MainLoop's frame body, extracted verbatim at NRI
// Phase 3 Task 4 and unified at Task 6 (the one-device landing). See
// RuntimeFrame.hpp's header comment for the contract.
//
// RenderNvrhi -- the NVRHI arm this file used to carry beside RenderGraph,
// kept through Phase 5a as the regression floor stage-golden comparisons
// were measured against -- was deleted at NRI Phase 5a, Task 4: the frame
// graph is the only render path left, so there was no floor left to protect.
// RenderGraph below is now the ONLY per-frame render function in this file.

#include "RuntimeFrame.hpp"
#include "RuntimeApp.hpp"   // FrameIo::app.PushSceneCamera -- see that method's comment

#include <Arcane/Assets/Assets.hpp>       // Arcane::ReadTexturePixels (CaptureTail)
#include <Arcane/Audio/AudioDevice.hpp>   // complete type for AudioSystem().Update (AdvanceSim's voice reap)
#include <Arcane/Base/Diagnostics.hpp>    // Diagnostics::Heartbeat (PumpAndResize)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Host/GoldenHarness.hpp>  // Arcane::GoldenArtifact (NRI Phase 3, Task 13: shared with ArcaneEditor -- moved out of this file's anonymous namespace)
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Render/Batcher2D.hpp>              // Arcane::Batch2DStats (BuildHud's HUD text, CaptureTail's perf tick)
#include <Arcane/Render/Device.hpp>                 // Arcane::GraphicsBackend / ToString (BuildHud's HUD text)
#include <Arcane/Render/GpuInstrumentation.hpp>     // Arcane::GpuDeviceLostObserved (PumpAndResize)
#include <Arcane/Render/Nri/NriDiagnostics.hpp>      // dev-only --crash-gpu N on the graph arm (RenderGraph)
#include <Arcane/Render/PickEmit.hpp>                // CollectPickables (RenderGraph's --pick-probe)

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace
{
    // GoldenArtifact (the golden capture/compare tail shared by both render
    // paths) moved to the shared Arcane::Host seam at NRI Phase 3 Task 13
    // (Arcane/Host/GoldenHarness.hpp/.cpp): ArcaneEditor's own golden mode
    // needed the identical machinery, and an anonymous-namespace helper has
    // internal linkage -- it cannot be called across translation units, let
    // alone a second executable. See that header's extraction history and
    // the task-13 report for the diff-match; CaptureTail's call site below
    // needed no change beyond the include (unqualified `GoldenArtifact`
    // still resolves -- ordinary unqualified lookup finds Arcane::
    // GoldenArtifact from this enclosing Arcane::RuntimeFrame namespace).

    // =============================================================
    // THE FRAME'S EXTENT (NRI Phase 3, Task 6).
    // =============================================================
    // Everything that needs a viewport size this frame -- the resolver's
    // material globals, the scene camera's fit, the batcher's Begin -- asks
    // HERE, so the two recorders cannot end up fitted to two different
    // rectangles. It is one rectangle by construction now: there is ONE
    // window, and each mode has exactly one presentation surface tracking it.
    //
    //   graph path : the NRI swapchain's extent. There is no NVRHI canvas at
    //                all on this path, and the graph's own canvas is a
    //                TRANSIENT sized to this swapchain inside BuildFrame --
    //                so the swapchain is the single source of truth, not a
    //                proxy for one.
    //   NVRHI path : the canvas', exactly as before. GpuContext::OnResize
    //                sizes it from the swapchain's ACTUAL extent, which is
    //                what every pre-Task-6 caller of Cnv().Width() was
    //                reading.
    void FrameExtent(const Arcane::RuntimeFrame::FrameIo& io,
                     std::uint32_t& width, std::uint32_t& height)
    {
        if (io.graph)
        {
            width  = io.graph->Swap().Width();
            height = io.graph->Swap().Height();
            return;
        }
        width  = io.gpu->Cnv().Width();
        height = io.gpu->Cnv().Height();
    }
}

namespace Arcane::RuntimeFrame
{

bool PumpAndResize(FrameIo& io)
{
    // FIRST statement in the frame, before anything can block. Mirrors
    // EditorApp::MainLoop -- the two hosts must not diverge on liveness.
    Arcane::Diagnostics::Heartbeat();

    // Device-lost exit: the latch is set only AFTER the gpu-crash report
    // was written, so breaking here is "report captured, stop cleanly" --
    // the alternative was spinning in a Present-fail loop forever.
    // Mirrors EditorApp::MainLoop.
    if (Arcane::GpuDeviceLostObserved())
    {
        ARC_ERROR("GPU device lost -- crash report written; shutting down");
        return true;
    }

    // EXACTLY ONE PUMP PER FRAME, OF THE ONE WINDOW THIS PROCESS HAS
    // (NRI Phase 3, Task 6). Before the landing the graph path owned a
    // SECOND window and this line chose between them -- forced, because
    // DXGI allows only one flip-model swapchain per HWND and an NVRHI
    // swapchain already owned the host's. With no NVRHI device there is
    // no second swapchain and therefore no second window: the graph's
    // swapchain binds THIS one. SDL's event queue is process-wide and
    // Window::PumpEvents drains all of it, so "exactly one pump" was
    // always the rule; now there is only one window it can name.
    Arcane::Window& eventWindow = io.gpu->Win();
    const Arcane::WindowEvents events = eventWindow.PumpEvents();
    if (events.quitRequested) return true;
    if (events.resized)
    {
        // ONE resize, to whichever presentation surface this run has.
        //
        // Strictly BETWEEN frames -- never between the graph's acquire and
        // its present, which both live inside one Execute() call
        // (RgExecuteDesc::swapChain). A frame driver that resizes at the
        // top of its loop satisfies that structurally, and owes nothing
        // else: the graph rebuilds imported-texture views every Execute.
        //
        // TWO RESIDUALS DIED WITH THE SECOND WINDOW. The hidden host
        // window's SetSize lockstep (kept so ImGui_ImplSDL3_NewFrame
        // reported the right DisplaySize) is unnecessary now that the
        // platform backend reads the same window the swapchain binds --
        // one surface, one extent. And GpuContext::OnResize on the graph
        // path resized a hidden NVRHI swapchain that no longer exists; it
        // was the heaviest device-level burst in the frame and it was
        // aimed at a device this process does not create.
        if (io.graph)
            io.graph->Resize(events.width, events.height);
        else
            io.gpu->OnResize(events.width, events.height);
    }
    if (eventWindow.IsMinimized())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        io.skipFrame = true;
        return false;
    }
    io.skipFrame = false;
    return false;
}

void AdvanceSim(FrameIo& io)
{
    io.perf.FrameStart();

    // Input: sample SDL state, evaluate actions. Must precede ImGui BeginFrame
    // so capture flags are set before the evaluator reads them.
    {
        const auto now = std::chrono::steady_clock::now();
        const double wallDt = std::chrono::duration<double>(now - io.lastFrameTime).count();
        io.lastFrameTime = now;
        // Golden runs pin the frame clock too, before m_lastFrameDt/m_hostClock
        // consume it -- see the sim-advance block below for why (deterministic
        // shader inputs).
        const double frameDt = io.config.GoldenMode() ? 1.0 / 60.0 : wallDt;
        // The host clock the compile service debounces against + the frame dt
        // the material globals report. Advanced exactly once per frame, here,
        // because this is where the frame's wall-clock delta is measured (the
        // editor's m_editorClock does the same job in its phase 9).
        io.lastFrameDt = frameDt;
        io.hostClock += frameDt;
        const Arcane::InputSnapshot snap =
            io.gpu->InDevices().Sample(io.gpu->Imgui().WantCaptureKeyboard(),
                                      io.gpu->Imgui().WantCaptureMouse());
        io.runtime->SetInputSnapshot(snap);   // plugins read it via Runtime::Input()
        io.gpu->Input().Update(frameDt, snap);
        if (io.gpu->Input().Pressed("quit"))                { io.quit = true; return; }
        if (io.gpu->Input().Pressed("reload_plugin"))       io.plugin->ForceReload();
        if (io.gpu->Input().Pressed("reload_plugin_fresh")) io.plugin->ReloadFresh();
    }

    // Sim advance: clamp dt, drive RunLoop with plugin callbacks interleaved.
    {
        const auto now = std::chrono::steady_clock::now();
        double simDt = std::chrono::duration<double>(now - io.simPrev).count();
        io.simPrev = now;
        if (simDt > 0.25) simDt = 0.25;
        // Golden runs are deterministic by construction: wall-clock dt would make
        // every animated/timed shader input a per-run variable. One fixed 60 Hz
        // step per rendered frame; --frames N gives N identical steps.
        if (io.config.GoldenMode())
            simDt = 1.0 / 60.0;
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.runtime->Loop().Advance(simDt,
            [&](double dt)          { io.plugin->FixedUpdateAll(dt); },
            [&](double dt, double a){ io.plugin->UpdateAll(dt, a); });
        // Reclaim finished fire-and-forget SFX voices each frame (and, on the
        // headless null backend, advance audio time so one-shots actually end).
        io.runtime->AudioSystem().Update(simDt);
        io.perf.Add(io.perf.accSim, t0, io.perf.Now());
    }
}

void BuildHud(FrameIo& io)
{
    io.gpu->Imgui().BeginFrame();
    {
        ImGui::Begin("ArcaneRuntime");
        // The graph flavor has no RenderDevice to ask (NRI Phase 3, Task 6),
        // so it reads the config instead. The two ALWAYS agree --
        // GpuContext::Create passes exactly this value into
        // RenderDeviceDesc::backend and each RenderDevice subclass returns the
        // constant it was selected for -- so this is the same string either
        // way and the `full` golden's HUD pixels are unchanged. Written as a
        // gate rather than as "just read the config" so the NVRHI arm's own
        // statement is literally the one the baselines were captured with.
        ImGui::Text("Backend: %s", Arcane::ToString(io.graph ? io.config.backend
                                                            : io.gpu->Device().Backend()));
        ImGui::Text("Plugin gen: %u", io.plugin->Generation());
        const Arcane::Batch2DStats s = io.gpu->Batch().Stats();
        ImGui::Text("Quads: %u  Draws: %u", s.quads, s.drawCalls);
        ImGui::End();
    }

    // ABI v2: the game module + any secondary plugins draw their own ImGui between
    // BeginFrame and Render. Each entry point is null-checked inside DrawUIAll.
    io.plugin->DrawUIAll();
}

void PrepareFrame(FrameIo& io)
{
    // The NVRHI backbuffer, acquired only on the NVRHI path -- the graph
    // acquires its own inside Execute(), as late as possible, and never
    // touches this swapchain (which, since Task 6, does not exist on that
    // path at all).
    io.backbuffer = nullptr;
    if (!io.graph)
    {
        io.backbuffer = io.gpu->Swap().BeginFrame();
        if (!io.backbuffer)
        {
            // No backbuffer this frame: still balance the BeginFrame BuildHud
            // just made with an EndFrame so ImGui's assert (double-Begin)
            // doesn't fire next iteration.
            //
            // Deliberately the BARE call, not ImGuiLayer::EndFrameDiscard:
            // this branch is NVRHI-only (it is inside `if (!io.graph)`,
            // unreachable since `io.graph` is unconditional as of Phase 5a
            // Task 2b). It used to be the regression floor RenderNvrhi's
            // baselines were captured against -- NRI Phase 5a, Task 4 deleted
            // RenderNvrhi outright (see this file's header comment), so there
            // is no longer a baseline this statement is keeping faith with;
            // it is simply unreached, dead code, left as-is because deleting
            // it is not forced by anything this task removed (Swapchain
            // survives) and collapsing `if (!io.graph)` guards repo-wide is
            // Task 11's job. The pinned variant is the graph arm's, where the
            // context-theft hazard is the Phase-2 carry Task 6 closed.
            ImGui::EndFrame();
            io.skipFrame = true;
            return;
        }
    }

    // Hot reload: poll shaders once a second; pipeline caches rebuild lazily.
    //
    // NVRHI-ONLY, and by absence rather than by choice: the graph path has no
    // ShaderLibrary (GpuContext's graph flavor builds none). It reads the same
    // offline artifacts through NriGraphContext::ShaderBytecode, from the same
    // ShaderLibrary::ResolveFlavorDir directory -- but caches them for the
    // vehicle's lifetime, because NriPipelineCache's fill contract holds SPANS
    // into those bytes. Hot-reloading fixed-function shaders on the graph path
    // is its own arc; MATERIAL shaders (the ones a designer actually re-saves)
    // hot-reload on both paths through the resolver's Refresh below.
    if (!io.graph)
    {
        const auto now0 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now0 - io.lastShaderPoll).count() >= 1.0)
        {
            io.gpu->Shaders().Poll();
            io.lastShaderPoll = now0;
        }
    }

    // Scene asset resolution, BEFORE the batcher's Begin and before
    // SubmitRender: the drain inside registers compiled materials with the
    // batcher (a pipeline/binding-set table mutation that does not belong
    // inside a recording), and the SpriteTable/SpriteMaterialTable it
    // publishes are what THIS frame's submission reads. Without this call the
    // host published no tables at all, so every sprite fell back to the 1x1 m
    // untextured quad and every material to the plain pipeline -- which is
    // exactly what "the game window draws nothing but the editor viewport
    // looks right" was.
    if (io.resolver)
    {
        std::uint32_t width = 0, height = 0;
        FrameExtent(io, width, height);

        Arcane::SceneRenderResolver::FrameInfo frame;
        frame.now            = io.hostClock;
        frame.dt             = io.lastFrameDt;
        frame.viewportWidth  = (float)width;
        frame.viewportHeight = (float)height;
        io.resolver->Refresh(frame);
        io.frameGlobals = io.resolver->Globals();
    }

    io.skipFrame = false;
}

// =============================================================
// The GRAPH half (--nri-graph), and since NRI Phase 3 Task 6 the ONLY
// recorder in the process when it runs -- there is no NVRHI device.
// One RenderGraph frame: Reset, declare, Compile, Execute -- and
// Execute is what acquires the backbuffer, records every node, submits
// and presents. The frame is batch2d -> [post chain] -> tonemap ->
// [pick/outline] -> [imgui] -> [capture] -> present, declared inside
// DeclareGraphFrame and gated by the same --golden-stage vocabulary
// passed here.
//
// The plugin's RENDER submission runs on BOTH paths as of Task 8: the
// batcher is Begin()'d with no command list (nothing is recorded into
// NVRHI here), SubmitRender fills it exactly as it does above, and the
// graph's Batch2DNode DRAINS it -- one batcher, one batching algorithm,
// two recorders. End() is deliberately NOT called on this path: it is
// the NVRHI recorder, and calling it would need a target this path does
// not have. Since Task 6 that batcher is DEVICE-LESS by construction
// (GpuContext::CreateForGraph), so End() would refuse anyway.
//
// The scene POST CHAIN runs here too as of Task 10, from the same
// compiled bytes the NVRHI chain was built from -- one node per pass,
// between the canvas and the tonemap.
//
// The HUD runs here too as of Task 12, from the same ImGui frame the
// NVRHI path renders -- see the block below.
//
// ImGui INPUT WORKS ON THIS PATH AGAIN (D3 exit, 2026-08-18). It was a
// named gap through the landing: ImGuiLayer::CreateForGraph withheld the
// SDL event tap so a drag could not move the HUD and silently rewrite
// the shared imgui.ini the NVRHI baselines were captured under. That
// stance was time-boxed to desk checkpoint D3b's compares; D3b closed
// green with both golden sets frozen, so InitForGraph installs the tap
// again and the HUD responds. See ImGuiLayer.cpp for the full account.
// (The Phase-2 texture gaps are CLOSED: NriTextureCache makes a span's
// and a material's declared images resident on this device -- Task 2.)
// The SIM half (FixedUpdate/Update) is untouched and runs identically.
// =============================================================
Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io)
{
    Arcane::NriGraphContext::FrameDesc graphFrame;

#if !defined(ARCANE_DIST)
    // --crash-gpu N ON THE GRAPH ARM (NRI Phase 3, Task 5). The same gate,
    // the same "fires exactly once", the same fired-FIRST ordering (a failed
    // build must not retry every frame) and the same ERROR text as the NVRHI
    // arm above -- so `--crash-gpu 30` means one thing on both paths and the
    // desk battery's items read the same either way. It was a documented
    // no-op through Phase 2 (HostConfig.hpp said so); it is live now.
    //
    // TWO things differ, both forced by the recording topology and neither
    // observable in the report:
    //   * the fault goes out on its OWN one-off command buffer rather than
    //     nested inside this frame's recording -- the graph's command buffers
    //     belong to RenderGraph::Execute, which owns their whole open/record/
    //     close/submit cycle, and a deliberate TDR must not be threaded
    //     through the very machinery the crash report has to survive to
    //     describe;
    //   * it therefore fires BEFORE the frame is declared rather than inside
    //     it, which keeps `pass:gpu-fault` the last scope the GPU begins --
    //     exactly what the NVRHI arm achieves by recording it first into a
    //     list submitted at the end of the frame.
    // FireFault is stateless, so io.gpuFault (the nvrhi injector this arm
    // never builds) stays null on this path; only the fired latch is shared.
    if (io.config.crashGpuFrame != 0 && !io.gpuFaultFired &&
        io.frameCount >= io.config.crashGpuFrame)
    {
        io.gpuFaultFired = true;   // set FIRST: a failed build must not retry every frame
        nri::Queue* const queue = io.graph->Device().GraphicsQueue();
        if (!queue || !Arcane::NriDiagnostics::FireFault(io.graph->Device(), *queue))
            ARC_ERROR("--crash-gpu: fault injector unavailable -- nothing dispatched");
    }
#endif

    // ============================================================
    // THE HUD (Task 12) -- and this block is THE parity mechanism,
    // not a convenience. The `full` golden's baseline was captured
    // from the NVRHI path WITH the HUD on it, so the vehicle must
    // draw the SAME content through the SAME code that feeds the
    // NVRHI HUD. It does, structurally: the "ArcaneRuntime" window
    // and the plugin's DrawUIAll are recorded ABOVE this branch, into
    // one ImGui frame both render paths share. Nothing graph-specific
    // is added here -- no "NRI" marker, no extra line -- because any
    // text difference is a golden diff, and nothing on the NVRHI side
    // changed either.
    //
    // The stage split mirrored the NVRHI block's exactly, back when that
    // block existed (NRI Phase 5a, Task 4 deleted RenderNvrhi outright): both
    // non-Full golden stages END the frame without rendering it (host chrome
    // would mask the pixels a stage golden compares), and Full renders it and
    // hands the draw data to the graph. Both go THROUGH THE LAYER rather than
    // calling ImGui::Render()/EndFrame() bare, which is Task 6 closing the
    // Phase-2 carry: ImGuiLayer pins its own context in every entry point,
    // and a bare call here would end whatever context an editor document or
    // a plugin last left current. Either way the frame IS ended, so
    // ImGuiLayer's "every BeginFrame is paired exactly once" contract holds
    // here regardless.
    //
    // The draw data lives in the ImGui context until the next
    // NewFrame, i.e. for the whole of the RenderFrame call below --
    // which is what lets the node copy the vertices at RECORD time.
    // ============================================================
    if (io.config.GoldenMode() &&
        io.config.goldenStage != Arcane::GoldenStage::Full)
    {
        io.gpu->Imgui().EndFrameDiscard();
    }
    else
    {
        graphFrame.imgui = io.gpu->Imgui().RenderToDrawData();
    }

    // THE FRAME'S EXTENT, from the one surface this run has (see
    // FrameExtent): the graph swapchain's. Read once and used for the
    // batcher's viewport AND the scene camera's fit, so those two can
    // never be fitted to different rectangles -- and it is the same
    // number PrepareFrame gave the resolver for the material globals.
    std::uint32_t frameWidth = 0, frameHeight = 0;
    FrameExtent(io, frameWidth, frameHeight);

    // The 2D submission, CPU-identical to the NVRHI block above. The
    // null command list / null framebuffer are what say "record
    // nothing": Batcher2D::Begin only stores them, and every recording
    // use is inside End(), which this path never calls.
    io.gpu->Batch().Begin(nullptr, nullptr, frameWidth, frameHeight);
    io.gpu->Batch().SetGlobals(io.frameGlobals);
    io.app.PushSceneCamera((float)frameWidth, (float)frameHeight);
    {
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.runtime->SetRenderContext(&io.gpu->Batch());
        io.runtime->Loop().SubmitRender();
        io.perf.Add(io.perf.accRec, t0, io.perf.Now());
    }

#if !defined(ARCANE_DIST)
    // --pick-probe (NRI Phase 2, Task 11). THE ONE PLACE the entity ids
    // are produced: CollectPickables is a pure walk of the registry and
    // the k-th drawable it appends IS hit-proxy id k+1, which is what
    // the graph's pick node rasterises and what RuntimeApp::
    // ShutdownGraphPath inverts through PickEntityForId. It lives here,
    // not in the vehicle, because this is the object that owns a
    // registry and a camera -- NriGraphContext is a render vehicle and
    // must not grow either.
    //
    // The view is the SCENE camera PushSceneCamera just installed, so
    // the id pass rasterises the same silhouettes the batch node drew.
    // Since Task 6 they cannot even disagree in principle: both were
    // fitted to frameWidth/frameHeight above, which IS the graph
    // swapchain's extent.
    if (io.config.pickProbe)
    {
        const Arcane::PickView view{ io.runtime->CameraOffset(), io.runtime->CameraZoom() };
        io.pickDrawables.clear();
        Arcane::CollectPickables(io.runtime->Registry(), view, io.pickDrawables);

        // THE SCRIPTED SELECTION -- Task 11's stand-in for an editor
        // selection this host does not have: the FIRST pickable entity
        // in the scene, whose hit-proxy id is 1 by CollectPickables'
        // own ordering contract. Phase 3 replaces exactly these three
        // lines with the editor's real selection and nothing inside the
        // vehicle or the nodes changes.
        io.pickSelectedIds.clear();
        if (!io.pickDrawables.empty())
            io.pickSelectedIds.push_back(1u);

        graphFrame.pickOutline = true;
        graphFrame.pickables   = io.pickDrawables;
        graphFrame.selectedIds = io.pickSelectedIds;
    }
#endif
    // Only read in golden mode (Parse refuses a non-Full stage
    // outside it). Every stage renders the same frame today, because
    // the nodes the other two would add do not exist on this path yet.
    graphFrame.stage = io.config.goldenStage;
    graphFrame.batch = &io.gpu->Batch();
    // The scene post chain, as the BYTES its NVRHI twin was built
    // from (SceneRenderResolver::PostDesc -- NRI Phase 2, Task 10).
    // Re-read every frame for the same reason PostChain() is above:
    // a drain may swap the bound instance under an asset re-save.
    // Null (no PostProcess assignment, or the compile has not landed
    // yet) simply renders canvas -> tonemap.
    //
    // NOT stage-gated here: DeclareGraphFrame applies the same
    // `--golden-stage batch` bypass the NVRHI block above does, so
    // the two paths drop the chain on identical terms.
    graphFrame.post    = io.resolver ? io.resolver->PostDesc() : nullptr;
    // The same GlobalParams the batcher got via SetGlobals, and the
    // same ones the NVRHI post hook passes to the chain -- so the two
    // recorders cannot bind different b1 contents.
    graphFrame.globals = &io.frameGlobals;
    // The capture is taken on the LAST frame, which has to be known
    // BEFORE the frame is declared (the readback is a graph NODE, not
    // an after-the-fact copy). Hence `+ 1`: io.frameCount is bumped in
    // CaptureTail, i.e. after this arm returns Presented.
    const bool willBeLastFrame =
        io.config.maxFrames != 0 && (io.frameCount + 1) >= io.config.maxFrames;
    graphFrame.capture = willBeLastFrame &&
                         (io.config.GoldenMode() || !io.config.screenshotPath.empty());

    // Timed into accPresent rather than left unmeasured, so `--perf`
    // reports something real on this path too (the spec's waiting-
    // frame budget is mostly the pacing wait inside
    // AcquireNextTexture, which is inside this call). It is the whole
    // record + submit + present cost, not just the present: Execute()
    // does all three, and splitting one call across three
    // accumulators would be a fiction. accRec IS real on this path
    // (the submission above is the same work), but accEnd/accTone stay
    // 0 -- the batcher's NVRHI flush and the NVRHI tonemap did not run;
    // their graph counterparts are inside this one call.
    const auto graphT0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
    const Arcane::NriGraphContext::FrameOutcome outcome =
        io.graph->RenderFrame(graphFrame);
    io.perf.Add(io.perf.accPresent, graphT0, io.perf.Now());
    if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
    {
        // Already reported through the "nri-graph" seam (so the latch
        // grew and the exit code would be nonzero anyway) -- stop
        // rather than spin on a broken device.
        ARC_ERROR("the frame could not be recorded or submitted; stopping");
        io.graphExit = 1;
        return outcome;
    }
    if (outcome == Arcane::NriGraphContext::FrameOutcome::Skipped)
    {
        // Routine: a zero-sized surface or an OUT_OF_DATE acquire,
        // both self-healing on the next resize event. m_frameCount
        // deliberately does NOT advance -- the vehicle's own frame
        // counter did not either, and the two must stay in lockstep
        // for the command-slot recycling to be safe.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return outcome;
    }
    return outcome;
}

bool CaptureTail(FrameIo& io)
{
    // Debounced watcher: rebuild PlaygroundGame -> auto hot reload.
    {
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.plugin->Poll();
        io.perf.Add(io.perf.accPoll, t0, io.perf.Now());
    }

    if (io.perf.On())
    {
        const Arcane::Batch2DStats bs = io.gpu->Batch().Stats();
        io.perf.Tick(bs.quads, bs.drawCalls);
    }

    ++io.frameCount;
    const bool lastFrame = io.config.maxFrames != 0 && io.frameCount >= io.config.maxFrames;

    // The capture tail. Both render paths reach it, on the final frame
    // only, AFTER the present -- so the pixels are the ones that were
    // actually shown and the capture cannot race the recording that
    // produced it. The ONLY difference between the paths is where the
    // pixels come from; everything downstream (artifact naming, the
    // comparator, the tolerances, the exit code) is shared.
    if (lastFrame && (!io.config.screenshotPath.empty() || io.config.GoldenMode()))
    {
        // LATCHED BEFORE THE READBACK, not after (whole-branch review, I2;
        // exactly the editor's rule at CaptureEditorGolden): this is the
        // "did the run ever reach a real capture" signal MainLoop's post-loop
        // check reads, and it must be true even when the readback then fails
        // -- that failure sets goldenExit = 3 itself, below, and the two are
        // independent facts. Every path out of the loop that never reaches
        // here (a window close, the quit action, a --frames count the run
        // never got to) therefore leaves it false, which is precisely the
        // "PASS that compared nothing" this closes.
        if (io.config.GoldenMode())
            io.goldenCaptured = true;

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> actual;
        const bool read =
            io.graph
                // The graph's readback NODE ran inside this frame's own
                // command buffer (declared up front, because a graph
                // capture is a node and not an afterthought) --
                // ReadCapture maps it and normalizes BGRA -> RGBA, since
                // NRI resolves the swapchain's channel order rather than
                // letting us pin it.
                ? io.graph->ReadCapture(w, h, actual)
                : Arcane::ReadTexturePixels(io.gpu->Device().Nvrhi(), io.backbuffer, w, h, actual);

        if (!read)
        {
            // Only a GOLDEN run fails on this. A --screenshot-only run has
            // always degraded to a warning (a screenshot that cannot be
            // written must not trip the GPU tests' RenderErrorCount()==0
            // gate -- SaveTexturePng's own contract), and folding the two
            // readbacks into one must not quietly change that.
            if (io.config.GoldenMode())
            {
                ARC_ERROR("golden: backbuffer readback failed");
                io.goldenExit = 3;
            }
            else
            {
                ARC_WARN("screenshot FAILED: {} (backbuffer readback)", io.config.screenshotPath);
            }
        }
        else
        {
            // --screenshot: the exact pixels a player sees (post-tonemap,
            // post-ImGui). Kept a WARN rather than an exit code, exactly
            // as before -- only the golden harness fails a run.
            if (!io.config.screenshotPath.empty())
            {
                if (Arcane::WritePngRgba(io.config.screenshotPath, w, h, actual.data()))
                    ARC_INFO("screenshot written: {}", io.config.screenshotPath);
                else
                    ARC_WARN("screenshot FAILED: {}", io.config.screenshotPath);
            }
            // NRI Phase 0 golden harness, unchanged in behaviour and now
            // shared by both paths -- see GoldenArtifact.
            if (io.config.GoldenMode() && GoldenArtifact(io.config, w, h, actual) != 0)
                io.goldenExit = 3;
        }
    }

    return lastFrame;
}

}
