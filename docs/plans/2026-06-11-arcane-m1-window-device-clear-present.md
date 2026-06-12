# Arcane M1 — Window + NVRHI Device (Both Backends) + Clear + Present — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up `Arcane.dll` (Base/Platform/Render modules), open an SDL3 window, create a real NVRHI device on BOTH backends (D3D12 + Vulkan), clear the backbuffer, and present — proven by `Playground.exe` and by headless/hidden-window GPU tests in ArcaneTests.

**Architecture:** New `Arcane` SharedLib project in the existing Arcane workspace, with one folder per module under a second namespaced include root (`Arcane/Arcane/src/Arcane/{Base,Platform,Render}`, consumed as `#include <Arcane/Platform/Window.hpp>` — relative paths are disjoint from Core's root, so both roots coexist on the include path). NVRHI + SDL3 link INTO the DLL; consumers (ArcaneTests, Playground) link only the Arcane import lib and use headers. Per-backend device + swapchain live in one TU each (`DeviceD3D12.cpp`, `DeviceVulkan.cpp`) behind abstract `RenderDevice` / `Swapchain` interfaces. The Vulkan-Hpp dispatcher storage TU moves from Tests into the DLL's Render module (the M0 contract).

**Tech Stack:** SDL3 (vcpkg static-md), NVRHI (vendored wrapper, D3D12+Vulkan), Vulkan-Headers (vulkan.hpp dynamic dispatch), DirectX-Headers, spdlog, Catch2.

**Spec:** `docs/superpowers/specs/2026-06-11-engine-architecture-design.md` (M1 bullet of the bring-up order, workspace layout, module responsibilities) + `docs/superpowers/specs/2026-06-10-engine-thirdparty-stack-design.md` (NVRHI/SDL3 integration mechanics).

---

## Decisions made by this plan (deviations and interpretations — flagged for review)

1. **M1 introduces the `Arcane` SharedLib project.** Forced by the standing M0 contract: "the dispatcher TU moves into Arcane.dll's Render module in M1" (CLAUDE.md + VulkanDispatchStorage.cpp comment). Playground links the engine; it does not re-implement device code.
2. **No `Arcane::RunLoop` yet.** The spec ties RunLoop to the hosts (Loom/Grimoire, M4+). Playground owns its ~100-line loop — it converts to a plugin in M4 anyway. YAGNI.
3. **M1 frame pacing = one frame in flight.** `Swapchain::Present()` calls `waitForIdle()` + `runGarbageCollection()` every frame. Correct-first, zero semaphore-lifetime hazards for a clear-only milestone; real pacing arrives with the M2 renderer. This also makes single acquire/present semaphores safe to reuse on Vulkan.
4. **ArcaneTests links Arcane.dll; the NVRHI/SDL3 vendor link-smokes are removed.** Real device creation + window creation through the engine supersede the M0 arrival gates (their stated purpose). Tests drop their direct nvrhi/SDL3/d3d12 links; glm/stb/miniaudio/Astra/enkiTS/FreeType smokes stay (those deps enter the DLL in M2/M3). Tests still link Core statically — Core is header-only, and no Core state crosses the test↔DLL boundary; the one-Core-copy-per-process rule is a *game-runtime* rule enforced from M4's EngineContext.
5. **GPU tests are tagged `[gpu]`, run by default.** Jenkins agent `windows-1` has the `gpu` label and an interactive session, so device/window tests run in CI as-is (no Jenkinsfile change). Machines without a backend can exclude with `ArcaneTests.exe ~[gpu]`. Tests are strict — no auto-SKIP that could mask CI breakage.
6. **Engine logging is `Base/Log` (new, tiny), not Core's `Logger`.** Core's Logger is server-flavored (writes `logs/gacha_server.log`, categories Auth/Gacha/...). The spec assigns "logging (spdlog)" to the DLL's Base module. M1 ships a console-sink engine logger + `ARC_*` macros; file sinks/categories can grow later.
7. **Swapchain format BGRA8_UNORM, 3 backbuffers, vsync default on** (`--no-vsync` to disable). FIFO when vsync on Vulkan; MAILBOX-else-IMMEDIATE when off.
8. **Headless device creation is supported** (`RenderDevice::Create` takes no window; swapchain is a separate `CreateSwapchain(window, vsync)` call) so device tests need no window and future tools can run compute-only.
9. **No exported class carries STL members** (interfaces are pure-virtual; concrete classes are internal). If C4251 ever fires, fix the design rather than silencing the warning.
10. **Playground gets `--backend dx12|vulkan`, `--frames N`, `--no-vsync`** so both backends are verifiable scripted (run N frames, exit 0) — CI-able later without a human watching.

## Rendering-pipeline foundation contracts (binding for M1 code and all reviews)

M1 is clear+present, but it lays the device/swapchain layer an AAA 2D pipeline (M2's
batcher/canvas/shader system, M3's render-graph-fed scene) builds on. These contracts
keep the M1 shortcuts from leaking into the architecture:

1. **Backend isolation is absolute.** Raw D3D12/Vulkan/DXGI calls exist ONLY inside
   `DeviceD3D12.cpp` / `DeviceVulkan.cpp`. Everything above the `RenderDevice` /
   `Swapchain` interfaces speaks `nvrhi::IDevice`/`nvrhi::ICommandList` exclusively.
   This is what makes M3's runtime backend swap (and device-lost recovery) tractable.
2. **Present() promises presentation, not synchronization.** The idle-after-present
   M1 pacing is an implementation detail inside the swapchains. No caller may assume
   the GPU is idle after `Present()`; anything needing sync calls `waitForIdle()`
   explicitly. M2 replaces the pacing with frames-in-flight WITHOUT changing the
   `Swapchain` interface — if an interface change is needed, that's a design failure
   to flag now.
3. **The backbuffer is display-referred output only.** `BGRA8_UNORM` is the raw
   output surface. The real pipeline (M2+) renders linear/HDR offscreen and a
   tonemap/output pass writes the backbuffer — mirroring the client's landed
   linear+ACES pipeline. Nothing in M1 may bake sRGB/gamma assumptions anywhere
   except "the swapchain format is a swapchain detail" (`Swapchain::Format()`).
4. **One canonical submission path (homogenized-rendering mandate).** Playground's
   direct `clearTextureFloat` on the backbuffer is M1 scaffolding, replaced by the
   M2 renderer — never extended. No bespoke per-consumer render chains grow out of
   M1 code.
5. **Resource-state discipline from day one.** All textures use explicit
   `ResourceStates` + `keepInitialState(true)` conventions; the NVRHI validation
   layer runs in every Debug test and must stay silent. Validation noise is a
   failing test, not a warning.
6. **Everything per-frame tolerates swapchain recreation.** Resize, out-of-date,
   minimize are first-class paths (tested), because backend swap and device-lost
   recovery reuse exactly these paths (stack-spec verification requirement).

## File structure

```
Arcane/
├── premake5.lua                              MODIFIED — Arcane (SharedLib) + Playground projects; ArcaneTests rewired
├── Arcane/                                   NEW — Arcane.dll project dir
│   └── src/Arcane/
│       ├── Base/Api.hpp                      NEW — ARCANE_API export/import macro
│       ├── Base/Log.hpp, Log.cpp             NEW — engine logger (spdlog console) + ARC_* macros
│       ├── Base/Engine.hpp, Engine.cpp       NEW — BuildInfo() export (DLL liveness probe)
│       ├── Platform/Window.hpp, Window.cpp   NEW — SDL3 window + event pump
│       ├── Render/Device.hpp                 NEW — GraphicsBackend, RenderDeviceDesc, RenderDevice
│       ├── Render/Swapchain.hpp              NEW — Swapchain interface (Task 5)
│       ├── Render/DeviceFactories.hpp        NEW — internal: per-backend factory decls
│       ├── Render/NvrhiMessageCallback.hpp   NEW — internal: nvrhi message -> ARC_* bridge
│       ├── Render/Device.cpp                 NEW — Create() dispatch + ToString
│       ├── Render/DeviceD3D12.cpp            NEW — DXGI/D3D12 device (Task 3) + swapchain (Task 5)
│       ├── Render/DeviceVulkan.cpp           NEW — Vulkan device (Task 4) + swapchain (Task 6)
│       └── Render/VulkanDispatchStorage.cpp  MOVED from Tests/src (Task 3)
├── Playground/
│   └── src/main.cpp                          NEW — clear+present demo (Task 7)
├── Core/src/Arcane/Version.hpp               MODIFIED — version string "(M0)" -> "(M1)"
└── Tests/src/
    ├── EngineExportTest.cpp                  NEW — DLL export liveness (Task 1)
    ├── WindowTest.cpp                        NEW — hidden window create/pump (Task 2)
    ├── Helpers/GpuTestHelpers.hpp            NEW — offscreen clear + CPU readback helper (Task 3)
    ├── DeviceTest.cpp                        NEW — [gpu] device creation, both backends (Tasks 3-4)
    ├── SwapchainTest.cpp                     NEW — [gpu] windowed clear/present/resize (Tasks 5-6)
    ├── VendorSmokeTest.cpp                   MODIFIED — NVRHI + SDL3 sections removed (Task 3)
    └── VulkanDispatchStorage.cpp             DELETED — moved into the DLL (Task 3)
CLAUDE.md                                     MODIFIED — Arcane section reflects M1 (Task 8)
```

## Constraints carried into every task

- All new/edited files: **UTF-8 without BOM, ASCII-only comments**. Prefer the Write/Edit tools; never PowerShell `Out-File` without `-Encoding utf8`.
- No `/fp:fast` anywhere; don't override the MSVC default (`/fp:precise`).
- **Never run `db-reset.bat`, `clean.bat --deep`, or `docker compose down -v`** in any verification step (standing user rule).
- "Impossible" build errors after header moves → full rebuild (`msbuild ... /t:Rebuild`) before debugging (stale-object ODR lesson).
- Commit after every task; `type(scope):` message convention.
- Build/test loop used throughout (run from repo root unless noted):
  ```bat
  cd Arcane
  GenerateProjects.bat
  msbuild Arcane.slnx /p:Configuration=Debug /m
  bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
  ```
- The dev box and CI agent `windows-1` both have an NVIDIA discrete GPU with D3D12 + Vulkan; a `[gpu]` test failure is a real failure, not an environment shrug.

---

### Task 1: Arcane.dll project skeleton — ARCANE_API, Base/Log, BuildInfo export

The DLL exists, exports one function, ArcaneTests loads it and calls it. Proves: SharedLib premake config, export/import macro, DLL copy step, import-lib linking — before any graphics code.

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Base/Api.hpp`
- Create: `Arcane/Arcane/src/Arcane/Base/Log.hpp`, `Arcane/Arcane/src/Arcane/Base/Log.cpp`
- Create: `Arcane/Arcane/src/Arcane/Base/Engine.hpp`, `Arcane/Arcane/src/Arcane/Base/Engine.cpp`
- Create: `Arcane/Tests/src/EngineExportTest.cpp`
- Modify: `Arcane/premake5.lua`
- Modify: `Arcane/Core/src/Arcane/Version.hpp`

- [ ] **Step 1: Write `Arcane/Arcane/src/Arcane/Base/Api.hpp`**

```cpp
#pragma once

// ARCANE_API: dllexport when building Arcane.dll, dllimport for consumers
// (Loom, Grimoire, Playground, Game.dll, ArcaneTests). The only C surface
// in the architecture is the plugin entry-point set (M4); everything
// marked ARCANE_API is direct same-toolchain C++ linkage by design.

#if defined(_WIN32)
    #if defined(ARCANE_BUILD_DLL)
        #define ARCANE_API __declspec(dllexport)
    #else
        #define ARCANE_API __declspec(dllimport)
    #endif
#else
    #define ARCANE_API __attribute__((visibility("default")))
#endif
```

- [ ] **Step 2: Write `Arcane/Arcane/src/Arcane/Base/Log.hpp`**

```cpp
#pragma once

// Engine logger (Base module): console-sink spdlog logger named "Arcane".
// Deliberately separate from Core's server-flavored Logger (which writes
// logs/gacha_server.log with Auth/Gacha/... categories). File sinks and
// per-module categories can grow here when the engine needs them.

#include <Arcane/Base/Api.hpp>

#include <spdlog/spdlog.h>

namespace Arcane::Log
{
    ARCANE_API void Init(spdlog::level::level_enum level = spdlog::level::info);
    ARCANE_API void Shutdown();

    // Never returns null: lazily calls Init() with defaults if needed.
    ARCANE_API spdlog::logger* Engine();
}

#define ARC_TRACE(...)    ::Arcane::Log::Engine()->trace(__VA_ARGS__)
#define ARC_DEBUG(...)    ::Arcane::Log::Engine()->debug(__VA_ARGS__)
#define ARC_INFO(...)     ::Arcane::Log::Engine()->info(__VA_ARGS__)
#define ARC_WARN(...)     ::Arcane::Log::Engine()->warn(__VA_ARGS__)
#define ARC_ERROR(...)    ::Arcane::Log::Engine()->error(__VA_ARGS__)
#define ARC_CRITICAL(...) ::Arcane::Log::Engine()->critical(__VA_ARGS__)
```

- [ ] **Step 3: Write `Arcane/Arcane/src/Arcane/Base/Log.cpp`**

```cpp
#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>

namespace Arcane::Log
{
    namespace
    {
        std::shared_ptr<spdlog::logger> s_engine;
    }

    void Init(spdlog::level::level_enum level)
    {
        if (s_engine)
            return;
        s_engine = spdlog::stdout_color_mt("Arcane");
        s_engine->set_level(level);
        s_engine->set_pattern("%^[%H:%M:%S.%e] [%n] [%l]%$ %v");
    }

    void Shutdown()
    {
        if (s_engine)
        {
            spdlog::drop("Arcane");
            s_engine.reset();
        }
    }

    spdlog::logger* Engine()
    {
        if (!s_engine)
            Init();
        return s_engine.get();
    }
}
```

- [ ] **Step 4: Write `Arcane/Arcane/src/Arcane/Base/Engine.hpp`**

```cpp
#pragma once

#include <Arcane/Base/Api.hpp>

namespace Arcane
{
    // Version + build flavor of the loaded engine DLL. Doubles as the
    // simplest possible export for proving the DLL boundary works.
    ARCANE_API const char* BuildInfo();
}
```

- [ ] **Step 5: Write `Arcane/Arcane/src/Arcane/Base/Engine.cpp`**

```cpp
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Version.hpp>

namespace Arcane
{
    const char* BuildInfo()
    {
#if defined(ARCANE_DEBUG)
        return "Arcane 0.1 (M1) [Debug]";
#elif defined(ARCANE_RELEASE)
        return "Arcane 0.1 (M1) [Release]";
#else
        return "Arcane 0.1 (M1) [Dist]";
#endif
    }
}
```

- [ ] **Step 6: Bump `Arcane/Core/src/Arcane/Version.hpp`**

Change the `VersionString()` body:

```cpp
    inline const char* VersionString() { return "Arcane 0.1 (M1)"; }
```

(`VersionTest.cpp` checks `kVersionMajor == 0` and non-empty string — unaffected.)

- [ ] **Step 7: Add the `Arcane` project to `Arcane/premake5.lua`**

Insert between the `Core` project and the `ArcaneTests` project. This is the project's FINAL form for M1 — the nvrhi/SDL3 links and Vulkan defines are used from Task 2 onward; adding them once now avoids three rounds of premake churn (unreferenced static-lib content is simply not pulled in by the linker):

```lua
-- ============================================================================
-- Arcane: the engine DLL. One DLL, modular inside by folder/namespace
-- (Base, Platform, Render for M1; Audio/Text/Assets/UI/Jobs/Plugin later).
-- NVRHI and SDL3 link INTO this DLL; consumers link only the import lib.
-- Second namespaced include root: src/Arcane/{Base,Platform,Render} --
-- relative paths are disjoint from Core's root (Net/Crypto/Types/Util),
-- so <Arcane/...> resolves unambiguously across both.
-- ============================================================================
project "Arcane"
    location "Arcane"
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.Core}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.VulkanHeaders}",
        "%{IncludeDir.DirectXHeaders}",
        "%{IncludeDir.DirectXHeaders}/directx",
        "%{IncludeDir.SDL3}",
    }

    links { "Core", "nvrhi" }

    defines {
        "ARCANE_BUILD_DLL",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        defines { "VK_USE_PLATFORM_WIN32_KHR" }
        -- d3d12/dxgi/dxguid: D3D12 backend. SDL3-static + system libs:
        -- the platform layer (list mirrors SDL3's pkgconfig Libs line).
        links {
            "d3d12", "dxgi", "dxguid",
            "SDL3-static",
            "user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
            "version", "uuid", "advapi32", "setupapi", "shell32", "dinput8",
        }

    filter { "system:windows", "configurations:Debug" }
        libdirs { VCPKG_INSTALLED_MD .. "/debug/lib" }
    filter { "system:windows", "configurations:Release or configurations:Dist" }
        libdirs { VCPKG_INSTALLED_MD .. "/lib" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"
```

- [ ] **Step 8: Wire ArcaneTests to the DLL in `Arcane/premake5.lua`**

In the `ArcaneTests` project:

**(a)** Add the DLL's include root to `includedirs` (after `"%{IncludeDir.Core}",`):

```lua
        "%{wks.location}/Arcane/src",
```

**(b)** Change the links line to add `"Arcane"`:

```lua
    links { "Core", "Arcane", "Catch2", "rapidcheck", "enkiTS", "freetype", "nvrhi" }
```

**(c)** Add a postbuild DLL copy (after the `links` block, before `defines`):

```lua
    -- The test exe loads Arcane.dll from its own directory.
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
    }
```

- [ ] **Step 9: Write the failing test `Arcane/Tests/src/EngineExportTest.cpp`**

```cpp
// Proves the Arcane.dll boundary: an exported function is callable through
// the import lib and the DLL actually loads at run time.

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Version.hpp>

TEST_CASE("engine dll: BuildInfo exports and matches the Core version line", "[engine]")
{
    const char* info = Arcane::BuildInfo();
    REQUIRE(info != nullptr);
    REQUIRE(std::strlen(info) > 0);
    // Same milestone tag as Core's VersionString.
    REQUIRE(std::strstr(info, "(M1)") != nullptr);
    REQUIRE(std::strstr(Arcane::VersionString(), "(M1)") != nullptr);
}
```

- [ ] **Step 10: Generate, build all three configurations, run tests**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```

Expected: all configs build; `Arcane.dll` + `Arcane.lib` appear in `bin/Debug-windows-x86_64-md/Arcane/`; the DLL is copied next to `ArcaneTests.exe`; all tests pass including the new `[engine]` case. If the test crashes at startup with "DLL not found", the Step 8(c) copy path is wrong.

- [ ] **Step 11: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): M1 Arcane.dll skeleton - ARCANE_API, Base/Log, BuildInfo export"
```

---

### Task 2: Platform module — SDL3 window + event pump

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Platform/Window.hpp`
- Create: `Arcane/Arcane/src/Arcane/Platform/Window.cpp`
- Create: `Arcane/Tests/src/WindowTest.cpp`

- [ ] **Step 1: Write the failing test `Arcane/Tests/src/WindowTest.cpp`**

```cpp
// Platform smoke: a hidden SDL3 window creates, reports its pixel size and
// Win32 handle, and survives one event pump. Hidden so it runs on the CI
// agent without flashing windows.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Platform/Window.hpp>

TEST_CASE("window: hidden window creates, reports size and native handle", "[platform]")
{
    Arcane::Window window;
    Arcane::WindowDesc desc;
    desc.title  = "ArcaneTests hidden window";
    desc.width  = 640;
    desc.height = 360;
    desc.hidden = true;
    REQUIRE(window.Create(desc));

    uint32_t w = 0, h = 0;
    window.GetPixelSize(w, h);
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(window.NativeHandle() != nullptr);
    REQUIRE_FALSE(window.IsMinimized());

    auto events = window.PumpEvents();
    REQUIRE_FALSE(events.quitRequested);

    window.Destroy();
}
```

- [ ] **Step 2: Run it to verify it fails**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `Arcane/Platform/Window.hpp` does not exist.

- [ ] **Step 3: Write `Arcane/Arcane/src/Arcane/Platform/Window.hpp`**

```cpp
#pragma once

// Platform module: SDL3 window + event pump. M1 surfaces only what the
// host loop needs (quit, resize); the input-action layer
// (input_actions.json semantics) is a later milestone.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <string>

struct SDL_Window;

namespace Arcane
{
    struct WindowDesc
    {
        std::string title = "Arcane";
        uint32_t width    = 1280;
        uint32_t height   = 720;
        bool resizable    = true;
        bool hidden       = false;  // tests create hidden windows
        bool vulkan       = false;  // SDL_WINDOW_VULKAN: required before a
                                    // VkSurfaceKHR can be created on it
    };

    struct WindowEvents
    {
        bool quitRequested = false;  // SDL_EVENT_QUIT, window close, or ESC
        bool resized       = false;  // pixel size changed since last pump
        uint32_t width     = 0;      // valid when resized
        uint32_t height    = 0;
    };

    class ARCANE_API Window
    {
    public:
        Window() = default;
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        bool Create(const WindowDesc& desc);
        void Destroy();

        WindowEvents PumpEvents();

        void SetTitle(const std::string& title);
        void GetPixelSize(uint32_t& width, uint32_t& height) const;
        bool IsMinimized() const;

        void* NativeHandle() const;                       // HWND on Windows
        SDL_Window* SdlWindow() const { return m_window; }

    private:
        SDL_Window* m_window = nullptr;
    };
}
```

- [ ] **Step 4: Write `Arcane/Arcane/src/Arcane/Platform/Window.cpp`**

```cpp
#include <Arcane/Platform/Window.hpp>

#include <Arcane/Base/Log.hpp>

#include <SDL3/SDL.h>

namespace Arcane
{
    bool Window::Create(const WindowDesc& desc)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            ARC_ERROR("SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = 0;
        if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
        if (desc.hidden)    flags |= SDL_WINDOW_HIDDEN;
        if (desc.vulkan)    flags |= SDL_WINDOW_VULKAN;

        m_window = SDL_CreateWindow(desc.title.c_str(),
                                    (int)desc.width, (int)desc.height, flags);
        if (!m_window)
        {
            ARC_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }

        ARC_INFO("Window created: '{}' {}x{}", desc.title, desc.width, desc.height);
        return true;
    }

    void Window::Destroy()
    {
        if (m_window)
        {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    Window::~Window()
    {
        Destroy();
    }

    WindowEvents Window::PumpEvents()
    {
        WindowEvents events;
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_EVENT_QUIT:
                events.quitRequested = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (e.window.windowID == SDL_GetWindowID(m_window))
                    events.quitRequested = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE)
                    events.quitRequested = true;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (e.window.windowID == SDL_GetWindowID(m_window))
                {
                    events.resized = true;
                    events.width   = (uint32_t)e.window.data1;
                    events.height  = (uint32_t)e.window.data2;
                }
                break;
            default:
                break;
            }
        }
        return events;
    }

    void Window::SetTitle(const std::string& title)
    {
        SDL_SetWindowTitle(m_window, title.c_str());
    }

    void Window::GetPixelSize(uint32_t& width, uint32_t& height) const
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        width  = (uint32_t)w;
        height = (uint32_t)h;
    }

    bool Window::IsMinimized() const
    {
        return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
    }

    void* Window::NativeHandle() const
    {
        return SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                      SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    }
}
```

- [ ] **Step 5: Build and run the tests**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Expected: PASS, including the new `[platform]` case. (SDL3-static was already linked into the DLL by Task 1's project block; the existing SDL3 vendor smoke in the test exe is untouched until Task 3.)

- [ ] **Step 6: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): Platform module - SDL3 window + event pump"
```

---

### Task 3: Render module — device API, D3D12 device, tests shed direct graphics links

The `RenderDevice` interface, the D3D12 implementation (headless — swapchain is Task 5), the nvrhi→log bridge, and the test-side rewire: the dispatcher TU moves into the DLL, the NVRHI/SDL3 link-smokes are removed (superseded by real device/window tests), and ArcaneTests drops its direct nvrhi/SDL3/d3d12 links.

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Render/Device.hpp`
- Create: `Arcane/Arcane/src/Arcane/Render/DeviceFactories.hpp`
- Create: `Arcane/Arcane/src/Arcane/Render/NvrhiMessageCallback.hpp`
- Create: `Arcane/Arcane/src/Arcane/Render/Device.cpp`
- Create: `Arcane/Arcane/src/Arcane/Render/DeviceD3D12.cpp`
- Move: `Arcane/Tests/src/VulkanDispatchStorage.cpp` → `Arcane/Arcane/src/Arcane/Render/VulkanDispatchStorage.cpp`
- Create: `Arcane/Tests/src/Helpers/GpuTestHelpers.hpp`
- Create: `Arcane/Tests/src/DeviceTest.cpp`
- Modify: `Arcane/Tests/src/VendorSmokeTest.cpp`
- Modify: `Arcane/premake5.lua` (ArcaneTests project only)

- [ ] **Step 1: Write the failing test `Arcane/Tests/src/DeviceTest.cpp`**

```cpp
// Real GPU device creation through the engine, headless (no window, no
// swapchain). The offscreen clear+readback helper proves end-to-end GPU
// work: command list, clear, copy to staging, CPU map, byte check.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Device.hpp>

#include "Helpers/GpuTestHelpers.hpp"

TEST_CASE("d3d12: headless device creates and clears an offscreen target", "[gpu][d3d12]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    REQUIRE(device->Nvrhi() != nullptr);
    REQUIRE_FALSE(device->AdapterName().empty());
    CHECK(std::string(Arcane::ToString(device->Backend())) == "D3D12");

    CheckOffscreenClear(*device);
}
```

- [ ] **Step 2: Write `Arcane/Tests/src/Helpers/GpuTestHelpers.hpp`**

```cpp
#pragma once

// Shared GPU test helper: clears a 4x4 offscreen RGBA8 target to a known
// color, copies it to a staging texture, maps it on the CPU, and asserts
// the bytes. The strongest headless proof that a device does real work.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

#include <Arcane/Render/Device.hpp>

inline void CheckOffscreenClear(Arcane::RenderDevice& device)
{
    nvrhi::IDevice* nv = device.Nvrhi();

    auto targetDesc = nvrhi::TextureDesc()
        .setWidth(4)
        .setHeight(4)
        .setFormat(nvrhi::Format::RGBA8_UNORM)
        .setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::RenderTarget)
        .setKeepInitialState(true)
        .setDebugName("ClearTarget");
    nvrhi::TextureHandle target = nv->createTexture(targetDesc);
    REQUIRE(target != nullptr);

    auto stagingDesc = nvrhi::TextureDesc()
        .setWidth(4)
        .setHeight(4)
        .setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDebugName("ClearReadback");
    nvrhi::StagingTextureHandle staging =
        nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    REQUIRE(staging != nullptr);

    nvrhi::CommandListHandle commandList = nv->createCommandList();
    commandList->open();
    commandList->clearTextureFloat(target, nvrhi::AllSubresources,
                                   nvrhi::Color(1.0f, 0.5f, 0.25f, 1.0f));
    commandList->copyTexture(staging, nvrhi::TextureSlice(),
                             target, nvrhi::TextureSlice());
    commandList->close();
    nv->executeCommandList(commandList);
    nv->waitForIdle();

    size_t rowPitch = 0;
    const auto* pixels = static_cast<const uint8_t*>(nv->mapStagingTexture(
        staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    REQUIRE(pixels != nullptr);
    // UNORM bytes for (1.0, 0.5, 0.25, 1.0): 255, ~128, ~64, 255.
    // +-1 tolerance on the rounded channels (UNORM round-to-nearest).
    CHECK((int)pixels[0] == 255);
    CHECK(std::abs((int)pixels[1] - 128) <= 1);
    CHECK(std::abs((int)pixels[2] - 64) <= 1);
    CHECK((int)pixels[3] == 255);
    nv->unmapStagingTexture(staging);
    nv->runGarbageCollection();
}
```

- [ ] **Step 3: Write `Arcane/Arcane/src/Arcane/Render/Device.hpp`**

```cpp
#pragma once

// Render module: the engine's GPU device. Creation is headless by design --
// a swapchain is created separately against a Window (Task 5), so tools
// and tests can run compute/offscreen work without any window.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Arcane
{
    enum class GraphicsBackend : uint8_t
    {
        D3D12,
        Vulkan,
    };

    ARCANE_API const char* ToString(GraphicsBackend backend);

    struct RenderDeviceDesc
    {
        GraphicsBackend backend = GraphicsBackend::D3D12;
#if defined(ARCANE_DEBUG)
        bool enableValidation = true;   // D3D12 debug layer / VK validation
#else                                   // layer + the NVRHI validation layer
        bool enableValidation = false;
#endif
    };

    class ARCANE_API RenderDevice
    {
    public:
        // Returns null on failure (no adapter, missing runtime, ...);
        // the failure reason is logged via ARC_ERROR.
        static std::unique_ptr<RenderDevice> Create(const RenderDeviceDesc& desc);

        virtual ~RenderDevice() = default;

        virtual GraphicsBackend Backend() const = 0;
        virtual nvrhi::IDevice* Nvrhi() const = 0;
        virtual std::string AdapterName() const = 0;
    };
}
```

- [ ] **Step 4: Write `Arcane/Arcane/src/Arcane/Render/DeviceFactories.hpp`**

```cpp
#pragma once

// Internal to the Render module: per-backend factory functions implemented
// in DeviceD3D12.cpp / DeviceVulkan.cpp, dispatched by RenderDevice::Create.
// Not part of the engine's public API surface.

#include <Arcane/Render/Device.hpp>

#include <memory>

namespace Arcane
{
    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc);
}
```

(Task 4 appends the `CreateDeviceVulkan` declaration.)

- [ ] **Step 5: Write `Arcane/Arcane/src/Arcane/Render/NvrhiMessageCallback.hpp`**

```cpp
#pragma once

// Internal: routes NVRHI's diagnostics (including the validation layer's)
// into the engine log.

#include <Arcane/Base/Log.hpp>

#include <nvrhi/nvrhi.h>

namespace Arcane
{
    class NvrhiMessageCallback final : public nvrhi::IMessageCallback
    {
    public:
        static NvrhiMessageCallback& Instance()
        {
            static NvrhiMessageCallback s_instance;
            return s_instance;
        }

        void message(nvrhi::MessageSeverity severity, const char* messageText) override
        {
            switch (severity)
            {
            case nvrhi::MessageSeverity::Info:
                ARC_INFO("[nvrhi] {}", messageText);
                break;
            case nvrhi::MessageSeverity::Warning:
                ARC_WARN("[nvrhi] {}", messageText);
                break;
            case nvrhi::MessageSeverity::Error:
                ARC_ERROR("[nvrhi] {}", messageText);
                break;
            case nvrhi::MessageSeverity::Fatal:
                ARC_CRITICAL("[nvrhi] {}", messageText);
                break;
            }
        }
    };
}
```

- [ ] **Step 6: Write `Arcane/Arcane/src/Arcane/Render/Device.cpp`**

```cpp
#include <Arcane/Render/Device.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceFactories.hpp>

namespace Arcane
{
    const char* ToString(GraphicsBackend backend)
    {
        switch (backend)
        {
        case GraphicsBackend::D3D12:  return "D3D12";
        case GraphicsBackend::Vulkan: return "Vulkan";
        }
        return "Unknown";
    }

    std::unique_ptr<RenderDevice> RenderDevice::Create(const RenderDeviceDesc& desc)
    {
        switch (desc.backend)
        {
        case GraphicsBackend::D3D12:
            return CreateDeviceD3D12(desc);
        default:
            ARC_ERROR("RenderDevice::Create: backend {} not available",
                      ToString(desc.backend));
            return nullptr;
        }
    }
}
```

(Task 4 replaces the `default:` arm with the Vulkan case.)

- [ ] **Step 7: Write `Arcane/Arcane/src/Arcane/Render/DeviceD3D12.cpp`**

```cpp
// D3D12 backend: DXGI factory + adapter + device + direct queue, wrapped
// by nvrhi::d3d12::createDevice. Headless here; the swapchain half of this
// TU arrives with the windowed milestone task.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Arcane
{
    namespace
    {
        class DeviceD3D12 final : public RenderDevice
        {
        public:
            bool Init(const RenderDeviceDesc& desc);

            GraphicsBackend Backend() const override { return GraphicsBackend::D3D12; }
            nvrhi::IDevice* Nvrhi() const override { return m_nvrhi.Get(); }
            std::string AdapterName() const override { return m_adapterName; }

            IDXGIFactory6* Factory() const { return m_factory.Get(); }
            ID3D12CommandQueue* GraphicsQueue() const { return m_graphicsQueue.Get(); }

        private:
            // Declaration order is destruction order in reverse: the nvrhi
            // device must release its D3D12 references before the queue,
            // device, adapter, and factory go away.
            ComPtr<IDXGIFactory6>      m_factory;
            ComPtr<IDXGIAdapter1>      m_adapter;
            ComPtr<ID3D12Device>       m_device;
            ComPtr<ID3D12CommandQueue> m_graphicsQueue;
            nvrhi::DeviceHandle        m_nvrhi;
            std::string                m_adapterName;
        };

        bool DeviceD3D12::Init(const RenderDeviceDesc& desc)
        {
            UINT factoryFlags = 0;
            if (desc.enableValidation)
            {
                ComPtr<ID3D12Debug> debug;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                {
                    debug->EnableDebugLayer();
                    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                }
                else
                {
                    ARC_WARN("D3D12 debug layer unavailable; continuing without it");
                }
            }

            if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory))))
            {
                ARC_ERROR("CreateDXGIFactory2 failed");
                return false;
            }

            if (FAILED(m_factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&m_adapter))))
            {
                ARC_ERROR("No DXGI adapter found");
                return false;
            }

            DXGI_ADAPTER_DESC1 adapterDesc{};
            m_adapter->GetDesc1(&adapterDesc);
            char name[128]{};
            size_t converted = 0;
            wcstombs_s(&converted, name, adapterDesc.Description, _TRUNCATE);
            m_adapterName = name;

            if (FAILED(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                         IID_PPV_ARGS(&m_device))))
            {
                ARC_ERROR("D3D12CreateDevice failed (feature level 12_0)");
                return false;
            }

            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(m_device->CreateCommandQueue(&queueDesc,
                                                    IID_PPV_ARGS(&m_graphicsQueue))))
            {
                ARC_ERROR("CreateCommandQueue failed");
                return false;
            }

            nvrhi::d3d12::DeviceDesc nvrhiDesc;
            nvrhiDesc.errorCB = &NvrhiMessageCallback::Instance();
            nvrhiDesc.pDevice = m_device.Get();
            nvrhiDesc.pGraphicsCommandQueue = m_graphicsQueue.Get();
            m_nvrhi = nvrhi::d3d12::createDevice(nvrhiDesc);
            if (!m_nvrhi)
            {
                ARC_ERROR("nvrhi::d3d12::createDevice failed");
                return false;
            }

            if (desc.enableValidation)
                m_nvrhi = nvrhi::validation::createValidationLayer(m_nvrhi);

            ARC_INFO("D3D12 device created on '{}'", m_adapterName);
            return true;
        }
    }

    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc)
    {
        auto device = std::make_unique<DeviceD3D12>();
        if (!device->Init(desc))
            return nullptr;
        return device;
    }
}
```

- [ ] **Step 8: Move the dispatcher TU into the DLL**

```bash
git mv Arcane/Tests/src/VulkanDispatchStorage.cpp Arcane/Arcane/src/Arcane/Render/VulkanDispatchStorage.cpp
```

Then update its comment block (the defines and the storage macro line stay byte-identical):

```cpp
// Vulkan-Hpp default dynamic-dispatcher storage. NVRHI's static-lib build
// (no NVRHI_SHARED_LIBRARY_BUILD) follows the standard Vulkan-Hpp contract:
// each module that statically links vulkan.hpp code defines this storage in
// exactly one TU. Arcane.dll is that module -- the one-engine-instance-per-
// process owner (moved here from ArcaneTests in M1, per the M0 plan).
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
```

- [ ] **Step 9: Remove the superseded vendor smokes from `Arcane/Tests/src/VendorSmokeTest.cpp`**

Delete the entire `// --- NVRHI` section (the `#include <nvrhi/nvrhi.h>`, `<nvrhi/d3d12.h>`, `<nvrhi/vulkan.h>` includes and the `"NVRHI: both backend factories link"` test case) and the entire `// --- SDL3` section (the `<SDL3/SDL_version.h>` include and the `"SDL3: links and reports its version"` test case). Both are superseded: DeviceTest creates real NVRHI devices through the DLL, and WindowTest creates a real SDL3 window through the DLL. The glm/stb/miniaudio/Astra/enkiTS/FreeType sections stay untouched (those deps enter the DLL in M2/M3).

- [ ] **Step 10: Drop the direct graphics links from ArcaneTests in `Arcane/premake5.lua`**

In the `ArcaneTests` project:

**(a)** `links` — remove `"nvrhi"`:

```lua
    links { "Core", "Arcane", "Catch2", "rapidcheck", "enkiTS", "freetype" }
```

**(b)** Windows `links` filter — remove `"d3d12", "dxgi", "dxguid"`, `"SDL3-static"`, and the SDL system libs; only the Core socket dep remains:

```lua
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        links { "ws2_32" }  -- Core TcpSocket
```

**(c)** Remove both `libdirs` filter blocks (they existed only for SDL3).

**(d)** `includedirs` — remove `"%{IncludeDir.SDL3}"`, `"%{IncludeDir.VulkanHeaders}"`, and `"%{IncludeDir.DirectXHeaders}"`. Keep `"%{IncludeDir.nvrhi}"` — the public `Device.hpp` includes `<nvrhi/nvrhi.h>`.

- [ ] **Step 11: Generate, build, run — `[gpu]` proves real hardware work**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```

Expected: all tests pass, including `[gpu][d3d12]` with the byte-exact readback. The two removed vendor smoke cases are gone from the count. With validation on (Debug default), any NVRHI/D3D12 misuse appears as `[nvrhi]` log lines — a clean run logs only the "D3D12 device created on '...'" info line.

- [ ] **Step 12: Build Release + Dist to prove the premake rewire holds everywhere**

```bat
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
bin\Release-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Expected: builds clean; Release tests pass (validation off — `enableValidation` defaults false outside Debug).

- [ ] **Step 13: Commit**

```bash
git add -A Arcane/ && git commit -m "feat(arcane): Render module - RenderDevice API + D3D12 device, GPU readback test

Headless D3D12 device creation through nvrhi::d3d12::createDevice with the
debug+validation layers in Debug. The Vulkan-Hpp dispatcher TU moves into
the DLL's Render module (M0 contract). ArcaneTests drops its direct
nvrhi/SDL3/d3d12 links and the superseded link-smoke tests - real device
and window creation through Arcane.dll replace those arrival gates."
```

---

### Task 4: Vulkan device (headless)

Vulkan instance → physical device → device + graphics queue, wrapped by `nvrhi::vulkan::createDevice`. Key facts already verified against the vendored sources: NVRHI's Vulkan queue uses **timeline semaphores** (`vk::TimelineSemaphoreSubmitInfo` in `vulkan-queue.cpp`), so `VkPhysicalDeviceVulkan12Features::timelineSemaphore` MUST be enabled; the dynamic loader class is `vk::detail::DynamicLoader` (Vulkan-Headers VK_HEADER_VERSION 353); NVRHI receives the enabled extension lists through `DeviceDesc::instanceExtensions/deviceExtensions`.

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Render/DeviceVulkan.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/DeviceFactories.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/Device.cpp`
- Modify: `Arcane/Tests/src/DeviceTest.cpp`

- [ ] **Step 1: Add the failing test to `Arcane/Tests/src/DeviceTest.cpp`**

```cpp
TEST_CASE("vulkan: headless device creates and clears an offscreen target", "[gpu][vulkan]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::Vulkan;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    REQUIRE(device->Nvrhi() != nullptr);
    REQUIRE_FALSE(device->AdapterName().empty());
    CHECK(std::string(Arcane::ToString(device->Backend())) == "Vulkan");

    CheckOffscreenClear(*device);
}
```

- [ ] **Step 2: Run it to verify it fails**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[vulkan]"
```
Expected: FAIL — `RenderDevice::Create` returns null ("backend Vulkan not available" in the log).

- [ ] **Step 3: Add the Vulkan factory declaration to `DeviceFactories.hpp`**

```cpp
    std::unique_ptr<RenderDevice> CreateDeviceVulkan(const RenderDeviceDesc& desc);
```

(directly under the `CreateDeviceD3D12` declaration.)

- [ ] **Step 4: Replace the `default:` arm in `Device.cpp`'s `RenderDevice::Create`**

```cpp
        switch (desc.backend)
        {
        case GraphicsBackend::D3D12:
            return CreateDeviceD3D12(desc);
        case GraphicsBackend::Vulkan:
            return CreateDeviceVulkan(desc);
        }
        ARC_ERROR("RenderDevice::Create: unknown backend");
        return nullptr;
```

- [ ] **Step 5: Write `Arcane/Arcane/src/Arcane/Render/DeviceVulkan.cpp`**

```cpp
// Vulkan backend: instance + physical device + device + graphics queue,
// wrapped by nvrhi::vulkan::createDevice. Uses the Vulkan-Hpp default
// dynamic dispatcher (storage TU: VulkanDispatchStorage.cpp). Headless
// here; the swapchain half of this TU arrives with the windowed task.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>

#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>

#include <vulkan/vulkan.hpp>

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    namespace
    {
        // Surface extensions are requested even for headless devices: they
        // cost nothing without a surface, and keep one code path for both
        // headless tests and windowed swapchains.
        const char* kInstanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        const char* kDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

        class DeviceVulkan final : public RenderDevice
        {
        public:
            ~DeviceVulkan() override;
            bool Init(const RenderDeviceDesc& desc);

            GraphicsBackend Backend() const override { return GraphicsBackend::Vulkan; }
            nvrhi::IDevice* Nvrhi() const override { return m_nvrhi.Get(); }
            std::string AdapterName() const override { return m_adapterName; }

            vk::Instance Instance() const { return m_instance; }
            vk::PhysicalDevice PhysicalDevice() const { return m_physicalDevice; }
            vk::Device Device() const { return m_device; }
            vk::Queue GraphicsQueue() const { return m_graphicsQueue; }
            uint32_t GraphicsQueueFamily() const { return (uint32_t)m_graphicsQueueFamily; }

            // The unwrapped backend device, for native queue-semaphore calls.
            // (The validation layer wraps Nvrhi(); the swapchain needs the
            // nvrhi::vulkan::IDevice interface underneath.)
            nvrhi::vulkan::IDevice* VulkanNvrhi() const
            {
                return static_cast<nvrhi::vulkan::IDevice*>(m_nvrhiBackend.Get());
            }

        private:
            vk::detail::DynamicLoader m_loader;  // must outlive everything below
            vk::Instance       m_instance;
            vk::PhysicalDevice m_physicalDevice;
            vk::Device         m_device;
            vk::Queue          m_graphicsQueue;
            int                m_graphicsQueueFamily = -1;
            nvrhi::DeviceHandle m_nvrhiBackend;  // the real vulkan device
            nvrhi::DeviceHandle m_nvrhi;         // possibly validation-wrapped
            std::string         m_adapterName;
        };

        bool DeviceVulkan::Init(const RenderDeviceDesc& desc)
        {
            auto vkGetInstanceProcAddr =
                m_loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
            if (!vkGetInstanceProcAddr)
            {
                ARC_ERROR("Vulkan loader not available (vulkan-1.dll missing?)");
                return false;
            }
            VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

            std::vector<const char*> layers;
            if (desc.enableValidation)
            {
                for (const auto& layer : vk::enumerateInstanceLayerProperties())
                {
                    if (std::string_view(layer.layerName) == kValidationLayer)
                    {
                        layers.push_back(kValidationLayer);
                        break;
                    }
                }
                if (layers.empty())
                    ARC_WARN("{} not installed; continuing without it", kValidationLayer);
            }

            auto appInfo = vk::ApplicationInfo()
                .setPApplicationName("Arcane")
                .setApiVersion(VK_API_VERSION_1_3);

            auto instanceInfo = vk::InstanceCreateInfo()
                .setPApplicationInfo(&appInfo)
                .setEnabledLayerCount((uint32_t)layers.size())
                .setPpEnabledLayerNames(layers.data())
                .setEnabledExtensionCount((uint32_t)std::size(kInstanceExtensions))
                .setPpEnabledExtensionNames(kInstanceExtensions);

            m_instance = vk::createInstance(instanceInfo);
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);

            auto physicalDevices = m_instance.enumeratePhysicalDevices();
            if (physicalDevices.empty())
            {
                ARC_ERROR("No Vulkan physical devices");
                return false;
            }
            m_physicalDevice = physicalDevices[0];
            for (auto& candidate : physicalDevices)
            {
                if (candidate.getProperties().deviceType ==
                    vk::PhysicalDeviceType::eDiscreteGpu)
                {
                    m_physicalDevice = candidate;
                    break;
                }
            }
            m_adapterName = std::string(
                m_physicalDevice.getProperties().deviceName.data());

            auto families = m_physicalDevice.getQueueFamilyProperties();
            for (uint32_t i = 0; i < (uint32_t)families.size(); ++i)
            {
                if (families[i].queueFlags & vk::QueueFlagBits::eGraphics)
                {
                    m_graphicsQueueFamily = (int)i;
                    break;
                }
            }
            if (m_graphicsQueueFamily < 0)
            {
                ARC_ERROR("No graphics queue family");
                return false;
            }

            const float priority = 1.0f;
            auto queueInfo = vk::DeviceQueueCreateInfo()
                .setQueueFamilyIndex((uint32_t)m_graphicsQueueFamily)
                .setQueueCount(1)
                .setPQueuePriorities(&priority);

            // NVRHI's Vulkan queue submits with vk::TimelineSemaphoreSubmitInfo
            // (vulkan-queue.cpp) -- timelineSemaphore is a hard requirement.
            auto vulkan12Features = vk::PhysicalDeviceVulkan12Features()
                .setTimelineSemaphore(true);

            auto deviceInfo = vk::DeviceCreateInfo()
                .setQueueCreateInfoCount(1)
                .setPQueueCreateInfos(&queueInfo)
                .setEnabledExtensionCount((uint32_t)std::size(kDeviceExtensions))
                .setPpEnabledExtensionNames(kDeviceExtensions)
                .setPNext(&vulkan12Features);

            m_device = m_physicalDevice.createDevice(deviceInfo);
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device);
            m_graphicsQueue = m_device.getQueue((uint32_t)m_graphicsQueueFamily, 0);

            nvrhi::vulkan::DeviceDesc nvrhiDesc;
            nvrhiDesc.errorCB = &NvrhiMessageCallback::Instance();
            nvrhiDesc.instance = m_instance;
            nvrhiDesc.physicalDevice = m_physicalDevice;
            nvrhiDesc.device = m_device;
            nvrhiDesc.graphicsQueue = m_graphicsQueue;
            nvrhiDesc.graphicsQueueIndex = m_graphicsQueueFamily;
            nvrhiDesc.instanceExtensions = kInstanceExtensions;
            nvrhiDesc.numInstanceExtensions = std::size(kInstanceExtensions);
            nvrhiDesc.deviceExtensions = kDeviceExtensions;
            nvrhiDesc.numDeviceExtensions = std::size(kDeviceExtensions);

            m_nvrhiBackend = nvrhi::vulkan::createDevice(nvrhiDesc);
            if (!m_nvrhiBackend)
            {
                ARC_ERROR("nvrhi::vulkan::createDevice failed");
                return false;
            }

            m_nvrhi = m_nvrhiBackend;
            if (desc.enableValidation)
                m_nvrhi = nvrhi::validation::createValidationLayer(m_nvrhi);

            ARC_INFO("Vulkan device created on '{}'", m_adapterName);
            return true;
        }

        DeviceVulkan::~DeviceVulkan()
        {
            if (m_nvrhi)
            {
                m_nvrhi->waitForIdle();
                m_nvrhi->runGarbageCollection();
            }
            m_nvrhi = nullptr;
            m_nvrhiBackend = nullptr;
            if (m_device)
                m_device.destroy();
            if (m_instance)
                m_instance.destroy();
        }
    }

    std::unique_ptr<RenderDevice> CreateDeviceVulkan(const RenderDeviceDesc& desc)
    {
        auto device = std::make_unique<DeviceVulkan>();
        if (!device->Init(desc))
            return nullptr;
        return device;
    }
}
```

- [ ] **Step 6: Build and run both backend tests**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[gpu]"
```
Expected: both `[gpu][d3d12]` and `[gpu][vulkan]` pass with byte-exact readbacks. If `createInstance` throws `vk::LayerNotPresentError` the validation-layer probe has a bug; if device creation fails with a feature error, the `timelineSemaphore` pNext chain didn't reach `createDevice`.

- [ ] **Step 7: Run the full suite + Release config**

```bat
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
msbuild Arcane.slnx /p:Configuration=Release /m
bin\Release-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Expected: all pass in both configs.

- [ ] **Step 8: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): Vulkan device - instance/device/queue via vulkan.hpp dynamic dispatch, nvrhi wrap"
```

---

### Task 5: Swapchain interface + D3D12 swapchain — first pixels presented

The `Swapchain` interface, `RenderDevice::CreateSwapchain`, and the DXGI implementation. Verified by a hidden-window test that renders real frames and exercises resize.

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Render/Swapchain.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/Device.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/DeviceD3D12.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/DeviceVulkan.cpp` (temporary stub override, replaced in Task 6)
- Create: `Arcane/Tests/src/SwapchainTest.cpp`

- [ ] **Step 1: Write the failing test `Arcane/Tests/src/SwapchainTest.cpp`**

The helper is written for both backends from the start; Task 5 registers only the D3D12 case, Task 6 adds Vulkan.

```cpp
// Windowed GPU path: hidden window + swapchain, several cleared and
// presented frames, then a resize and more frames. Hidden windows present
// fine on both DXGI and Vulkan; vsync off so the test never waits on the
// display.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Swapchain.hpp>

namespace
{
    void RunWindowedClearFrames(Arcane::GraphicsBackend backend)
    {
        Arcane::Window window;
        Arcane::WindowDesc windowDesc;
        windowDesc.title  = std::string("SwapchainTest ") + Arcane::ToString(backend);
        windowDesc.width  = 640;
        windowDesc.height = 360;
        windowDesc.hidden = true;
        windowDesc.vulkan = (backend == Arcane::GraphicsBackend::Vulkan);
        REQUIRE(window.Create(windowDesc));

        Arcane::RenderDeviceDesc deviceDesc;
        deviceDesc.backend = backend;
        auto device = Arcane::RenderDevice::Create(deviceDesc);
        REQUIRE(device != nullptr);

        auto swapchain = device->CreateSwapchain(window, /*vsync=*/false);
        REQUIRE(swapchain != nullptr);
        REQUIRE(swapchain->Width() == 640);
        REQUIRE(swapchain->Height() == 360);

        nvrhi::CommandListHandle commandList = device->Nvrhi()->createCommandList();

        auto renderFrame = [&](float red) {
            nvrhi::ITexture* backbuffer = swapchain->BeginFrame();
            REQUIRE(backbuffer != nullptr);
            commandList->open();
            commandList->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                           nvrhi::Color(red, 0.2f, 0.4f, 1.0f));
            commandList->close();
            device->Nvrhi()->executeCommandList(commandList);
            swapchain->Present();
        };

        for (int i = 0; i < 5; ++i)
            renderFrame(0.1f * (float)i);

        swapchain->Resize(800, 450);
        REQUIRE(swapchain->Width() == 800);
        REQUIRE(swapchain->Height() == 450);

        for (int i = 0; i < 2; ++i)
            renderFrame(0.8f);
    }
}

TEST_CASE("d3d12: windowed swapchain clears, presents, resizes", "[gpu][d3d12]")
{
    RunWindowedClearFrames(Arcane::GraphicsBackend::D3D12);
}
```

- [ ] **Step 2: Run it to verify it fails**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `Swapchain.hpp` and `CreateSwapchain` don't exist.

- [ ] **Step 3: Write `Arcane/Arcane/src/Arcane/Render/Swapchain.hpp`**

```cpp
#pragma once

// Render module: backbuffer presentation against a Window. M1 pacing rule:
// Present() waits for GPU idle (one frame in flight) -- correct-first;
// real frame pacing arrives with the M2 renderer work.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Arcane
{
    class ARCANE_API Swapchain
    {
    public:
        virtual ~Swapchain() = default;

        // Acquires the current backbuffer. Returns null when the frame must
        // be skipped (zero-sized window mid-resize, surface out of date);
        // callers skip rendering for that frame.
        virtual nvrhi::ITexture* BeginFrame() = 0;

        virtual void Present() = 0;

        virtual void Resize(uint32_t width, uint32_t height) = 0;

        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
        virtual nvrhi::Format Format() const = 0;
    };
}
```

- [ ] **Step 4: Add `CreateSwapchain` to `RenderDevice` in `Device.hpp`**

Add a forward declaration above the class and the pure virtual below `AdapterName()`:

```cpp
    class Window;
    class Swapchain;
```

```cpp
        // The window must outlive the swapchain. For Vulkan, the window
        // must have been created with WindowDesc::vulkan = true.
        virtual std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                           bool vsync) = 0;
```

- [ ] **Step 5: Implement the D3D12 swapchain in `DeviceD3D12.cpp`**

Add to the includes:

```cpp
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <vector>
```

Inside the anonymous namespace, above `class DeviceD3D12`, add the constants; inside `DeviceD3D12`, declare the override (defined after the swapchain class):

```cpp
        constexpr uint32_t kBackbufferCount = 3;
        constexpr DXGI_FORMAT kSwapchainFormatDxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
        constexpr nvrhi::Format kSwapchainFormat = nvrhi::Format::BGRA8_UNORM;
```

```cpp
            std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                       bool vsync) override;
```

After the `DeviceD3D12` class definition (still in the anonymous namespace):

```cpp
        class SwapchainD3D12 final : public Swapchain
        {
        public:
            ~SwapchainD3D12() override;
            bool Init(DeviceD3D12& device, Window& window, bool vsync);

            nvrhi::ITexture* BeginFrame() override;
            void Present() override;
            void Resize(uint32_t width, uint32_t height) override;
            uint32_t Width() const override { return m_width; }
            uint32_t Height() const override { return m_height; }
            nvrhi::Format Format() const override { return kSwapchainFormat; }

        private:
            bool CreateBackbufferHandles();
            void ReleaseBackbufferHandles();

            DeviceD3D12* m_device = nullptr;
            ComPtr<IDXGISwapChain3> m_swapchain;
            std::vector<nvrhi::TextureHandle> m_backbuffers;
            uint32_t m_width = 0;
            uint32_t m_height = 0;
            bool m_vsync = true;
        };

        bool SwapchainD3D12::Init(DeviceD3D12& device, Window& window, bool vsync)
        {
            m_device = &device;
            m_vsync = vsync;
            window.GetPixelSize(m_width, m_height);

            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            scDesc.Width = m_width;
            scDesc.Height = m_height;
            scDesc.Format = kSwapchainFormatDxgi;
            scDesc.SampleDesc = { 1, 0 };
            scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scDesc.BufferCount = kBackbufferCount;
            scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            HWND hwnd = static_cast<HWND>(window.NativeHandle());
            ComPtr<IDXGISwapChain1> swapchain1;
            if (FAILED(device.Factory()->CreateSwapChainForHwnd(
                    device.GraphicsQueue(), hwnd, &scDesc, nullptr, nullptr,
                    &swapchain1)))
            {
                ARC_ERROR("CreateSwapChainForHwnd failed");
                return false;
            }
            if (FAILED(swapchain1.As(&m_swapchain)))
            {
                ARC_ERROR("IDXGISwapChain3 not available");
                return false;
            }
            return CreateBackbufferHandles();
        }

        bool SwapchainD3D12::CreateBackbufferHandles()
        {
            m_backbuffers.resize(kBackbufferCount);
            for (uint32_t i = 0; i < kBackbufferCount; ++i)
            {
                ComPtr<ID3D12Resource> buffer;
                if (FAILED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer))))
                {
                    ARC_ERROR("Swapchain GetBuffer({}) failed", i);
                    return false;
                }

                auto texDesc = nvrhi::TextureDesc()
                    .setWidth(m_width)
                    .setHeight(m_height)
                    .setFormat(kSwapchainFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::Present)
                    .setKeepInitialState(true)
                    .setDebugName("SwapchainBuffer");
                m_backbuffers[i] = m_device->Nvrhi()->createHandleForNativeTexture(
                    nvrhi::ObjectTypes::D3D12_Resource,
                    nvrhi::Object(buffer.Get()), texDesc);
                if (!m_backbuffers[i])
                {
                    ARC_ERROR("createHandleForNativeTexture failed for buffer {}", i);
                    return false;
                }
            }
            return true;
        }

        void SwapchainD3D12::ReleaseBackbufferHandles()
        {
            m_device->Nvrhi()->waitForIdle();
            m_backbuffers.clear();
            m_device->Nvrhi()->runGarbageCollection();
        }

        nvrhi::ITexture* SwapchainD3D12::BeginFrame()
        {
            if (m_width == 0 || m_height == 0 || m_backbuffers.empty())
                return nullptr;
            return m_backbuffers[m_swapchain->GetCurrentBackBufferIndex()];
        }

        void SwapchainD3D12::Present()
        {
            m_swapchain->Present(m_vsync ? 1 : 0, 0);
            // M1 pacing: one frame in flight, idle after present.
            m_device->Nvrhi()->waitForIdle();
            m_device->Nvrhi()->runGarbageCollection();
        }

        void SwapchainD3D12::Resize(uint32_t width, uint32_t height)
        {
            if (width == m_width && height == m_height)
                return;
            m_width = width;
            m_height = height;
            ReleaseBackbufferHandles();
            if (width == 0 || height == 0)
                return;  // minimized; BeginFrame returns null until restored
            if (FAILED(m_swapchain->ResizeBuffers(kBackbufferCount, width, height,
                                                  kSwapchainFormatDxgi, 0)))
            {
                ARC_ERROR("ResizeBuffers({}x{}) failed", width, height);
                return;
            }
            CreateBackbufferHandles();
        }

        SwapchainD3D12::~SwapchainD3D12()
        {
            if (m_device)
                ReleaseBackbufferHandles();
        }

        std::unique_ptr<Swapchain> DeviceD3D12::CreateSwapchain(Window& window,
                                                                bool vsync)
        {
            auto swapchain = std::make_unique<SwapchainD3D12>();
            if (!swapchain->Init(*this, window, vsync))
                return nullptr;
            return swapchain;
        }
```

- [ ] **Step 6: Give `DeviceVulkan` a temporary failing override (replaced wholesale by Task 6)**

Without it the now-abstract `CreateSwapchain` breaks the Vulkan TU's compile. Add to the `DeviceVulkan` class:

```cpp
            std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                       bool vsync) override
            {
                (void)window; (void)vsync;
                ARC_ERROR("Vulkan swapchain not implemented yet (next task)");
                return nullptr;
            }
```

and add `#include <Arcane/Render/Swapchain.hpp>` plus `#include <Arcane/Platform/Window.hpp>` to `DeviceVulkan.cpp`'s includes.

- [ ] **Step 7: Build and run**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[gpu]"
```
Expected: all `[gpu]` cases pass, including the new windowed D3D12 case. Validation (Debug) must stay silent through resize — barrier or lifetime mistakes show up as `[nvrhi]` error lines.

- [ ] **Step 8: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): Swapchain interface + DXGI flip-discard swapchain - first presented pixels"
```

---

### Task 6: Vulkan swapchain

SDL-created `VkSurfaceKHR`, `VkSwapchainKHR` with wrapped images, binary acquire/present semaphores bridged into NVRHI via `queueWaitForSemaphore`/`queueSignalSemaphore` (the donut-proven pattern: signal, flush with an empty `executeCommandLists`, then `vkQueuePresentKHR`). The M1 idle-after-present rule makes the single semaphore pair safe to reuse.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Render/DeviceVulkan.cpp`
- Modify: `Arcane/Tests/src/SwapchainTest.cpp`

- [ ] **Step 1: Add the failing test case to `Arcane/Tests/src/SwapchainTest.cpp`**

```cpp
TEST_CASE("vulkan: windowed swapchain clears, presents, resizes", "[gpu][vulkan]")
{
    RunWindowedClearFrames(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 2: Run it to verify it fails**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[vulkan]"
```
Expected: the new case FAILS — `CreateSwapchain` returns null (the Task 5 stub) and logs "Vulkan swapchain not implemented yet".

- [ ] **Step 3: Implement `SwapchainVulkan` in `DeviceVulkan.cpp`**

Add to the includes:

```cpp
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
```

(`<Arcane/Platform/Window.hpp>` and `<Arcane/Render/Swapchain.hpp>` arrived with Task 5's stub.) Add next to the other constants:

```cpp
        constexpr nvrhi::Format kSwapchainFormat = nvrhi::Format::BGRA8_UNORM;
        constexpr vk::Format kSwapchainFormatVk = vk::Format::eB8G8R8A8Unorm;
```

**Delete the Task 5 stub override** from the `DeviceVulkan` class and replace it with a declaration (defined after the swapchain class):

```cpp
            std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                       bool vsync) override;
```

After the `DeviceVulkan` class definition (still inside the anonymous namespace), add:

```cpp
        class SwapchainVulkan final : public Swapchain
        {
        public:
            ~SwapchainVulkan() override;
            bool Init(DeviceVulkan& device, Window& window, bool vsync);

            nvrhi::ITexture* BeginFrame() override;
            void Present() override;
            void Resize(uint32_t width, uint32_t height) override;
            uint32_t Width() const override { return m_width; }
            uint32_t Height() const override { return m_height; }
            nvrhi::Format Format() const override { return kSwapchainFormat; }

        private:
            bool CreateSwapchainObjects();
            void ReleaseBackbufferHandles();

            DeviceVulkan* m_device = nullptr;
            vk::SurfaceKHR m_surface;
            vk::SwapchainKHR m_swapchain;
            std::vector<nvrhi::TextureHandle> m_backbuffers;
            vk::Semaphore m_acquireSemaphore;
            vk::Semaphore m_presentSemaphore;
            uint32_t m_currentImage = 0;
            uint32_t m_width = 0;
            uint32_t m_height = 0;
            bool m_vsync = true;
            bool m_acquired = false;
        };

        bool SwapchainVulkan::Init(DeviceVulkan& device, Window& window, bool vsync)
        {
            m_device = &device;
            m_vsync = vsync;
            window.GetPixelSize(m_width, m_height);

            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(window.SdlWindow(), device.Instance(),
                                          nullptr, &surface))
            {
                ARC_ERROR("SDL_Vulkan_CreateSurface failed: {} "
                          "(was the window created with WindowDesc::vulkan?)",
                          SDL_GetError());
                return false;
            }
            m_surface = surface;

            if (!device.PhysicalDevice().getSurfaceSupportKHR(
                    device.GraphicsQueueFamily(), m_surface))
            {
                ARC_ERROR("Graphics queue family cannot present to this surface");
                return false;
            }

            m_acquireSemaphore = device.Device().createSemaphore({});
            m_presentSemaphore = device.Device().createSemaphore({});

            return CreateSwapchainObjects();
        }

        bool SwapchainVulkan::CreateSwapchainObjects()
        {
            vk::PhysicalDevice physical = m_device->PhysicalDevice();
            auto caps = physical.getSurfaceCapabilitiesKHR(m_surface);

            vk::Extent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX)  // surface lets us choose
            {
                extent.width = std::clamp(m_width, caps.minImageExtent.width,
                                          caps.maxImageExtent.width);
                extent.height = std::clamp(m_height, caps.minImageExtent.height,
                                           caps.maxImageExtent.height);
            }
            m_width = extent.width;
            m_height = extent.height;
            if (m_width == 0 || m_height == 0)
                return true;  // minimized; BeginFrame skips until restored

            vk::ColorSpaceKHR colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
            bool formatFound = false;
            for (const auto& format : physical.getSurfaceFormatsKHR(m_surface))
            {
                if (format.format == kSwapchainFormatVk)
                {
                    colorSpace = format.colorSpace;
                    formatFound = true;
                    break;
                }
            }
            if (!formatFound)
            {
                ARC_ERROR("Surface does not support B8G8R8A8_UNORM");
                return false;
            }

            vk::PresentModeKHR presentMode = vk::PresentModeKHR::eFifo;  // vsync
            if (!m_vsync)
            {
                presentMode = vk::PresentModeKHR::eImmediate;
                for (auto mode : physical.getSurfacePresentModesKHR(m_surface))
                {
                    if (mode == vk::PresentModeKHR::eMailbox)
                    {
                        presentMode = vk::PresentModeKHR::eMailbox;
                        break;
                    }
                }
            }

            uint32_t imageCount = std::max(3u, caps.minImageCount);
            if (caps.maxImageCount != 0)
                imageCount = std::min(imageCount, caps.maxImageCount);

            auto swapchainInfo = vk::SwapchainCreateInfoKHR()
                .setSurface(m_surface)
                .setMinImageCount(imageCount)
                .setImageFormat(kSwapchainFormatVk)
                .setImageColorSpace(colorSpace)
                .setImageExtent(extent)
                .setImageArrayLayers(1)
                .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment |
                               vk::ImageUsageFlagBits::eTransferDst)
                .setImageSharingMode(vk::SharingMode::eExclusive)
                .setPreTransform(caps.currentTransform)
                .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
                .setPresentMode(presentMode)
                .setClipped(true)
                .setOldSwapchain(m_swapchain);

            vk::SwapchainKHR newSwapchain =
                m_device->Device().createSwapchainKHR(swapchainInfo);
            if (m_swapchain)
                m_device->Device().destroySwapchainKHR(m_swapchain);
            m_swapchain = newSwapchain;

            auto images = m_device->Device().getSwapchainImagesKHR(m_swapchain);
            m_backbuffers.clear();
            m_backbuffers.reserve(images.size());
            for (size_t i = 0; i < images.size(); ++i)
            {
                auto texDesc = nvrhi::TextureDesc()
                    .setWidth(m_width)
                    .setHeight(m_height)
                    .setFormat(kSwapchainFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::Present)
                    .setKeepInitialState(true)
                    .setDebugName("SwapchainImage");
                nvrhi::TextureHandle handle =
                    m_device->Nvrhi()->createHandleForNativeTexture(
                        nvrhi::ObjectTypes::VK_Image,
                        nvrhi::Object((VkImage)images[i]), texDesc);
                if (!handle)
                {
                    ARC_ERROR("createHandleForNativeTexture failed for image {}", i);
                    return false;
                }
                m_backbuffers.push_back(handle);
            }
            return true;
        }

        void SwapchainVulkan::ReleaseBackbufferHandles()
        {
            m_device->Nvrhi()->waitForIdle();
            m_backbuffers.clear();
            m_device->Nvrhi()->runGarbageCollection();
        }

        nvrhi::ITexture* SwapchainVulkan::BeginFrame()
        {
            if (m_width == 0 || m_height == 0 || m_backbuffers.empty())
                return nullptr;

            try
            {
                auto acquired = m_device->Device().acquireNextImageKHR(
                    m_swapchain, UINT64_MAX, m_acquireSemaphore, nullptr);
                if (acquired.result != vk::Result::eSuccess &&
                    acquired.result != vk::Result::eSuboptimalKHR)
                    return nullptr;
                m_currentImage = acquired.value;
            }
            catch (const vk::OutOfDateKHRError&)
            {
                // Surface changed under us: rebuild at the current size and
                // skip this frame.
                ReleaseBackbufferHandles();
                CreateSwapchainObjects();
                return nullptr;
            }

            m_device->VulkanNvrhi()->queueWaitForSemaphore(
                nvrhi::CommandQueue::Graphics, m_acquireSemaphore, 0);
            m_acquired = true;
            return m_backbuffers[m_currentImage];
        }

        void SwapchainVulkan::Present()
        {
            if (!m_acquired)
                return;
            m_acquired = false;

            m_device->VulkanNvrhi()->queueSignalSemaphore(
                nvrhi::CommandQueue::Graphics, m_presentSemaphore, 0);
            // Empty submit flushes the queued semaphore signal (donut pattern).
            m_device->Nvrhi()->executeCommandLists(nullptr, 0);

            auto presentInfo = vk::PresentInfoKHR()
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&m_presentSemaphore)
                .setSwapchainCount(1)
                .setPSwapchains(&m_swapchain)
                .setPImageIndices(&m_currentImage);
            try
            {
                (void)m_device->GraphicsQueue().presentKHR(presentInfo);
            }
            catch (const vk::OutOfDateKHRError&)
            {
                // Rebuilt on the next BeginFrame/Resize.
            }

            // M1 pacing: one frame in flight, idle after present -- also what
            // makes the single acquire/present semaphore pair safe to reuse.
            m_device->Nvrhi()->waitForIdle();
            m_device->Nvrhi()->runGarbageCollection();
        }

        void SwapchainVulkan::Resize(uint32_t width, uint32_t height)
        {
            if (width == m_width && height == m_height)
                return;
            m_width = width;
            m_height = height;
            ReleaseBackbufferHandles();
            CreateSwapchainObjects();
        }

        SwapchainVulkan::~SwapchainVulkan()
        {
            if (!m_device)
                return;
            ReleaseBackbufferHandles();
            if (m_swapchain)
                m_device->Device().destroySwapchainKHR(m_swapchain);
            if (m_acquireSemaphore)
                m_device->Device().destroySemaphore(m_acquireSemaphore);
            if (m_presentSemaphore)
                m_device->Device().destroySemaphore(m_presentSemaphore);
            if (m_surface)
                m_device->Instance().destroySurfaceKHR(m_surface);
        }

        std::unique_ptr<Swapchain> DeviceVulkan::CreateSwapchain(Window& window,
                                                                 bool vsync)
        {
            auto swapchain = std::make_unique<SwapchainVulkan>();
            if (!swapchain->Init(*this, window, vsync))
                return nullptr;
            return swapchain;
        }
```

- [ ] **Step 4: Build and run the full GPU suite**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[gpu]"
```
Expected: all four `[gpu]` cases pass (headless + windowed, both backends). Likely failure modes: a hang in `acquireNextImageKHR` means the empty-submit flush in `Present` didn't reach the queue; a validation error about semaphore reuse means an idle-wait was skipped; `eErrorOutOfDateKHR` loops mean `CreateSwapchainObjects` isn't honoring `caps.currentExtent`.

- [ ] **Step 5: Run the whole suite in Debug + Release**

```bat
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
msbuild Arcane.slnx /p:Configuration=Release /m
bin\Release-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Expected: all pass, both configs.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): Vulkan swapchain - SDL surface, semaphore bridge into nvrhi, resize/out-of-date handling"
```

---

### Task 7: Playground.exe — the M1 gate

A windowed demo that animates the clear color so present is visibly working: `--backend dx12|vulkan` picks the backend, `--frames N` exits cleanly after N frames (scripted verification), title bar shows backend + adapter + frame time.

**Files:**
- Create: `Arcane/Playground/src/main.cpp`
- Modify: `Arcane/premake5.lua`

- [ ] **Step 1: Add the `Playground` project to `Arcane/premake5.lua`**

Insert after the `Arcane` project, before `ArcaneTests`:

```lua
-- ============================================================================
-- Playground: standalone exe, the living integration test (stack spec).
-- M1 scope: window + device + clear + present on both backends. Becomes
-- PlaygroundGame.dll under Loom in M4.
-- ============================================================================
project "Playground"
    location "Playground"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
    }

    includedirs {
        "%{wks.location}/Arcane/src",
        "%{IncludeDir.Core}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.nvrhi}",
    }

    links { "Arcane" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"
```

- [ ] **Step 2: Write `Arcane/Playground/src/main.cpp`**

```cpp
// Arcane Playground -- M1: window + NVRHI device + clear + present.
// The clear color animates so a working present loop is visually obvious.
// Scripted verification: --frames N renders N frames and exits 0.

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <nvrhi/nvrhi.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
            maxFrames = std::strtoull(argv[++i], nullptr, 10);
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
            continue;

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
        ++framesSinceTitle;
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
```

(Destruction order matters and falls out of declaration order: `swapchain` destructs before `device`, `device` before `window`.)

- [ ] **Step 3: Generate, build, scripted runs on both backends**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\Playground\Playground.exe --backend dx12 --frames 180 --no-vsync
echo %ERRORLEVEL%
bin\Debug-windows-x86_64-md\Playground\Playground.exe --backend vulkan --frames 180 --no-vsync
echo %ERRORLEVEL%
```
Expected: each run opens a window, the clear color visibly animates, the process exits with `0` after 180 frames, and the log shows the right "device created on '...'" line per backend. No `[nvrhi]` validation errors.

- [ ] **Step 4: Manual visual check (the spec's M1 gate)**

Run `Playground.exe` with no args (D3D12, vsync) and again with `--backend vulkan`:
- title bar shows backend + adapter + a plausible frame time (~16.7 ms with vsync);
- resizing the window by dragging works without crashes, flicker, or validation errors;
- minimize + restore works;
- ESC and the close button both exit cleanly.

- [ ] **Step 5: Release + Dist builds and a Release scripted run**

```bat
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
bin\Release-windows-x86_64-md\Playground\Playground.exe --backend vulkan --frames 60
echo %ERRORLEVEL%
```
Expected: builds clean, run exits 0.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ && git commit -m "feat(arcane): Playground.exe - animated clear+present on D3D12 and Vulkan (M1 gate)"
```

---

### Task 8: Docs + cross-workspace regression sweep

CLAUDE.md's Arcane section still describes the M0 world (Core+Tests only, dispatcher TU in Tests). No Jenkinsfile change is needed — CI already builds `Arcane.slnx` across all three configs (new projects ride along) and runs ArcaneTests Debug+Release on `windows-1`, which has the `gpu` label and an interactive session, so the new `[gpu]` and `[platform]` tests run as-is.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update the Arcane section of `CLAUDE.md`**

Three edits in the "Build System (Arcane engine)" section:

**(a)** The intro line — replace:

> M0 contains `Core` (static lib) + `ArcaneTests`; Arcane.dll/Loom/Grimoire/Playground arrive in later milestones.

with:

> M1 state: `Core` (static lib), `Arcane` (the engine DLL — Base/Platform/Render modules: SDL3 window, NVRHI device on D3D12 + Vulkan, swapchain/clear/present), `Playground` (demo exe: `--backend dx12|vulkan`, `--frames N`, `--no-vsync`), and `ArcaneTests`. Loom/Grimoire/Game arrive in later milestones.

**(b)** The build snippet — add the Playground run line after the ArcaneTests line:

```bat
bin\Debug-windows-x86_64-md\Playground\Playground.exe --backend vulkan --frames 180
```

**(c)** The Vulkan dispatcher bullet — replace:

> One consumer TU owns the Vulkan-Hpp dynamic dispatcher storage (`VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE`) — currently `Tests/src/VulkanDispatchStorage.cpp`; it moves into Arcane.dll's Render module in M1.

with:

> One TU per module owns the Vulkan-Hpp dynamic dispatcher storage (`VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE`) — `Arcane/Arcane/src/Arcane/Render/VulkanDispatchStorage.cpp` (moved out of Tests in M1; Arcane.dll is the one-engine-instance-per-process owner). GPU-touching tests are tagged `[gpu]` (exclude with `ArcaneTests.exe ~[gpu]` on machines without a capable GPU; CI's windows-1 runs them).

- [ ] **Step 2: Cross-workspace regression — Server still builds and passes**

The only file both workspaces compile that this plan touches is `Core/src/Arcane/Version.hpp` (string change), but prove it:

```bat
cd Server
GenerateProjects.bat
msbuild Aphelyon.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64\CommonTests\CommonTests.exe
```
Expected: builds clean, CommonTests pass. (Skip AccountTests — ~17 min and nothing DB-adjacent changed; CI runs it on main anyway.)

- [ ] **Step 3: Full Arcane suite one last time (CI rehearsal)**

```bat
cd Arcane
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe -r junit::out=%TEMP%\arcane-m1.xml
type %TEMP%\arcane-m1.xml | findstr /C:"failures=\"0\""
```
Expected: JUnit report writes (the exact CI invocation shape) with zero failures.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md && git commit -m "docs: CLAUDE.md Arcane section - M1 state (Arcane.dll, Playground, dispatcher TU move)"
```

---

## M1 exit criteria (mirror of the spec's bring-up bullet)

- [ ] `Arcane.dll` builds in Debug/Release/Dist with Base/Platform/Render modules; consumers link only the import lib.
- [ ] SDL3 window opens/pumps/resizes through `Arcane::Window`.
- [ ] NVRHI device creates on **both** D3D12 and Vulkan, headless and windowed, with validation clean in Debug.
- [ ] Clear + present loop runs on both backends (Playground), survives resize/minimize, exits 0 under `--frames N`.
- [ ] Offscreen clear readback tests pin byte-exact GPU output per backend in ArcaneTests (CI-run).
- [ ] Vulkan dispatcher TU lives in the DLL's Render module (M0 contract closed).

Out of scope, per the bring-up order: 2D batcher/canvas/shaders/text/ImGui (M2), the full Playground scene (M3), PluginHost/RunLoop/Game.dll (M4), runtime backend swap (M3's ImGui dropdown — M1 selects backend at launch).

