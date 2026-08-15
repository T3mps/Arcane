// RuntimeFrame: RuntimeApp::MainLoop's frame body, extracted verbatim (NRI
// Phase 3, Task 4). See RuntimeFrame.hpp's header comment for the contract;
// this file's six functions are MainLoop's old body, moved statement-for-
// statement, with `m_foo` renamed to `io.foo` (free functions, not RuntimeApp
// members) and the control-flow substitutions FrameIo::quit/skipFrame exist
// for. Nothing here is a behavior change -- see the task-4 report for the
// line-range mapping back to the pre-extraction RuntimeApp.cpp.

#include "RuntimeFrame.hpp"
#include "RuntimeApp.hpp"   // FrameIo::app.PushSceneCamera -- see that method's comment

#include <Arcane/Assets/Assets.hpp>       // Arcane::ReadTexturePixels/WritePngRgba (CaptureTail, GoldenArtifact)
#include <Arcane/Assets/GoldenImage.hpp>  // Arcane::CompareRgbaImages/WriteDiffPng (GoldenArtifact)
#include <Arcane/Audio/AudioDevice.hpp>   // complete type for AudioSystem().Update (AdvanceSim's voice reap)
#include <Arcane/Base/Diagnostics.hpp>    // Diagnostics::Heartbeat (PumpAndResize)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Render/Batcher2D.hpp>              // Arcane::Batch2DStats (BuildHud's HUD text, CaptureTail's perf tick)
#include <Arcane/Render/Device.hpp>                 // Arcane::GraphicsBackend / ToString (BuildHud's HUD text, GoldenArtifact)
#include <Arcane/Render/FullscreenMaterialChain.hpp>// scene post hook (RenderNvrhi)
#include <Arcane/Render/GpuInstrumentation.hpp>     // Arcane::GpuPassScope, GpuDeviceLostObserved (RenderNvrhi, PumpAndResize)
#include <Arcane/Render/PickEmit.hpp>                // CollectPickables (RenderGraph's --pick-probe)

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace
{
    // The golden artifact tail, shared by BOTH render paths (NRI Phase 2,
    // Task 7). It takes display-referred RGBA8 pixels of the frame that was
    // just presented and nothing else -- so --nri-graph inherits the artifact
    // NAMING, the comparator, the tolerances and the exit code from the NVRHI
    // path by construction rather than by a second implementation that could
    // drift from it. Returns 0, or 3 on any capture-write or compare failure
    // (the host's documented golden exit code).
    //
    // The two paths differ only in where the pixels came from: nvrhi
    // ReadTexturePixels off the backbuffer, or NriGraphContext::ReadCapture
    // off the graph's readback node (which normalizes BGRA -> RGBA, since NRI
    // resolves the swapchain's channel order rather than letting us pin it).
    //
    // Moved verbatim from RuntimeApp.cpp's anonymous namespace (NRI Phase 3,
    // Task 4): its one and only call site (the capture tail, below in
    // CaptureTail) moved here too, and an anonymous-namespace helper has
    // internal linkage -- it cannot be called across translation units, so
    // duplicating it was the only alternative to moving it. See the task-4
    // report.
    int GoldenArtifact(const Arcane::HostConfig& config, std::uint32_t width, std::uint32_t height,
                       const std::vector<unsigned char>& actual)
    {
        // Stage-golden stem (HostConfig::goldenStage's own comment carries the
        // contract). Full resolves to EXACTLY the pre-Phase-2 string, so Phase
        // 0's captured goldens keep working under their existing filenames;
        // batch/post get their own stem so a scripted three-stage run writes
        // three files instead of overwriting one.
        const char* stage =
            config.goldenStage == Arcane::GoldenStage::Batch ? "batch"
          : config.goldenStage == Arcane::GoldenStage::Post  ? "post"
                                                             : nullptr;
        const std::string name =
            !config.goldenName.empty()
                // Explicit stem = the whole filename stem (the cross-backend
                // compare names the OTHER backend's golden here), so the stage
                // can only be appended to it.
                ? (stage ? config.goldenName + "-" + stage : config.goldenName)
                : std::string("main-") + (stage ? std::string(stage) + "-" : "") +
                  (config.backend == Arcane::GraphicsBackend::Vulkan ? "vulkan" : "dx12");

        // --golden-capture takes priority if both flags were somehow given;
        // ordinary invocations pass exactly one, matching the harness scripts.
        if (!config.goldenCapturePath.empty())
        {
            const std::filesystem::path out =
                std::filesystem::path(config.goldenCapturePath) / (name + ".png");
            if (Arcane::WritePngRgba(out, width, height, actual.data()))
            {
                ARC_INFO("golden captured: {} ({}x{})", out.generic_string(), width, height);
                return 0;
            }
            ARC_ERROR("golden capture FAILED: {}", out.generic_string());
            return 3;
        }

        const std::filesystem::path dir(config.goldenComparePath);
        const std::filesystem::path goldenPath = dir / (name + ".png");
        std::uint32_t gw = 0, gh = 0;
        std::vector<unsigned char> golden;
        if (!Arcane::LoadPngRgba(goldenPath, gw, gh, golden))
        {
            ARC_ERROR("golden: no golden at {}", goldenPath.generic_string());
            return 3;
        }

        const Arcane::GoldenCompareResult r =
            Arcane::CompareRgbaImages(golden.data(), gw, gh, actual.data(), width, height);
        if (r.ok)
        {
            ARC_INFO("golden PASS: {} (maxDelta {}, bad {:.4f}%)",
                     name, r.maxChannelDelta, r.badPixelFraction * 100.0f);
            return 0;
        }
        ARC_ERROR("golden FAIL: {} (dims {}, maxDelta {}, bad {:.4f}%, first ({},{}))",
                  name, r.dimensionsMatch ? "ok" : "MISMATCH",
                  r.maxChannelDelta, r.badPixelFraction * 100.0f, r.firstBadX, r.firstBadY);
        (void)Arcane::WritePngRgba(dir / (name + ".actual.png"), width, height, actual.data());
        if (r.dimensionsMatch)
            (void)Arcane::WriteDiffPng(dir / (name + ".diff.png"),
                                        golden.data(), actual.data(), gw, gh,
                                        Arcane::GoldenCompareParams{}.channelTolerance);
        return 3;
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

    // EXACTLY ONE pump per frame, and on the --nri-graph path it is the
    // VEHICLE's window rather than the host's. SDL's event queue is
    // process-wide and Window::PumpEvents drains all of it (filtering by
    // its own id for close/resize), so pumping both windows would make
    // each silently eat the other's events. The vehicle owns the visible
    // surface on that path -- the host's window stays hidden, because
    // DXGI allows only one flip-model swapchain per HWND (see
    // NriGraphContext.hpp, THE TWO-DEVICE WINDOW) -- so the window the
    // user can actually close and drag is the one that must be pumped.
    Arcane::Window& eventWindow = io.graph ? io.graph->Win() : io.gpu->Win();
    const Arcane::WindowEvents events = eventWindow.PumpEvents();
    if (events.quitRequested) return true;
    if (events.resized)
    {
        // Strictly BETWEEN frames -- never between the graph's acquire and
        // its present, which both live inside one Execute() call
        // (RgExecuteDesc::swapChain). A frame driver that resizes at the
        // top of its loop satisfies that structurally, and owes nothing
        // else: the graph rebuilds imported-texture views every Execute.
        if (io.graph)
        {
            io.graph->Resize(events.width, events.height);
            // ...and the HIDDEN host window's SIZE, which is not cosmetic
            // on this path: ImGui_ImplSDL3_NewFrame reads its size into
            // io.DisplaySize, and the HUD's vertices are in that space.
            // Leaving it pinned at 1280x720 while the visible (vehicle)
            // surface grew would stretch and mis-clip the HUD, because the
            // ImGui node's projection maps DisplaySize onto the whole
            // backbuffer. Same reasoning as the canvas line below -- ONE
            // source of truth for the frame's extent -- and inert at the
            // default size every golden was captured at. The host window
            // is hidden, so resizing it shows nothing and its own resize
            // event is filtered out by the vehicle window's PumpEvents.
            io.gpu->Win().SetSize(events.width, events.height);
        }
        // The canvas -- and therefore the viewport extent the resolver
        // reports into the material globals -- tracks the window on BOTH
        // paths. On the graph path this also resizes the hidden host
        // swapchain, which is wasted work, but keeping ONE source of truth
        // for the frame's extent beats a second, divergent one.
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
        ImGui::Text("Backend: %s", Arcane::ToString(io.gpu->Device().Backend()));
        ImGui::Text("Plugin gen: %u", io.plugin->Generation());
        const Arcane::Batch2DStats s = io.gpu->Batch().Stats();
        ImGui::Text("Quads: %u  Draws: %u", s.quads, s.drawCalls);
        ImGui::End();
    }

    // ABI v2: the game module + any secondary plugins draw their own ImGui between
    // BeginFrame and Render. Each entry point is null-checked inside DrawUIAll.
    io.plugin->DrawUIAll();

    // The NVRHI backbuffer, acquired only on the NVRHI path -- the graph
    // acquires its own inside Execute(), as late as possible, and never
    // touches this swapchain.
    io.backbuffer = nullptr;
    if (!io.graph)
    {
        io.backbuffer = io.gpu->Swap().BeginFrame();
        if (!io.backbuffer)
        {
            // No backbuffer this frame: still balance BeginFrame with EndFrame
            // so ImGui's assert (double-Begin) doesn't fire next iteration.
            ImGui::EndFrame();
            io.skipFrame = true;
            return;
        }
    }

    // Hot reload: poll shaders once a second; pipeline caches rebuild lazily.
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
        Arcane::SceneRenderResolver::FrameInfo frame;
        frame.now            = io.hostClock;
        frame.dt             = io.lastFrameDt;
        frame.viewportWidth  = (float)io.gpu->Cnv().Width();
        frame.viewportHeight = (float)io.gpu->Cnv().Height();
        io.resolver->Refresh(frame);
        io.frameGlobals = io.resolver->Globals();
    }

    io.skipFrame = false;
}

// =============================================================
// THE RENDER HALF. Exactly one of these two blocks runs per frame.
//
// NVRHI (default, and the regression floor this whole phase is
// measured against): record the whole frame into one command list --
// canvas clear, batcher, post chain, tonemap, ImGui -- then submit and
// present. NOT ONE LINE of it changed for Phase 2 beyond this brace
// and the indent it forced; the graph path is built BESIDE it, never
// through it.
// =============================================================
void RenderNvrhi(FrameIo& io)
{
    io.gpu->Cmd()->open();
    // F-8b: the runtime records its WHOLE frame into one command list, so
    // the outer scope is that recording -- everything below nests inside it,
    // and the canvas clear (which has no scope of its own) is covered by it.
    // Held in an optional because the scope must END while the list is still
    // open, and close() is ~100 lines down at the bottom of this loop body:
    // a plain block would mean reindenting the entire frame for one marker.
    std::optional<Arcane::GpuPassScope> framePass;
    framePass.emplace(io.gpu->Cmd(), "pass:frame");

    io.gpu->Cmd()->clearTextureFloat(io.gpu->Cnv().Texture(), nvrhi::AllSubresources,
                                    nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));

#if !defined(ARCANE_DIST)
    // --crash-gpu N: the deliberate fault, nested inside pass:frame so the
    // breadcrumb ring shows the real timeline this host records rather than
    // a lone synthetic scope. INSIDE the frame's own command list, not on a
    // private one, for the same reason -- the capture should see the host's
    // ordinary recording shape, with pass:gpu-fault as the last scope the
    // GPU ever begins. Fires exactly once.
    if (io.config.crashGpuFrame != 0 && !io.gpuFaultFired &&
        io.frameCount >= io.config.crashGpuFrame)
    {
        io.gpuFaultFired = true;   // set FIRST: a failed build must not retry every frame
        if (!io.gpuFault)
            io.gpuFault = Arcane::GpuFaultInjector::Create(io.gpu->Device().Nvrhi(),
                                                          io.gpu->Shaders());
        if (io.gpuFault)
            io.gpuFault->Fire(io.gpu->Cmd());
        else
            ARC_ERROR("--crash-gpu: fault injector unavailable -- nothing dispatched");
    }
#endif

    io.gpu->Batch().Begin(io.gpu->Cmd(), io.gpu->Cnv().Framebuffer(),
                         io.gpu->Cnv().Width(), io.gpu->Cnv().Height());
    // Engine-global material constants (Time/Delta/Viewport) for registered
    // sprite materials; sticky, so once per frame after Begin. Built-in
    // pipelines ignore them -- but before this arc an animated material read
    // zeros here, because the host never called SetGlobals at all.
    io.gpu->Batch().SetGlobals(io.frameGlobals);

    // Set the render context IN Arcane.dll so TypeID<RenderContext2D> resolves
    // in the correct module; then drive the plugin's RenderSubmissionSystem.
    // ArcaneRuntime stays camera-agnostic: SetRenderContext writes the STORED camera the
    // plugin drives via Runtime::SetCamera (default identity if it never does).
    io.app.PushSceneCamera((float)io.gpu->Cnv().Width(), (float)io.gpu->Cnv().Height());

    {
        // Scope names deliberately mirror FramePerf's acc* accumulators
        // (F-8b): a CPU timeline and a GPU timeline that use two vocabularies
        // for the same phases cannot be read side by side.
        Arcane::GpuPassScope pass(io.gpu->Cmd(), "pass:rec");
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.runtime->SetRenderContext(&io.gpu->Batch());
        io.runtime->Loop().SubmitRender();
        io.perf.Add(io.perf.accRec, t0, io.perf.Now());
    }

    {
        // Where the batcher's draws actually get recorded -- Begin() above
        // only sets state, End() is the flush.
        Arcane::GpuPassScope pass(io.gpu->Cmd(), "pass:end");
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.gpu->Batch().End();
        io.perf.Add(io.perf.accEnd, t0, io.perf.Now());
    }

    nvrhi::FramebufferHandle& fb = io.gpu->FramebufferFor(io.backbuffer);
    {
        Arcane::GpuPassScope pass(io.gpu->Cmd(), "pass:tone");
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        // Scene post hook (post arc): with a bound chain the linear canvas
        // feeds it as the external Scene input and the tonemap samples the
        // chain's output -- without one this is exactly the old line
        // (byte-identical). The chain comes from the resolver's PostProcess
        // sweep, re-read every frame because a drain may swap the bound
        // instance under an asset re-save.
        Arcane::FullscreenMaterialChain* postChain =
            io.resolver ? io.resolver->PostChain() : nullptr;
        const Arcane::MaterialInstance* postInstance =
            io.resolver ? io.resolver->PostInstance() : nullptr;
        // --golden-stage batch: the BATCH golden is "batcher + tonemap and
        // nothing else", so the post chain is bypassed even when the scene
        // binds one -- that is the whole point of a stage golden (a batch
        // node regression and a post node regression must not look the
        // same). Gated on GoldenMode() so an ordinary run is untouched:
        // outside a golden run goldenStage is not even read (and Parse
        // refuses a non-Full stage without golden mode anyway).
        const bool stageSkipsPost =
            io.config.GoldenMode() &&
            io.config.goldenStage == Arcane::GoldenStage::Batch;
        Arcane::Canvas* post =
            (!stageSkipsPost && postChain && postChain->Ready() && postInstance)
                ? io.gpu->EnsurePost() : nullptr;
        if (post)
        {
            postChain->Render(io.gpu->Cmd(), post->Framebuffer(),
                              *postInstance, io.frameGlobals,
                              &io.runtime->AssetsFacade(),
                              static_cast<std::size_t>(-1),
                              io.gpu->Cnv().Texture());
            io.gpu->Tone().Run(io.gpu->Cmd(), post->Texture(), fb);
        }
        else
        {
            io.gpu->Tone().Run(io.gpu->Cmd(), io.gpu->Cnv().Texture(), fb);
        }
        io.perf.Add(io.perf.accTone, t0, io.perf.Now());
    }
    // --golden-stage batch|post: the HUD is host chrome, not scene content
    // -- it would sit on top of every stage golden and mask exactly the
    // pixels a node-by-node cutover needs to compare. Both non-Full stages
    // drop it; Full keeps it (the Phase 0 goldens include it, and their
    // filenames are unchanged).
    //
    // ImGui::EndFrame() rather than simply skipping: ImGuiLayer's contract
    // is that every BeginFrame is paired with exactly one Render, and the
    // frame HAS begun above (the HUD window and the plugin's DrawUIAll
    // already recorded into it). Ending it without drawing is the same
    // balancing move the no-backbuffer path above makes.
    if (io.config.GoldenMode() &&
        io.config.goldenStage != Arcane::GoldenStage::Full)
    {
        ImGui::EndFrame();
    }
    else
    {
        Arcane::GpuPassScope pass(io.gpu->Cmd(), "pass:imgui");
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.gpu->Imgui().Render(io.gpu->Cmd(), fb);
        io.perf.Add(io.perf.accImgui, t0, io.perf.Now());
    }

    // Ends the frame scope BEFORE close(): a marker recorded into a closed
    // list latches the D3D12 marker layer off for the rest of the process
    // and is an access violation on Vulkan.
    framePass.reset();
    io.gpu->Cmd()->close();
    {
        // No pass scope here: submit + present happen with the list already
        // closed, so there is nothing left to record markers into. F-8b's
        // "close + submit + present" row is a CPU seam only.
        const auto t0 = io.perf.On() ? io.perf.Now() : Arcane::FramePerf::Clock::time_point{};
        io.gpu->Device().Nvrhi()->executeCommandList(io.gpu->Cmd());
        io.gpu->Swap().Present();
        io.perf.Add(io.perf.accPresent, t0, io.perf.Now());
    }

    // GPU-progress heartbeat (Task 7): after the frame's last submit.
    io.gpu->FrameProgress().EndFrame();
}

// =============================================================
// The GRAPH half (--nri-graph). One RenderGraph frame: Reset,
// declare, Compile, Execute -- and Execute is what acquires the
// backbuffer, records every node, submits and presents. Today that is
// batch2d -> tonemap (+ a capture node on the last frame of a golden /
// screenshot run); Tasks 10-12 add the post chain, pick/outline and
// ImGui inside DeclareGraphFrame, gated by the same --golden-stage
// vocabulary passed here.
//
// The plugin's RENDER submission runs on BOTH paths as of Task 8: the
// batcher is Begin()'d with no command list (nothing is recorded into
// NVRHI here), SubmitRender fills it exactly as it does above, and the
// graph's Batch2DNode DRAINS it -- one batcher, one batching algorithm,
// two recorders. End() is deliberately NOT called on this path: it is
// the NVRHI recorder, and calling it would need a target this path does
// not have.
//
// The scene POST CHAIN runs here too as of Task 10, from the same
// compiled bytes the NVRHI chain was built from -- one node per pass,
// between the canvas and the tonemap.
//
// The HUD runs here too as of Task 12, from the same ImGui frame the
// NVRHI path renders -- see the block below.
//
// WHAT THIS PATH STILL DOES NOT DO, named so it stays a known gap:
// sprite TEXTURES inside the batch node, which live on the engine's
// NVRHI device and cannot be sampled by the graph's own device
// (Batch2DNode.hpp, THE TEXTURE GAP); a post material's declared
// TEXTURE params, which take the same white-texel fallback
// (FullscreenNodes.cpp, THE POST TEXTURE GAP); and ImGui INPUT, which
// reaches the host window's event tap rather than this one
// (NriGraphContext.hpp, THE TWO-DEVICE WINDOW -- the HUD draws but
// does not respond, deliberately). The SIM half (FixedUpdate/Update)
// is untouched and runs identically.
// =============================================================
Arcane::NriGraphContext::FrameOutcome RenderGraph(FrameIo& io)
{
    Arcane::NriGraphContext::FrameDesc graphFrame;

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
    // The stage split mirrors the NVRHI block's exactly: both non-Full
    // golden stages END the frame without rendering it (host chrome
    // would mask the pixels a stage golden compares), and Full renders
    // it and hands the draw data to the graph. ImGui::Render() ends
    // the frame itself, so ImGuiLayer's "every BeginFrame is paired
    // exactly once" contract holds on both arms.
    //
    // The draw data lives in the ImGui context until the next
    // NewFrame, i.e. for the whole of the RenderFrame call below --
    // which is what lets the node copy the vertices at RECORD time.
    // ============================================================
    if (io.config.GoldenMode() &&
        io.config.goldenStage != Arcane::GoldenStage::Full)
    {
        ImGui::EndFrame();
    }
    else
    {
        ImGui::Render();
        graphFrame.imgui = ImGui::GetDrawData();
    }

    // The 2D submission, CPU-identical to the NVRHI block above. The
    // null command list / null framebuffer are what say "record
    // nothing": Batcher2D::Begin only stores them, and every recording
    // use is inside End(), which this path never calls. The canvas
    // extent is still the NVRHI canvas's -- it is what the resolver
    // reports into the material globals, and RuntimeApp resizes both
    // canvases from the same event.
    io.gpu->Batch().Begin(nullptr, nullptr,
                         io.gpu->Cnv().Width(), io.gpu->Cnv().Height());
    io.gpu->Batch().SetGlobals(io.frameGlobals);
    io.app.PushSceneCamera((float)io.gpu->Cnv().Width(), (float)io.gpu->Cnv().Height());
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
    // the graph's pick node rasterises and what PickEntityForId below
    // inverts. It lives here, not in the vehicle, because this is the
    // object that owns a registry and a camera -- NriGraphContext is a
    // render vehicle and must not grow either.
    //
    // The view is the SCENE camera PushSceneCamera just installed, so
    // the id pass rasterises the same silhouettes the batch node drew.
    // (The camera was fitted to the NVRHI canvas extent, which
    // RuntimeApp resizes from the same event as the graph swapchain --
    // the two agree at every size the host actually produces.)
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
    // an after-the-fact copy). Hence `+ 1`: m_frameCount is bumped
    // after a successful frame, below.
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
        ARC_ERROR("--nri-graph: the frame could not be recorded or submitted; stopping");
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
