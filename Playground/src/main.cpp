// Arcane Playground -- M4: simulation substrate + parent/child sprite scene
// wired into the M3 demo (bouncing shapes + HDR swatch + ACES tonemap + input).
// A Simulation owns the Registry + SystemSchedulers; RunLoop drives FixedUpdate
// (TransformPropagationSystem) and Render (RenderSubmissionSystem). The orbiter
// rotation is mutated inline each frame -- NOT as an ECS system -- so it
// coexists cleanly with the batcher's direct-draw demo path.
// Scripted verification: --frames N renders N frames and exits 0.

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/Swapchain.hpp>
#include <Arcane/Render/TonemapPass.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/Simulation.hpp>
#include <Arcane/Text/TextSystem.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>
#include <imgui.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace
{
    void PrintUsage()
    {
        std::printf(
            "Arcane Playground (M3: input action system demo)\n"
            "  --backend dx12|vulkan   graphics backend (default dx12)\n"
            "  --frames N              render N frames then exit (default: until closed)\n"
            "  --no-vsync              present without vsync\n");
    }
}

int main(int argc, char** argv)
{
    Arcane::GraphicsBackend backend = Arcane::GraphicsBackend::D3D12;
    uint64_t maxFrames = 0;
    bool vsync = true;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
        {
            ++i;
            if (std::strcmp(argv[i], "vulkan") == 0)
                backend = Arcane::GraphicsBackend::Vulkan;
            else if (std::strcmp(argv[i], "dx12") == 0)
                backend = Arcane::GraphicsBackend::D3D12;
            else
            {
                PrintUsage();
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        {
            char* end = nullptr;
            errno = 0;
            maxFrames = std::strtoull(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || maxFrames == 0)
            {
                std::fprintf(stderr, "error: --frames requires a positive integer\n");
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--no-vsync") == 0)
        {
            vsync = false;
        }
        else
        {
            PrintUsage();
            return 2;
        }
    }

    Arcane::Log::Init();
    ARC_INFO("{} -- requested backend: {}", Arcane::BuildInfo(),
             Arcane::ToString(backend));

    Arcane::Window window;
    Arcane::WindowDesc windowDesc;
    windowDesc.title  = "Arcane Playground";
    windowDesc.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
    if (!window.Create(windowDesc))
        return 1;

    Arcane::RenderDeviceDesc deviceDesc;
    deviceDesc.backend = backend;
    auto device = Arcane::RenderDevice::Create(deviceDesc);
    if (!device)
        return 1;

    auto swapchain = device->CreateSwapchain(window, vsync);
    if (!swapchain)
        return 1;

    auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                 "shaders");
    if (!shaders)
        return 1;
    auto canvas = Arcane::CreateCanvas(device->Nvrhi(),
                                       swapchain->Width(), swapchain->Height());
    if (!canvas)
        return 1;
    auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
    if (!batcher)
        return 1;
    auto tonemap = Arcane::TonemapPass::Create(device->Nvrhi(), *shaders);
    if (!tonemap)
        return 1;

    auto text = Arcane::TextSystem::Create(device->Nvrhi());
    if (!text)
    {
        std::fprintf(stderr, "error: TextSystem::Create failed\n");
        return 1;
    }
    const Arcane::FontId hudFont = text->LoadFont("data/fonts/Roboto-Regular.ttf");
    if (hudFont == Arcane::kInvalidFontId)
    {
        std::fprintf(stderr, "error: failed to load HUD font\n");
        return 1;
    }

    // ImGuiLayer must be destroyed before window (the layer taps window
    // events), so it is declared AFTER window -- stack unwinds in reverse
    // declaration order: imgui here is destroyed first, window last.
    auto imgui = Arcane::ImGuiLayer::Create(window, *device, *shaders);
    if (!imgui)
    {
        std::fprintf(stderr, "error: ImGuiLayer::Create failed\n");
        return 1;
    }

    auto inputDevices = Arcane::InputDevices::Create();
    auto input = Arcane::InputActions::Create();
    if (!inputDevices || !input || !input->LoadFile("data/input_actions.json"))
    {
        std::fprintf(stderr, "error: input system init failed\n");
        return 1;
    }
    input->SetBaseContext("demo");
    bool showStats = true;
    glm::vec2 moveOffset(0.0f);

    // --- M4 scene slice: substrate + a moving parent/child sprite ---
    Arcane::JobSystem jobs;
    Arcane::Simulation sim(jobs.WorkScheduler());
    Arcane::RegisterSceneComponents(sim.Registry());
    sim.Schedulers().fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
    sim.Schedulers().render.AddSystem<Arcane::RenderSubmissionSystem>();

    Astra::Entity sceneRoot = sim.Registry().CreateEntity();
    sim.Registry().AddComponent<Arcane::LocalTransform>(sceneRoot, Arcane::LocalTransform{});
    sim.Registry().AddComponent<Arcane::WorldTransform>(sceneRoot, Arcane::WorldTransform{});

    Astra::Entity orbiter = sim.Registry().CreateEntity();
    {
        Arcane::LocalTransform t;
        t.position = glm::vec2((float)canvas->Width() * 0.5f, (float)canvas->Height() * 0.5f);
        sim.Registry().AddComponent<Arcane::LocalTransform>(orbiter, t);
        sim.Registry().AddComponent<Arcane::WorldTransform>(orbiter, Arcane::WorldTransform{});
        Arcane::SpriteRenderer sr; sr.size = glm::vec2(48.0f); sr.tint = glm::vec4(0.9f, 0.7f, 0.2f, 1.0f);
        sim.Registry().AddComponent<Arcane::SpriteRenderer>(orbiter, sr);
        sim.Registry().SetParent(orbiter, sceneRoot);
    }
    Astra::Entity moon = sim.Registry().CreateEntity();
    {
        Arcane::LocalTransform t; t.position = glm::vec2(80.0f, 0.0f);
        sim.Registry().AddComponent<Arcane::LocalTransform>(moon, t);
        sim.Registry().AddComponent<Arcane::WorldTransform>(moon, Arcane::WorldTransform{});
        Arcane::SpriteRenderer sr; sr.size = glm::vec2(20.0f); sr.tint = glm::vec4(0.4f, 0.8f, 1.0f, 1.0f);
        sim.Registry().AddComponent<Arcane::SpriteRenderer>(moon, sr);
        sim.Registry().SetParent(moon, orbiter);
    }
    sim.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{sceneRoot});
    // NOTE: no SetResource<TextureTable> -- it would force Astra to serialize a
    // raw-pointer map (compile error); RenderSubmissionSystem renders Rects when
    // the table is absent. All slice sprites use textureId 0.

    Arcane::RunLoop runLoop(sim.Registry(), sim.Schedulers());
    // Separate clock for sim dt so we don't consume lastFrameTime before the
    // input block reads it (input block updates lastFrameTime each frame).
    auto simPrev = std::chrono::steady_clock::now();
    double sceneTime = 0.0;

    nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();
    // Swapchain backbuffer framebuffer views; reset on resize.
    std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle>
        backbufferFramebuffers;
    auto lastShaderPoll = std::chrono::steady_clock::now();

    const auto start = std::chrono::steady_clock::now();
    auto lastTitleUpdate = start;
    // First-frame input dt is measured from here (just before the loop) so it
    // is ~0 rather than the full subsystem-init wall time.
    auto lastFrameTime = start;
    uint64_t frameCount = 0;
    uint64_t framesSinceTitle = 0;

    bool running = true;
    while (running)
    {
        auto events = window.PumpEvents();
        if (events.quitRequested)
            break;
        if (events.resized)
        {
            backbufferFramebuffers.clear();
            swapchain->Resize(events.width, events.height);
            canvas->Resize(swapchain->Width(), swapchain->Height());
        }
        if (window.IsMinimized())
        {
            // No rendering while minimized; don't burn a core when vsync
            // isn't throttling the loop. (std sleep, not SDL_Delay -- SDL
            // stays behind the Platform module.)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Input: sample SDL state, evaluate actions.  Must precede ImGui
        // BeginFrame so capture flags are set before the evaluator reads them.
        {
            const auto nowInput = std::chrono::steady_clock::now();
            const double frameDt =
                std::chrono::duration<double>(nowInput - lastFrameTime).count();
            lastFrameTime = nowInput;

            input->Update(frameDt,
                          inputDevices->Sample(imgui->WantCaptureKeyboard(),
                                               imgui->WantCaptureMouse()));
            if (input->Pressed("quit"))
                break;
            if (input->Pressed("toggle_stats"))
                showStats = !showStats;
            if (input->Pressed("swap_backend"))
                ARC_INFO("swap_backend pressed (runtime backend swap lands with the M3 demo proper)");
            // WASD / left stick nudges the orbit center: composites +
            // processors visibly live in the demo.
            const auto mv = input->Axis("move");
            moveOffset += glm::vec2(mv.x, mv.y) * (float)(300.0 * frameDt);
        }

        // Advance the M4 simulation: orbit the parent (inline, not an ECS system),
        // then RunLoop runs FixedUpdate (transform propagation).
        {
            const auto simNow = std::chrono::steady_clock::now();
            const double simDt =
                std::chrono::duration<double>(simNow - simPrev).count();
            simPrev = simNow;
            sceneTime += simDt;
            if (auto* lt = sim.Registry().GetComponent<Arcane::LocalTransform>(orbiter))
                lt->rotation = (float)sceneTime;
            runLoop.Advance(simDt > 0.25 ? 0.25 : simDt);
        }

        imgui->BeginFrame();
        if (showStats)
        {
            ImGui::Begin("Arcane Stats");
            ImGui::Text("Backend: %s", Arcane::ToString(device->Backend()));
            ImGui::Text("Adapter: %s", device->AdapterName().c_str());
            const Arcane::Batch2DStats lastStats = batcher->Stats();
            ImGui::Text("Quads: %u  Draws: %u", lastStats.quads, lastStats.drawCalls);
            ImGui::End();
        }

        nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
        if (!backbuffer)
            continue;

        // Hot reload: poll once a second; pipeline caches rebuild lazily.
        const auto now0 = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now0 - lastShaderPoll).count() >= 1.0)
        {
            shaders->Poll();
            lastShaderPoll = now0;
        }

        const double t = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        const float w = (float)canvas->Width();
        const float h = (float)canvas->Height();

        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                       nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));

        batcher->Begin(commandList, canvas->Framebuffer(),
                       canvas->Width(), canvas->Height());

        // Three bouncing rects (linear colors).
        for (int i = 0; i < 3; ++i)
        {
            const float phase = (float)i * 2.1f;
            const float x = (0.5f + 0.4f * (float)std::sin(t * 0.8 + phase)) * w;
            const float y = (0.5f + 0.4f * (float)std::cos(t * 1.1 + phase)) * h;
            const glm::vec4 colors[3] = { { 0.9f, 0.2f, 0.2f, 1.0f },
                                          { 0.2f, 0.9f, 0.3f, 1.0f },
                                          { 0.2f, 0.4f, 0.9f, 1.0f } };
            batcher->Rect(glm::vec2(x - 40.0f, y - 40.0f), glm::vec2(80.0f),
                          colors[i]);
        }

        // Orbiting circle + a connecting line leash.
        // moveOffset is nudged by WASD/stick each frame (input demo).
        const glm::vec2 middle(w * 0.5f + moveOffset.x, h * 0.5f + moveOffset.y);
        const glm::vec2 orbit = middle +
            glm::vec2((float)std::cos(t) * 0.3f * w, (float)std::sin(t) * 0.3f * h);
        batcher->Line(middle, orbit, 3.0f, glm::vec4(0.7f, 0.7f, 0.8f, 0.8f));
        batcher->Circle(orbit, 24.0f, glm::vec4(1.0f, 0.8f, 0.2f, 1.0f));

        // HDR swatch on a high sorting layer (always on top): linear value
        // up to 4.0 -- visibly rolls off to white through ACES instead of
        // hard-clipping. Proves the HDR shoulder is working.
        batcher->SetLayer(10, 0);
        const float hdr = 2.0f + 2.0f * (float)std::sin(t * 2.0);
        batcher->Rect(glm::vec2(20.0f, 20.0f), glm::vec2(120.0f, 60.0f),
                      glm::vec4(hdr, hdr, hdr, 1.0f));

        // HUD text line: MSDF at layer 10/orderInLayer 1 (same layer as the
        // HDR swatch but drawn on top of it via orderInLayer). Bottom-left.
        // Recording order: Draw -> Flush(commandList) -> End, per TextSystem
        // contract (Flush records the atlas upload before End records draws).
        batcher->SetLayer(10, 1);
        char hud[96];
        std::snprintf(hud, sizeof(hud), "Arcane M3 -- %s",
                      Arcane::ToString(device->Backend()));
        text->Draw(*batcher, hudFont, 22.0f, glm::vec2(20.0f, h - 24.0f),
                   hud, glm::vec4(0.9f, 0.9f, 1.0f, 1.0f));
        text->Flush(commandList);

        // M4: submit ECS sprites through the RunLoop render phase.
        // SetResource each frame so the batcher pointer stays valid (batcher
        // Begin/End frame state changes; the resource must track the live ptr).
        sim.Registry().SetResource<Arcane::RenderContext2D>(
            Arcane::RenderContext2D{batcher.get(), glm::vec2(0, 0)});
        runLoop.SubmitRender();

        batcher->End();

        // The ONLY writer of the display-referred backbuffer. Framebuffers
        // are cached per backbuffer texture (cleared on resize above).
        nvrhi::FramebufferHandle& backbufferFb = backbufferFramebuffers[backbuffer];
        if (!backbufferFb)
            backbufferFb = device->Nvrhi()->createFramebuffer(
                nvrhi::FramebufferDesc().addColorAttachment(backbuffer));
        tonemap->Run(commandList, canvas->Texture(), backbufferFb);

        // ImGui renders post-tonemap into the display-referred backbuffer.
        imgui->Render(commandList, backbufferFb);

        commandList->close();
        device->Nvrhi()->executeCommandList(commandList);

        swapchain->Present();

        ++frameCount;
        ++framesSinceTitle;  // incremented before the title block: never 0 when sinceTitle >= 0.5
        const auto now = std::chrono::steady_clock::now();
        const double sinceTitle =
            std::chrono::duration<double>(now - lastTitleUpdate).count();
        if (sinceTitle >= 0.5)
        {
            const double frameMs = sinceTitle * 1000.0 / (double)framesSinceTitle;
            const Arcane::Batch2DStats stats = batcher->Stats();
            char title[200];
            std::snprintf(title, sizeof(title),
                          "Arcane Playground -- %s -- %s -- %.2f ms -- %u quads / %u draws",
                          Arcane::ToString(device->Backend()),
                          device->AdapterName().c_str(), frameMs,
                          stats.quads, stats.drawCalls);
            window.SetTitle(title);
            lastTitleUpdate = now;
            framesSinceTitle = 0;
        }

        if (maxFrames != 0 && frameCount >= maxFrames)
            running = false;
    }

    ARC_INFO("Exiting after {} frames", frameCount);
    device->Nvrhi()->waitForIdle();
    return 0;
}
