// Arcane Playground -- M1: window + NVRHI device + clear + present.
// The clear color animates so a working present loop is visually obvious.
// Scripted verification: --frames N renders N frames and exits 0.
// The direct backbuffer clear is M1 scaffolding -- the M2 renderer
// replaces it; do not grow rendering helpers here.

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <nvrhi/nvrhi.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
    void PrintUsage()
    {
        std::printf(
            "Arcane Playground (M1: window + device + clear + present)\n"
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

    nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();

    const auto start = std::chrono::steady_clock::now();
    auto lastTitleUpdate = start;
    uint64_t frameCount = 0;
    uint64_t framesSinceTitle = 0;

    bool running = true;
    while (running)
    {
        auto events = window.PumpEvents();
        if (events.quitRequested)
            break;
        if (events.resized)
            swapchain->Resize(events.width, events.height);
        if (window.IsMinimized())
        {
            // No rendering while minimized; don't burn a core when vsync
            // isn't throttling the loop. (std sleep, not SDL_Delay -- SDL
            // stays behind the Platform module.)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
        if (!backbuffer)
            continue;

        const double t = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        const float r = 0.5f + 0.5f * (float)std::sin(t * 0.7);
        const float g = 0.5f + 0.5f * (float)std::sin(t * 0.9 + 2.0);
        const float b = 0.5f + 0.5f * (float)std::sin(t * 1.1 + 4.0);

        commandList->open();
        commandList->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                       nvrhi::Color(r, g, b, 1.0f));
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
            char title[160];
            std::snprintf(title, sizeof(title),
                          "Arcane Playground -- %s -- %s -- %.2f ms",
                          Arcane::ToString(device->Backend()),
                          device->AdapterName().c_str(), frameMs);
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
