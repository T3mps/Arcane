// Loom: Init -> MainLoop -> Shutdown. The frame loop is the M4 Playground loop
// (scene content removed) interleaving the plugin's FixedUpdate/Update via the
// RunLoop with the engine schedulers, plus a PluginHost watching the game DLL.
// The render plumbing + teardown order live in GpuContext (m_gpu). The teardown
// CONTRACT is encoded in the Loom member declaration order -- see Loom.hpp.

#include "Loom.hpp"

#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Render/Batcher2D.hpp>   // Arcane::Batch2DStats (loop HUD + perf tick)
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (HUD)

#include <Astra/Core/TypeContext.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

Loom::Loom(LoomConfig cfg)
    : m_config(std::move(cfg)), m_perf(m_config.perf) {}

bool Loom::Init()
{
    // The whole platform/render/input stack, booted in order. Owned by m_gpu and
    // declared BEFORE m_runtime/m_plugin in Loom -- so it destructs AFTER them:
    // the render resources it owns (window/device/swapchain/shaders/canvas/batcher/
    // tonemap/imgui/input + commandList/framebuffers) must outlive runtime + plugin.
    m_gpu = GpuContext::Create(m_config);
    if (!m_gpu)
    {
        ARC_ERROR("Loom: GPU context create failed");
        return false;
    }

    ARC_INFO("{} -- Loom host, backend {}", Arcane::BuildInfo(), Arcane::ToString(m_config.backend));

    // The TypeContext is the process-wide type-identity singleton shared across
    // Loom.exe, Arcane.dll, and every loaded plugin. It is intentionally
    // heap-allocated and never freed: TypeMeta entries registered by the plugin
    // (via ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled
    // into the plugin DLL. After PluginHost::Unload -> DLClose, those thunks
    // point to unmapped memory. If the TypeContext (and its MetaRegistry) were
    // ever destructed, ~std::function() would invoke those thunks -> crash.
    // Heap-leaking is the correct production pattern for a long-running host;
    // the OS reclaims all process memory on exit anyway.
    // (The test exe is safe for a different reason: MetaRegistry::Register is first-writer-wins,
    // and the test TUs that reflect these types are never unloaded, so their thunks -- not the
    // plugin's -- own the MetaRegistry entries.)
    m_typeContext = new Astra::TypeContext();
    m_runtime.emplace(m_typeContext);

    // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
    // Runtime so a plugin can build its own engine render objects (e.g. the
    // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
    // plugin (m_gpu is declared before the runtime/plugin in Loom). Null in a
    // headless host -> the plugin skips its GPU-resource creation.
    m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders());

    // ABI v2: install the host's ImGui context + allocators on the Runtime BEFORE
    // the plugin loads. PluginHost::RefreshContext copies these into the EngineContext
    // at Init time, so the plugin's Init adopts the host's GImGui across the DLL boundary.
    // The ImGuiLayer (in m_gpu) has already created + set the current context by here.
    {
        ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
        ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
        m_runtime->SetImGui(ImGui::GetCurrentContext(),
                            reinterpret_cast<void*>(allocFn),
                            reinterpret_cast<void*>(freeFn),
                            ud);
    }

    m_plugin.emplace(*m_runtime, std::filesystem::path(m_config.pluginPath));
    if (!m_plugin->Load())
    {
        ARC_ERROR("Loom: failed to load plugin '{}'", m_config.pluginPath);
        return false;
    }

    return true;
}

void Loom::MainLoop()
{
    // The reused command list + the lazy backbuffer-framebuffer cache live in
    // m_gpu (m_gpu->Cmd() / m_gpu->FramebufferFor(bb)) so they release their
    // NVRHI handles before the device, in m_gpu's teardown.

    auto simPrev       = std::chrono::steady_clock::now();
    auto lastFrameTime = simPrev;
    auto lastShaderPoll = simPrev;
    bool running = true;

    while (running)
    {
        auto events = m_gpu->Win().PumpEvents();
        if (events.quitRequested) break;
        if (events.resized)
            m_gpu->OnResize(events.width, events.height);
        if (m_gpu->Win().IsMinimized())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        m_perf.FrameStart();

        // Input: sample SDL state, evaluate actions. Must precede ImGui BeginFrame
        // so capture flags are set before the evaluator reads them.
        {
            const auto now = std::chrono::steady_clock::now();
            const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
            lastFrameTime = now;
            const Arcane::InputSnapshot snap =
                m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(),
                                          m_gpu->Imgui().WantCaptureMouse());
            m_runtime->SetInputSnapshot(snap);   // plugins read it via Runtime::Input()
            m_gpu->Input().Update(frameDt, snap);
            if (m_gpu->Input().Pressed("quit"))                break;
            if (m_gpu->Input().Pressed("reload_plugin"))       m_plugin->ForceReload();
            if (m_gpu->Input().Pressed("reload_plugin_fresh")) m_plugin->ReloadFresh();
        }

        // Sim advance: clamp dt, drive RunLoop with plugin callbacks interleaved.
        {
            const auto now = std::chrono::steady_clock::now();
            double simDt = std::chrono::duration<double>(now - simPrev).count();
            simPrev = now;
            if (simDt > 0.25) simDt = 0.25;
            const Arcane::PluginVTable* vt = m_plugin->Vtable();
            const auto t0 = m_perf.On() ? m_perf.Now() : FramePerf::Clock::time_point{};
            m_runtime->Loop().Advance(simDt,
                [&](double dt)          { if (vt) vt->FixedUpdate(dt); },
                [&](double dt, double a){ if (vt) vt->Update(dt, a); });
            m_perf.Add(m_perf.accSim, t0, m_perf.Now());
        }

        m_gpu->Imgui().BeginFrame();
        {
            ImGui::Begin("Loom");
            ImGui::Text("Backend: %s", Arcane::ToString(m_gpu->Device().Backend()));
            ImGui::Text("Plugin gen: %u", m_plugin->Generation());
            const Arcane::Batch2DStats s = m_gpu->Batch().Stats();
            ImGui::Text("Quads: %u  Draws: %u", s.quads, s.drawCalls);
            ImGui::End();
        }

        // ABI v2: the plugin draws its own ImGui between BeginFrame and Render.
        // Null-checked (a v2 plugin may omit DrawUI); PlaygroundGame's is a no-op.
        const Arcane::PluginVTable* vtUI = m_plugin->Vtable();
        if (vtUI && vtUI->DrawUI) vtUI->DrawUI();

        nvrhi::ITexture* backbuffer = m_gpu->Swap().BeginFrame();
        if (!backbuffer)
        {
            // No backbuffer this frame: still balance BeginFrame with EndFrame
            // so ImGui's assert (double-Begin) doesn't fire next iteration.
            ImGui::EndFrame();
            continue;
        }

        // Hot reload: poll shaders once a second; pipeline caches rebuild lazily.
        {
            const auto now0 = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now0 - lastShaderPoll).count() >= 1.0)
            {
                m_gpu->Shaders().Poll();
                lastShaderPoll = now0;
            }
        }

        m_gpu->Cmd()->open();
        m_gpu->Cmd()->clearTextureFloat(m_gpu->Cnv().Texture(), nvrhi::AllSubresources,
                                        nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));

        m_gpu->Batch().Begin(m_gpu->Cmd(), m_gpu->Cnv().Framebuffer(),
                             m_gpu->Cnv().Width(), m_gpu->Cnv().Height());

        // Set the render context IN Arcane.dll so TypeID<RenderContext2D> resolves
        // in the correct module; then drive the plugin's RenderSubmissionSystem.
        // Loom stays camera-agnostic: SetRenderContext writes the STORED camera the
        // plugin drives via Runtime::SetCamera (default identity if it never does).
        {
            const auto t0 = m_perf.On() ? m_perf.Now() : FramePerf::Clock::time_point{};
            m_runtime->SetRenderContext(&m_gpu->Batch());
            m_runtime->Loop().SubmitRender();
            m_perf.Add(m_perf.accRec, t0, m_perf.Now());
        }

        {
            const auto t0 = m_perf.On() ? m_perf.Now() : FramePerf::Clock::time_point{};
            m_gpu->Batch().End();
            m_perf.Add(m_perf.accEnd, t0, m_perf.Now());
        }

        nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
        {
            const auto t0 = m_perf.On() ? m_perf.Now() : FramePerf::Clock::time_point{};
            m_gpu->Tone().Run(m_gpu->Cmd(), m_gpu->Cnv().Texture(), fb);
            m_perf.Add(m_perf.accTone, t0, m_perf.Now());
        }
        {
            const auto t0 = m_perf.On() ? m_perf.Now() : FramePerf::Clock::time_point{};
            m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
            m_perf.Add(m_perf.accImgui, t0, m_perf.Now());
        }

        m_gpu->Cmd()->close();
        {
            const auto t0 = m_perf.On() ? m_perf.Now() : FramePerf::Clock::time_point{};
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            m_gpu->Swap().Present();
            m_perf.Add(m_perf.accPresent, t0, m_perf.Now());
        }

        // Debounced watcher: rebuild PlaygroundGame -> auto hot reload.
        {
            const auto t0 = m_perf.On() ? m_perf.Now() : FramePerf::Clock::time_point{};
            m_plugin->Poll();
            m_perf.Add(m_perf.accPoll, t0, m_perf.Now());
        }

        if (m_perf.On())
        {
            const Arcane::Batch2DStats bs = m_gpu->Batch().Stats();
            m_perf.Tick(bs.quads, bs.drawCalls);
        }

        ++m_frameCount;
        if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames)
            running = false;
    }
}

void Loom::Shutdown()
{
    if (m_gpu) m_gpu->Device().Nvrhi()->waitForIdle();
    ARC_INFO("Loom exiting after {} frames", m_frameCount);

    // The member destructors then run (after Run returns + ~Loom), in reverse
    // declaration order -- the load-bearing TEARDOWN CONTRACT:
    //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
    //                ResetRegistry) while the plugin DLL is STILL mapped.
    //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
    //   m_gpu     -> ~GpuContext: the render stack (command list + framebuffer
    //                cache release their NVRHI handles before the device), window
    //                LAST. So gpu outlives runtime + plugin exactly as the old
    //                outer/inner main scopes did. See GpuContext's header.
    // m_typeContext is intentionally NOT freed (heap-leaked, see Init).
}

int Loom::Run()
{
    if (!Init()) return 1;
    MainLoop();
    Shutdown();
    return 0;
}
