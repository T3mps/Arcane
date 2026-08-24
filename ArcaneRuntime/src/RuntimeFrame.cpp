// RuntimeFrame: RuntimeApp::MainLoop's frame body. See RuntimeFrame.hpp's
// header comment for the contract.

#include "RuntimeFrame.hpp"
#include "RuntimeApp.hpp"   // FrameIo::app.PushSceneCamera -- see that method's comment

#include <Arcane/Assets/Assets.hpp>       // Arcane::WritePngRgba (CaptureTail's screenshot write), reached via Assets.hpp -> ImageIo.hpp
#include <Arcane/Audio/AudioDevice.hpp>   // complete type for AudioSystem().Update (AdvanceSim's voice reap)
#include <Arcane/Base/Assert.hpp>         // ARC_ASSERT (FrameExtent's io.graph invariant)
#include <Arcane/Base/Diagnostics.hpp>    // Diagnostics::Heartbeat (PumpAndResize)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Render/Batcher2D.hpp>              // Arcane::Batch2DStats (BuildHud's HUD text, CaptureTail's perf tick)
#include <Arcane/Render/GraphicsBackend.hpp>                 // Arcane::GraphicsBackend / ToString (BuildHud's HUD text)
#include <Arcane/Render/GpuInstrumentation.hpp>     // Arcane::GpuDeviceLostObserved (PumpAndResize)
#include <Arcane/Render/Nri/NriDiagnostics.hpp>      // dev-only --crash-gpu N (RenderGraph)
#include <Arcane/Render/PickEmit.hpp>                // CollectPickables (RenderGraph's --pick-probe)
#include <Arcane/Scene/MeshSubmissionSystem.hpp>     // CollectMeshInstances (RenderGraph's opaque 3D pass, F2a Task 10)
#include <Arcane/Scene/SceneCamera.hpp>              // ActivePerspectiveSceneCamera (the SAME guarded path MeshSceneDesc's comment requires)

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace
{
    // =============================================================
    // THE FRAME'S EXTENT.
    // =============================================================
    // Everything that needs a viewport size this frame -- the resolver's
    // material globals, the scene camera's fit, the batcher's Begin -- asks
    // HERE, so no two of them can end up fitted to different rectangles. It is
    // one rectangle by construction: the vehicle has exactly ONE target, and
    // the graph's own canvas is a TRANSIENT sized to that target inside
    // BuildFrame -- so the target is the single source of truth, not a proxy
    // for one. Windowed that target is the swapchain over the host's one
    // window; under --offscreen it is the vehicle's persistent output texture.
    //
    // READ MODE-AGNOSTICALLY, and that is not a style choice: this used to be
    // `io.graph->Swap().Width()/Height()`, and an offscreen context HAS NO
    // SWAPCHAIN, so that read would have faulted on frame 1 of every
    // --offscreen run. SurfaceWidth/SurfaceHeight are the accessors
    // NriGraphContext documents as "the mode-agnostic reading of
    // Swap().Width()/Height(), which an offscreen context cannot answer" --
    // the same pair PumpAndResize below already reads for its before/after
    // extent report.
    //
    // ASSERT-THEN-READ, not `if (io.graph) ... else fail`. `io.graph` is
    // unconditional: MainLoop refuses to reach the frame loop at all if the
    // vehicle -- either vehicle -- could not be created (RuntimeApp.cpp,
    // ShutdownGraphPath runs and MainLoop returns first), so every call into
    // RuntimeFrame's functions has a live vehicle -- which is why RenderGraph
    // and CaptureTail in this same file dereference it with no guard. The
    // fail-loud stands hoisted above the read rather than left as an
    // unreachable tail.
    //
    // The assert is not decoration. A 0x0 frame would propagate into the
    // resolver's material globals and the batcher's viewport as a quiet
    // wrong-answer instead of a loud one.
    void FrameExtent(const Arcane::RuntimeFrame::FrameIo& io,
                     std::uint32_t& width, std::uint32_t& height)
    {
        ARC_ASSERT(io.graph, "RuntimeFrame::FrameExtent: io.graph is null -- "
                             "the graph vehicle should be unconditional by the "
                             "time the frame loop runs");
        width  = io.graph->SurfaceWidth();
        height = io.graph->SurfaceHeight();
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

    // EXACTLY ONE PUMP PER FRAME, OF THE ONE WINDOW THIS PROCESS HAS.
    // There is one graphics device, and AT MOST one swapchain: windowed, it
    // binds THIS window -- DXGI allows only one flip-model swapchain per
    // HWND, so a second presentation surface would force a second window
    // and this line would have to choose between them. Under --offscreen
    // there is no swapchain at all, and the window below is never mapped --
    // but it still EXISTS and is still the object ImGui's platform backend
    // and the input stack were initialised against, so it is still pumped,
    // and it is still the only one there is. SDL's event queue is
    // process-wide and Window::PumpEvents drains all of it, so "exactly one
    // pump" is the rule in both modes, and there is one window to name.
    Arcane::Window& eventWindow = io.gpu->Win();
    const Arcane::WindowEvents events = eventWindow.PumpEvents();
    if (events.quitRequested) return true;
    // A RESIZE IS A PRESENTATION EVENT, AND --offscreen HAS NO PRESENTATION.
    //
    // NriGraphContext::Resize is documented HOST-WINDOW MODE ONLY and refuses
    // an offscreen context through the LATCHED error seam -- which is a grown
    // RenderErrorCount and therefore a nonzero exit for the whole run, not a
    // warning. So this must be gated rather than left to "a hidden window
    // never resizes": SDL posts SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED (the one
    // event Window::PumpEvents turns into `resized`) when a window's pixel
    // size is first established and again on a display-scale change, and
    // NEITHER requires the window to have been mapped. The offscreen output's
    // extent is fixed at Create and only ResizeOffscreen can change it, so
    // there is nothing here to forward the event to.
    if (events.resized && !io.graph->IsOffscreen())
    {
        // ONE resize, to this run's one presentation surface.
        //
        // Strictly BETWEEN frames -- never between the graph's acquire and
        // its present, which both live inside one Execute() call
        // (RgExecuteDesc::swapChain). A frame driver that resizes at the
        // top of its loop satisfies that structurally, and owes nothing
        // else: the graph rebuilds imported-texture views every Execute.
        //
        // NOTHING ELSE NEEDS RESIZING ALONGSIDE IT. The ImGui platform
        // backend reads the same window the swapchain binds, so this one
        // call leaves every consumer on one extent -- there is no second
        // surface to hold in lockstep and no device-level resize burst.
        //
        // ===== A RESIZE IS NEVER SILENT =====================================
        // This host CAPTURES ITS SWAPCHAIN, so a --screenshot artifact carries
        // the window's client extent -- which the OS owns, not us. Following a
        // resize without saying so once produced a run that started at 1280x720
        // and wrote a 1280x712 image with nothing in the log to explain it.
        //
        // The extent is deliberately NOT pinned: Vulkan's surface currentExtent
        // is authoritative, and refusing to follow it produces a
        // surface/swapchain mismatch -- trading a legible outcome for an
        // illegible one. Report it instead.
        const std::uint32_t wasW = io.graph->SurfaceWidth();
        const std::uint32_t wasH = io.graph->SurfaceHeight();
        io.graph->Resize(events.width, events.height);
        if (wasW != events.width || wasH != events.height)
        {
            if (!io.config.screenshotPath.empty())
            {
                ARC_WARN("[nri-graph] the window was RESIZED mid-run: {}x{} -> {}x{}. This run "
                         "captures its swapchain, so the screenshot will carry the NEW extent.",
                         wasW, wasH, events.width, events.height);
            }
            else
            {
                ARC_INFO("[nri-graph] window resized: {}x{} -> {}x{}",
                         wasW, wasH, events.width, events.height);
            }
        }
    }
    // Left ungated for --offscreen, deliberately: IsMinimized reads
    // SDL_WINDOW_MINIMIZED, which a never-shown window does not carry (hidden
    // and minimized are different SDL window states), so this is false for the
    // whole of an offscreen run and the skip below is unreachable there. It
    // would be harmless even if it fired -- a skipped frame is routine -- so
    // there is nothing to gate.
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
        const double frameDt = wallDt;
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
        // There is no RenderDevice to ask -- GpuContext builds none -- so
        // the HUD reads the backend out of the config it was selected with.
        ImGui::Text("Backend: %s", Arcane::ToString(io.config.backend));
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
// THE RENDER HALF -- the only recorder in the process.
// One RenderGraph frame: Reset, declare, Compile, Execute -- and
// Execute is what acquires the backbuffer, records every node, submits
// and presents. The frame is batch2d -> [post chain] -> tonemap ->
// [pick/outline] -> [imgui] -> [capture] -> present, declared inside
// DeclareGraphFrame; each bracketed stage is dropped by passing the
// FrameDesc field that carries it as null (see that struct).
//
// UNDER --offscreen the frame's SHAPE is identical -- same declaration,
// same node census -- and only its two ends differ: the tonemap writes
// the vehicle's imported output instead of an acquired backbuffer, and
// Execute runs with no swapchain, so it acquires nothing and presents
// nothing. That is the vehicle's business, not this function's; the one
// thing this function owes it is calling the right member of the
// RenderFrame/RenderFrameOffscreen pair. See that call below.
//
// The plugin's RENDER submission: the batcher is Begin()'d with the
// canvas extent alone, SubmitRender fills it, and the graph's
// Batch2DNode DRAINS it -- one batcher, one batching algorithm, one
// recorder. End() is deliberately NOT called: that batcher is
// DEVICE-LESS by construction (GpuContext::Create builds no render
// device), so End() would refuse anyway and records nothing.
//
// The scene POST CHAIN runs here too -- one node per pass, between the
// canvas and the tonemap. So does the HUD, from the same ImGui frame
// BuildHud left current -- see the block below.
//
// NriTextureCache makes a span's and a material's declared images
// resident on this device, so sprites and materials sample the textures
// they name rather than a fallback.
// The SIM half (FixedUpdate/Update) runs above, in AdvanceSim.
// =============================================================
Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io)
{
    Arcane::NriGraphContext::FrameDesc graphFrame;

#if !defined(ARCANE_DIST)
    // --crash-gpu N. Fires exactly once, and the latch is set FIRST -- a
    // failed build must not retry the injection every frame.
    //
    // TWO properties, both forced by the recording topology and neither
    // observable in the report:
    //   * the fault goes out on its OWN one-off command buffer rather than
    //     nested inside this frame's recording -- the graph's command buffers
    //     belong to RenderGraph::Execute, which owns their whole open/record/
    //     close/submit cycle, and a deliberate TDR must not be threaded
    //     through the very machinery the crash report has to survive to
    //     describe;
    //   * it therefore fires BEFORE the frame is declared rather than inside
    //     it, which keeps `pass:gpu-fault` the last scope the GPU begins.
    // FireFault is stateless -- it owns its objects for the length of one
    // dispatch -- so FrameIo carries nothing for it but the fired latch.
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
    // THE HUD. The "ArcaneRuntime" window and the plugin's DrawUIAll
    // are recorded ABOVE, into the one ImGui frame this host builds;
    // this line only hands the finished draw data to the graph.
    //
    // It goes THROUGH THE LAYER rather than calling ImGui::Render()/
    // EndFrame() bare: ImGuiLayer pins its own context in every entry
    // point, and a bare call here would end whatever context an editor
    // document or a plugin last left current. The frame IS ended either
    // way, so ImGuiLayer's "every BeginFrame is paired exactly once"
    // contract holds here regardless.
    //
    // The draw data lives in the ImGui context until the next
    // NewFrame, i.e. for the whole of the RenderFrame call below --
    // which is what lets the node copy the vertices at RECORD time.
    // ============================================================
    graphFrame.imgui = io.gpu->Imgui().RenderToDrawData();

    // THE FRAME'S EXTENT, from the one surface this run has (see
    // FrameExtent): the graph swapchain's, or -- under --offscreen -- the
    // vehicle's persistent output texture. Read once and used for the
    // batcher's viewport AND the scene camera's fit, so those two can
    // never be fitted to different rectangles -- and it is the same
    // number PrepareFrame gave the resolver for the material globals.
    std::uint32_t frameWidth = 0, frameHeight = 0;
    FrameExtent(io, frameWidth, frameHeight);

    // The 2D submission. Begin takes only the canvas extent (ABI v15). This
    // batch reaches the GPU through the graph's Batch2DNode, which drains it.
    io.gpu->Batch().Begin(frameWidth, frameHeight);
    io.gpu->Batch().SetGlobals(io.frameGlobals);
    io.app.PushSceneCamera((float)frameWidth, (float)frameHeight);
    {
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.runtime->SetRenderContext(&io.gpu->Batch());
        io.runtime->Loop().SubmitRender();
        io.perf.Add(io.perf.accRec, t0, io.perf.Now());
    }

#if !defined(ARCANE_DIST)
    // --pick-probe. THE ONE PLACE the entity ids
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
    // They cannot disagree even in principle: both are fitted to
    // frameWidth/frameHeight above, which IS the vehicle's own surface
    // extent (FrameExtent -- the swapchain's, or the offscreen output's).
    // In practice this block is windowed-only regardless: --pick-probe is
    // refused at parse time alongside --offscreen (HostConfig.cpp), because
    // an offscreen context declines to arm the fixed probe pixel.
    if (io.config.pickProbe)
    {
        const Arcane::PickView view{ io.runtime->CameraOffset(), io.runtime->CameraZoom() };
        io.pickDrawables.clear();
        Arcane::CollectPickables(io.runtime->Registry(), view, io.pickDrawables);

        // THE SCRIPTED SELECTION -- a stand-in for the editor selection
        // this host does not have: the FIRST pickable entity in the
        // scene, whose hit-proxy id is 1 by CollectPickables' own
        // ordering contract.
        io.pickSelectedIds.clear();
        if (!io.pickDrawables.empty())
            io.pickSelectedIds.push_back(1u);

        graphFrame.pickOutline = true;
        graphFrame.pickables   = io.pickDrawables;
        graphFrame.selectedIds = io.pickSelectedIds;
    }
#endif

    // ============================================================
    // THE OPAQUE 3D PASS (F2a Task 10) -- this frame's mesh scene, if the
    // scene the plugin just submitted has one. CollectMeshInstances is the
    // TESTED sweep (MeshSubmissionSystem.hpp); this call site stays thin on
    // purpose -- RuntimeFrame.cpp is not compiled into ArcaneTests, so
    // anything beyond "call the sweep, ask for a camera, decide whether to
    // arm the pass" would be untested logic living in a host TU.
    //
    // io.meshInstances (a RuntimeApp member reached through FrameIo -- see
    // that field's own comment) is the SAME "must outlive the RenderFrame
    // call" storage io.pickDrawables is just above, for the same reason:
    // MeshSceneDesc::instances is a span borrowed for the duration of that
    // call.
    Arcane::CollectMeshInstances(io.runtime->Registry(), io.meshInstances);

    // `meshScene` only needs to outlive `io.graph->RenderFrame(graphFrame)`
    // below, which happens inside THIS function call -- unlike the instance
    // vector above, a RenderGraph-local is enough for it, and graphFrame.mesh
    // is left null (its default) unless both an instance and a camera exist.
    Arcane::MeshSceneDesc meshScene;
    if (!io.meshInstances.empty())
    {
        // THE GUARDED PATH ONLY -- MeshSceneDesc's own comment (MeshNode.hpp)
        // is explicit that the caller owes valid matrices, and
        // ActivePerspectiveSceneCamera is the one function that either hands
        // back a validated (view, projection) pair or nullopt, never
        // identity. No active perspective camera means there is nothing to
        // draw the pass WITH, so graphFrame.mesh stays null below -- the same
        // "leave it alone rather than invent a viewpoint" contract
        // ActiveSceneCamera documents for the 2D path (SceneCamera.hpp).
        //
        // Aspect ratio guarded the same way the editor's camera-rect overlay
        // guards it (EditorAppFrame.cpp) rather than dividing raw: a 0-height
        // frameHeight would otherwise hand ActivePerspectiveSceneCamera an
        // Inf/NaN aspect that its own `aspectRatio <= 0.0f` check does not
        // catch.
        const float aspect = (frameHeight > 0)
                            ? (float)frameWidth / (float)frameHeight
                            : 1.0f;
        if (const auto cam = Arcane::ActivePerspectiveSceneCamera(io.runtime->Registry(), aspect))
        {
            meshScene.instances = io.meshInstances;
            meshScene.view       = cam->view;
            meshScene.projection = cam->projection;
            // NO SCENE LIGHT, DELIBERATELY: this engine has no light
            // component anywhere -- Scene/Components.hpp declares none and
            // SceneModule.hpp registers none -- so lightDirection/
            // lightColor/ambient are left at MeshSceneDesc's own documented
            // defaults (MeshNode.hpp:237-239). A light component is future
            // work and out of scope for F2a; inventing one here would land
            // an unreviewed scene-schema change in a host .cpp with zero
            // test coverage rather than in a reviewed, tested component.
            graphFrame.mesh = &meshScene;
        }
    }

    graphFrame.batch = &io.gpu->Batch();
    // The scene post chain as BYTES (SceneRenderResolver::PostDesc).
    // Re-read every frame for the same reason PostChain() is above:
    // a drain may swap the bound instance under an asset re-save.
    // Null (no PostProcess assignment, or the compile has not landed
    // yet) simply renders canvas -> tonemap -- which is also how a caller
    // asks for the frame WITHOUT a post chain at all (see FrameDesc).
    graphFrame.post    = io.resolver ? io.resolver->PostDesc() : nullptr;
    // The same GlobalParams the batcher got via SetGlobals, so the post
    // chain and the batch cannot bind different b1 contents.
    graphFrame.globals = &io.frameGlobals;
    // The capture is taken on the LAST frame, which has to be known
    // BEFORE the frame is declared (the readback is a graph NODE, not
    // an after-the-fact copy). Hence `+ 1`: io.frameCount is bumped in
    // CaptureTail, i.e. after this arm returns Presented.
    const bool willBeLastFrame =
        io.config.maxFrames != 0 && (io.frameCount + 1) >= io.config.maxFrames;
    graphFrame.capture = willBeLastFrame && !io.config.screenshotPath.empty();

    // Timed into accPresent rather than left unmeasured, so `--perf`
    // reports something real on this path too (the spec's waiting-
    // frame budget is mostly the pacing wait inside
    // AcquireNextTexture, which is inside this call). It is the whole
    // record + submit + present cost, not just the present: Execute()
    // does all three, and splitting one call across three
    // accumulators would be a fiction. accRec IS real (the submission
    // above is the work it names), but accEnd/accTone stay 0 -- there is
    // no separate flush or tonemap step left to time, both being inside
    // this one call.
    //
    // THE ONE LINE IN THIS FILE THAT NAMES THE MODE, and it is forced. The
    // vehicle deliberately exposes a PAIR that refuses each other's mode --
    // RenderFrame latches an error on an offscreen context and
    // RenderFrameOffscreen latches one on a host-window context, because
    // "a vehicle that quietly did the wrong thing for its mode would be a
    // wrong picture, not an error" (NriGraphContext.hpp). There is no
    // mode-agnostic entry point to route through, and inventing one in the
    // engine would erase exactly the refusal that design is buying. So the
    // frame driver picks, once, here -- everything above and below this line
    // is identical in both modes, which is the property Task 14's parity check
    // rests on.
    const auto graphT0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
    const Arcane::NriGraphContext::FrameOutcome outcome =
        io.graph->IsOffscreen() ? io.graph->RenderFrameOffscreen(graphFrame)
                                : io.graph->RenderFrame(graphFrame);
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

    // The capture tail. Reached on the final frame only, AFTER the
    // present -- so the pixels are the ones that were actually shown and
    // the capture cannot race the recording that produced it.
    if (lastFrame && !io.config.screenshotPath.empty())
    {
        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> actual;
        // The graph's readback NODE ran inside this frame's own command
        // buffer (declared up front, because a graph capture is a node and
        // not an afterthought) -- ReadCapture maps it and normalizes
        // BGRA -> RGBA off m_format, not a swapchain read, which is exactly
        // what makes this call MODE-AGNOSTIC: windowed resolves m_format
        // from the swapchain, offscreen sets it to kGraphOffscreenFormat,
        // and this call site never has to know which.
        const bool read = io.graph->ReadCapture(w, h, actual);

        if (!read)
        {
            // A WARN, never an exit code: ReadCapture's own contract is
            // explicit that "a capture that could not be read is not a bad
            // frame" -- so a miss here must not trip the GPU tests'
            // RenderErrorCount()==0 gate, nor flip io.graphExit.
            ARC_WARN("screenshot FAILED: {} (no capture landed)", io.config.screenshotPath);
        }
        else
        {
            // The exact pixels a player sees (post-tonemap, post-ImGui).
            if (Arcane::WritePngRgba(io.config.screenshotPath, w, h, actual.data()))
                ARC_INFO("screenshot written: {} ({}x{})", io.config.screenshotPath, w, h);
            else
                ARC_WARN("screenshot FAILED: {}", io.config.screenshotPath);
        }
    }

    return lastFrame;
}

}
