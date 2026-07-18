# Grimoire Editor Shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up `Grimoire.exe` — the native ImGui-on-NVRHI editor shell on top of Arcane — with sim-time control, a scene-in-a-panel viewport, a reflection-driven entity hierarchy + inspector, viewport click-pick, and play-in-editor (snapshot on Play, restore on Stop).

**Architecture:** Grimoire lives at `Arcane/Grimoire/` (sibling of Loom), builds an exe that links `Arcane.dll` and consumes only `ARCANE_API` (strict `Astra → Manifold2D → Arcane → Grimoire` one-way stack; Arcane stays editor-free). Task 1 lifts the plugin-host machinery (`PluginHost`/`Module`/`Plugin`) out of `Loom/src` into `Arcane.dll` as shared engine API so Loom, ArcaneTests, and Grimoire consume one implementation. Grimoire reuses Loom's host-boot helpers (`GpuContext`/`FramePerf`/`LoomConfig`) by source-compile, renders the hosted scene into an existing `OffscreenCanvas` shown via `ImGui::Image`, and drives the existing `Runtime`/`RunLoop` facade (sim-time control, `SnapshotRegistry`/`RestoreRegistry`, `Registry().InspectEntity`). The only new *engine* code is one `ARCANE_API` sprite-OBB pick function.

**Tech Stack:** C++23, Arcane engine (`Arcane.dll`, `/MD`), Astra ECS (reflection: `IFieldVisitor`, `Registry::InspectEntity`, `GetEntityManager`), Manifold2D, Dear ImGui (docking) via `imgui_impl_nvrhi` + `imgui_impl_sdl3`, NVRHI (D3D12/Vulkan), SDL3, spdlog, Catch2, premake5/MSBuild (VS2026).

## Global Constraints

- **Modularity is one-way:** `Arcane.dll` must never reference Grimoire; no `#ifdef EDITOR` or editor hooks inside Arcane. Any capability the editor needs from the engine is public `ARCANE_API` (or existing Astra API reached through `Runtime`), never an editor special-case. Grimoire (an exe) may source-compile Loom host helpers — that is host-to-host reuse, not an Arcane→Grimoire dependency.
- **No plugin-ABI change.** `Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp` is unchanged; no ABI-version bump. Grimoire hosts plugins through the same `GamePlugin_*` vtable Loom uses.
- **The Task-1 lift is byte-identical (strangler).** No behavior change; the existing `[loom]` + hot-reload + `[sandbox]` suites passing unchanged are the regression gate.
- **Build (Aphelyon):** `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`. msbuild is NOT on the Bash PATH — use PowerShell or the full path `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
- **Regenerate when the file LIST changes** (new project, new `.cpp`/`.hpp`, or a source moved between projects, or a new test file, or ArcaneTests `files{}` edits): `cd Arcane && GenerateProjects.bat`. The `Arcane` and `Grimoire`/`Loom`/`Sandbox` projects glob `src/**.cpp`/`.hpp`; ArcaneTests lists specific cross-project sources explicitly in `files{}`.
- **Run tests from the output dir** (mandatory — some tests use CWD-relative paths; running elsewhere yields ~6 false failures): `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]`. **Baseline before this work:** `~[gpu]` 27613 assertions / 294 cases.
- **GPU tests are `[gpu]`-tagged** and assert `Arcane::RenderErrorCount() == 0` (latches nvrhi + VK validation). Run interactively at the desk only (Parsec/virtual-display GPU-driver-crash hazard); `~[gpu]` is the dev-loop gate.
- **Commits:** Aphelyon convention `type(scope): summary`, **NO AI trailers / NO `Co-Authored-By`**. Each task commits at its end (staging only its own files by explicit path — the working tree carries pre-existing unrelated dirt: `Client/data/ui_screens/*.json`, `ThirdParty/box2d-3.1.1/`, `AGENTS.md`, `Arcane/.screenshots/`, `Arcane/scripts/launch.*`, `docs/*` plan docs, `Server/cpp_coding_style.txt` — which must NEVER be staged; never `git add -A`).
- **Scene components** (`Arcane/Arcane/src/Arcane/Scene/Components.hpp`): `WorldTransform{ glm::mat3 matrix }` (local→world 2D homogeneous; column 2 = world position); `SpriteRenderer{ uint32_t textureId; glm::vec2 size; glm::vec4 tint; int32_t sortingLayer; int32_t orderInLayer; SpriteShape shape }`. There is **no** Name/Tag/Label component — the hierarchy displays the entity id.
- **Camera transform (canonical, matches `Runtime::SetRenderContext`):** `screen = world * CameraZoom() + CameraOffset()`; inverse `world = (screen - CameraOffset()) / CameraZoom()`. `Runtime::CameraOffset()`/`CameraZoom()` are the values the hosted plugin drives via `SetCamera`. Grimoire unprojects viewport-local pixels with these.

---

# Task 1: Lift plugin-host machinery into Arcane.dll

Move `PluginHost` / `Module` / `Plugin` from `Loom/src/` into the engine at `Arcane/Arcane/src/Arcane/Plugin/` (beside `PluginABI.hpp`), in `namespace Arcane`, exported `ARCANE_API`. Rewire Loom, ArcaneTests, and premake. Byte-identical — the existing suites are the gate.

**Files:**
- Move (git mv) + edit:
  - `Arcane/Loom/src/PluginHost.hpp` → `Arcane/Arcane/src/Arcane/Plugin/PluginHost.hpp`
  - `Arcane/Loom/src/PluginHost.cpp` → `Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp`
  - `Arcane/Loom/src/Module.hpp` → `Arcane/Arcane/src/Arcane/Plugin/Module.hpp`
  - `Arcane/Loom/src/Module.cpp` → `Arcane/Arcane/src/Arcane/Plugin/Module.cpp`
  - `Arcane/Loom/src/Plugin.hpp` → `Arcane/Arcane/src/Arcane/Plugin/Plugin.hpp`
  - `Arcane/Loom/src/Plugin.cpp` → `Arcane/Arcane/src/Arcane/Plugin/Plugin.cpp`
- Modify: `Arcane/Loom/src/Loom.hpp`, `Arcane/Loom/src/Loom.cpp`, `Arcane/premake5.lua`
- Modify test wiring: `Arcane/premake5.lua` (ArcaneTests `files{}`)

**Interfaces:**
- Produces: `Arcane::PluginHost`, `Arcane::Module`, `Arcane::Plugin` (previously global-namespace `PluginHost`/`Module`/`Plugin`), all `ARCANE_API`. `PluginHost(Arcane::Runtime&, std::filesystem::path)`, `.Load()`, `.Unload()`, `.Reload(bool)`, `.Poll()`, `.ForceReload()`, `.ReloadFresh()`, `.IsLoaded()`, `.Vtable()`, `.Generation()` — signatures unchanged.

- [ ] **Step 1: Move the six files with git mv (preserve history).**

```bash
cd /d/dev/starworks/Gacha/Arcane
git mv Loom/src/PluginHost.hpp Arcane/src/Arcane/Plugin/PluginHost.hpp
git mv Loom/src/PluginHost.cpp Arcane/src/Arcane/Plugin/PluginHost.cpp
git mv Loom/src/Module.hpp     Arcane/src/Arcane/Plugin/Module.hpp
git mv Loom/src/Module.cpp     Arcane/src/Arcane/Plugin/Module.cpp
git mv Loom/src/Plugin.hpp     Arcane/src/Arcane/Plugin/Plugin.hpp
git mv Loom/src/Plugin.cpp     Arcane/src/Arcane/Plugin/Plugin.cpp
```

- [ ] **Step 2: Namespace + export the three headers.** In each of `PluginHost.hpp`, `Module.hpp`, `Plugin.hpp`: add `#include <Arcane/Base/Api.hpp>`, wrap the class in `namespace Arcane { ... }`, and mark the class `ARCANE_API`. Example for `PluginHost.hpp` (apply the same shape to `Module`/`Plugin`):

```cpp
#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Arcane
{
    class Runtime;   // was a forward-decl of Arcane::Runtime already

    // (unchanged doc comment)
    class ARCANE_API PluginHost
    {
    public:
        PluginHost(Runtime& runtime, std::filesystem::path sourceDllPath);
        // ... rest unchanged, but drop the now-redundant `Arcane::` qualifier on Runtime/PluginVTable ...
    };
}
```

For `Plugin.hpp`, keep `#include "Module.hpp"` (same directory) and mark `class ARCANE_API Plugin`. For `Module.hpp`, mark `class ARCANE_API Module`.

- [ ] **Step 3: Namespace the three .cpp files.** Wrap each definition file's contents in `namespace Arcane { ... }` (or qualify each definition `Arcane::PluginHost::...`). Update the `#include "PluginHost.hpp"` to `#include <Arcane/Plugin/PluginHost.hpp>` etc. (they now resolve via the Arcane `src` include root). Drop redundant `Arcane::` qualifiers on `Runtime`/`PluginVTable` inside `namespace Arcane`.

- [ ] **Step 4: Rewire Loom to consume the engine copies.** In `Arcane/Loom/src/Loom.hpp`, change `#include "PluginHost.hpp"` → `#include <Arcane/Plugin/PluginHost.hpp>`, and the member `std::optional<PluginHost> m_plugin;` → `std::optional<Arcane::PluginHost> m_plugin;`. In `Arcane/Loom/src/Loom.cpp`, `m_plugin.emplace(...)` and all `m_plugin->...` calls are unchanged except the type is now `Arcane::PluginHost` (the `->` calls need no edit). Grep Loom for any other `PluginHost`/`Plugin `/`Module ` bare references and qualify them `Arcane::`.

- [ ] **Step 5: Update premake — Arcane globs the new sources automatically; fix ArcaneTests + Loom.** The `Arcane` project already globs `src/**.cpp`, so the moved files compile into `Arcane.dll` with no `files{}` edit. In `Arcane/premake5.lua`, the `ArcaneTests` project `files{}` block currently lists the three moved sources — **remove** these three lines (they now come from `Arcane.dll` via the `"Arcane"` link):

```lua
        "%{wks.location}/Loom/src/Module.cpp",
        "%{wks.location}/Loom/src/Plugin.cpp",
        "%{wks.location}/Loom/src/PluginHost.cpp",
```

Keep `"%{wks.location}/Loom/src/LoomConfig.cpp"` (LoomConfig stays Loom-side). Loom globs `Loom/src/**.cpp` so the moved files simply vanish from its build — no Loom `files{}` edit needed.

- [ ] **Step 6: Regenerate + build.**

Run: `cd Arcane && GenerateProjects.bat` then `msbuild Arcane.slnx /p:Configuration=Debug /m`
Expected: clean build. If the linker reports an unresolved `Arcane::Module`/`Arcane::Plugin` symbol from a test TU that constructs them directly, that class needs `ARCANE_API` (Step 2) — confirm all three are exported.

- [ ] **Step 7: Regression gate — existing suites unchanged.**

Run (from output dir): `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[loom]"` then `ArcaneTests.exe "[sandbox]"` then `ArcaneTests.exe ~[gpu]`
Expected: `[loom]` + hot-reload + `[sandbox]` green; `~[gpu]` = 27613/294 unchanged (byte-identical lift adds no tests).

- [ ] **Step 8: Commit.**

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Arcane/src/Arcane/Plugin/PluginHost.hpp Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp \
        Arcane/Arcane/src/Arcane/Plugin/Module.hpp Arcane/Arcane/src/Arcane/Plugin/Module.cpp \
        Arcane/Arcane/src/Arcane/Plugin/Plugin.hpp Arcane/Arcane/src/Arcane/Plugin/Plugin.cpp \
        Arcane/Loom/src/Loom.hpp Arcane/Loom/src/Loom.cpp Arcane/premake5.lua
git commit -m "refactor(arcane): lift plugin-host machinery into Arcane.dll

Move PluginHost/Module/Plugin from Loom/src into Arcane/Plugin as ARCANE_API
(namespace Arcane), so Loom, ArcaneTests, and the coming Grimoire host consume
one implementation instead of source-compiling copies. Byte-identical strangler
move; PluginABI unchanged, no ABI bump. Gate: [loom]+hot-reload+[sandbox] green,
~[gpu] 27613/294 unchanged."
```

---

# Task 2: Grimoire project scaffold + host spine + `--frames` smoke

Create the `Grimoire/` project: an exe that boots the engine (reusing `GpuContext`/`FramePerf`/`LoomConfig`), hosts `Sandbox.dll` via the lifted `Arcane::PluginHost`, runs its own frame loop (advancing the sim through `RunLoop`, drawing a placeholder "Grimoire" ImGui window to the backbuffer), and supports headless `--frames N`.

**Files:**
- Create: `Arcane/Grimoire/src/main.cpp`, `Arcane/Grimoire/src/GrimoireApp.hpp`, `Arcane/Grimoire/src/GrimoireApp.cpp`
- Modify: `Arcane/premake5.lua` (new `Grimoire` project)
- Reuse by source-compile (no copy): `Arcane/Loom/src/GpuContext.{hpp,cpp}`, `Arcane/Loom/src/FramePerf.hpp`, `Arcane/Loom/src/LoomConfig.{hpp,cpp}`

**Interfaces:**
- Consumes: `Arcane::PluginHost` (Task 1); `GpuContext::Create(const LoomConfig&)`, `GpuContext::OnResize`, `GpuContext::Win/Device/Swap/Shaders/Imgui/InDevices/Input/Cmd/FramebufferFor`; `LoomConfig::Parse`; `Arcane::Runtime`, `Runtime::Loop()/Advance/SetInputSnapshot/SetImGui/SetRenderResources/AudioSystem`.
- Produces: `Grimoire::GrimoireApp` with `explicit GrimoireApp(LoomConfig)` and `int Run()`.

- [ ] **Step 1: Add the `Grimoire` premake project.** In `Arcane/premake5.lua`, after the `Loom` project block (ends ~line 461), insert:

```lua
-- ============================================================================
-- Grimoire: the editor shell (Grimoire.exe). Engine boot + RunLoop + PluginHost
-- + ImGui docking shell. Hosts Sandbox.dll by default. Reuses Loom's host-boot
-- helpers (GpuContext/FramePerf/LoomConfig) by source-compile -- host-to-host
-- reuse, NOT an Arcane->Grimoire dependency. Consumes only ARCANE_API otherwise.
-- ============================================================================
project "Grimoire"
    location "Grimoire"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
        -- Loom host-boot helpers, source-shared (same pattern as ArcaneTests).
        "%{wks.location}/Loom/src/GpuContext.cpp",
        "%{wks.location}/Loom/src/LoomConfig.cpp",
    }
    includedirs {
        "%{prj.location}/src",
        "%{wks.location}/Loom/src",      -- GpuContext.hpp / FramePerf.hpp / LoomConfig.hpp
        "%{wks.location}/Arcane/src",
        "%{IncludeDir.Core}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
    }
    links { "Core", "Arcane" }
    dependson { "Sandbox" }
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Sandbox/Sandbox.dll" "%{cfg.buildtarget.directory}/Sandbox.dll"',
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data"',
        '{COPYFILE} "%{wks.location}/Loom/data/input_actions.json" "%{cfg.buildtarget.directory}/data/input_actions.json"',
    }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }
    filter "configurations:Debug"    defines { "ARCANE_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
```

- [ ] **Step 2: Write `GrimoireApp.hpp`.** Mirror `Loom.hpp`'s member/teardown contract (GpuContext outlives runtime outlives plugin):

```cpp
#pragma once
// GrimoireApp: the editor application. Constructed in main from a LoomConfig
// (reused as the host config); Run() drives Init -> the frame loop -> Shutdown.
// Member declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN
// CONTRACT (destruct reverse: plugin Unload while the DLL is still mapped ->
// runtime -> render stack in GpuContext -> window last). Mirrors Loom.
#include <cstdint>
#include <memory>
#include <optional>

#include <LoomConfig.hpp>
#include <GpuContext.hpp>
#include <FramePerf.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>

namespace Astra { class TypeContext; }

namespace Grimoire
{
    class GrimoireApp
    {
    public:
        explicit GrimoireApp(LoomConfig cfg);
        int Run();   // Init() -> MainLoop() -> Shutdown(); process exit code

    private:
        bool Init();
        void MainLoop();
        void Shutdown();

        LoomConfig                        m_config;
        std::unique_ptr<GpuContext>       m_gpu;                    // destructs LAST
        Astra::TypeContext*               m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
        std::optional<Arcane::Runtime>    m_runtime;                // destructs before m_gpu
        std::optional<Arcane::PluginHost> m_plugin;                 // destructs before m_runtime
        FramePerf                         m_perf;
        std::uint64_t                     m_frameCount = 0;
    };
}
```

- [ ] **Step 3: Write `GrimoireApp.cpp` — Init + Shutdown (copy Loom's boot/teardown).** Init mirrors `Loom::Init` exactly (GpuContext::Create, heap-leaked TypeContext, `m_runtime.emplace(m_typeContext, m_config.maxFrames == 0)`, `SetRenderResources`, `SetImGui`, `m_plugin.emplace(*m_runtime, m_config.pluginPath)` + `Load()`). Shutdown mirrors `Loom::Shutdown` (`m_gpu->Device().Nvrhi()->waitForIdle()`). Read `Arcane/Loom/src/Loom.cpp:29-90,247-272` and reproduce, changing the class name to `GrimoireApp` and the log tags to "Grimoire". `Run()`:

```cpp
int GrimoireApp::Run()
{
    if (!Init()) return 1;
    MainLoop();
    Shutdown();
    return 0;
}
```

- [ ] **Step 4: Write `GrimoireApp::MainLoop` — the editor frame loop (placeholder UI for now).** Model on `Loom::MainLoop` (`Arcane/Loom/src/Loom.cpp:92-245`) but the scene renders to the backbuffer for THIS task (viewport panel arrives in Task 4). Key differences to write:

```cpp
void GrimoireApp::MainLoop()
{
    auto simPrev = std::chrono::steady_clock::now();
    auto lastFrameTime = simPrev;
    bool running = true;

    while (running)
    {
        auto events = m_gpu->Win().PumpEvents();
        if (events.quitRequested) break;
        if (events.resized) m_gpu->OnResize(events.width, events.height);
        if (m_gpu->Win().IsMinimized())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Input sample (before ImGui BeginFrame so capture flags are set).
        {
            const auto now = std::chrono::steady_clock::now();
            const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
            lastFrameTime = now;
            const Arcane::InputSnapshot snap =
                m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(),
                                          m_gpu->Imgui().WantCaptureMouse());
            m_runtime->SetInputSnapshot(snap);
            m_gpu->Input().Update(frameDt, snap);
        }

        // Sim advance through the RunLoop with the plugin callbacks interleaved.
        {
            const auto now = std::chrono::steady_clock::now();
            double simDt = std::chrono::duration<double>(now - simPrev).count();
            simPrev = now;
            if (simDt > 0.25) simDt = 0.25;
            const Arcane::PluginVTable* vt = m_plugin->Vtable();
            m_runtime->Loop().Advance(simDt,
                [&](double dt)          { if (vt) vt->FixedUpdate(dt); },
                [&](double dt, double a){ if (vt) vt->Update(dt, a); });
            m_runtime->AudioSystem().Update(simDt);
        }

        // ImGui: placeholder editor window (dockspace + panels arrive in later tasks).
        m_gpu->Imgui().BeginFrame();
        {
            ImGui::Begin("Grimoire");
            ImGui::Text("Backend: %s", Arcane::ToString(m_gpu->Device().Backend()));
            ImGui::Text("Plugin gen: %u", m_plugin->Generation());
            ImGui::Text("Paused: %s", m_runtime->Loop().IsPaused() ? "yes" : "no");
            ImGui::End();
        }
        const Arcane::PluginVTable* vtUI = m_plugin->Vtable();
        if (vtUI && vtUI->DrawUI) vtUI->DrawUI();

        nvrhi::ITexture* backbuffer = m_gpu->Swap().BeginFrame();
        if (!backbuffer) { ImGui::EndFrame(); continue; }

        m_gpu->Cmd()->open();
        // Clear the backbuffer directly (Grimoire's scene will live in a panel,
        // so there is no scene->tonemap->backbuffer pass as in Loom).
        m_gpu->Cmd()->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                        nvrhi::Color(0.06f, 0.06f, 0.08f, 1.0f));
        nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
        m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
        m_gpu->Cmd()->close();
        m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
        m_gpu->Swap().Present();

        m_plugin->Poll();

        ++m_frameCount;
        if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames) running = false;
    }
}
```

Add the includes `GrimoireApp.cpp` needs (mirror `Loom.cpp:7-24`): `<Arcane/Base/Engine.hpp>`, `<Arcane/Base/Log.hpp>`, `<Arcane/Input/InputSnapshot.hpp>`, `<Arcane/Render/Device.hpp>`, `<Astra/Core/TypeContext.hpp>`, `<nvrhi/nvrhi.h>`, `<imgui.h>`, `<chrono>`, `<thread>`, `<filesystem>`.

- [ ] **Step 5: Write `main.cpp`.** Mirror `Loom/src/main.cpp` (install the Mosaic diagnostics sink/handler — Grimoire is a host module, so it installs its own per the diagnostics arc):

```cpp
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <LoomConfig.hpp>
#include "GrimoireApp.hpp"

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;
    Grimoire::GrimoireApp app(*parsed.config);
    return app.Run();
}
```

- [ ] **Step 6: Regenerate + build.**

Run: `cd Arcane && GenerateProjects.bat` then `msbuild Arcane.slnx /p:Configuration=Debug /m`
Expected: `Grimoire.exe` builds; `Arcane.dll` + `Sandbox.dll` + shaders copied beside it.

- [ ] **Step 7: Headless `--frames` GPU smoke.**

Run (desk / GPU box): `cd Arcane\bin\Debug-windows-x86_64-md\Grimoire && .\Grimoire.exe --backend vulkan --frames 60`
Expected: exits 0 after 60 frames, no NVRHI/VK validation errors in the log. (This is the `[gpu]`-equivalent manual smoke; the automated `~[gpu]` suite is unaffected — run it to confirm 27613/294 unchanged.)

- [ ] **Step 8: Commit.**

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Grimoire/src/main.cpp Arcane/Grimoire/src/GrimoireApp.hpp Arcane/Grimoire/src/GrimoireApp.cpp Arcane/premake5.lua
git commit -m "feat(grimoire): project scaffold + host spine + --frames smoke

Grimoire.exe boots the engine (reusing Loom's GpuContext/FramePerf/LoomConfig by
source-compile), hosts Sandbox.dll via the lifted Arcane::PluginHost, advances the
sim through RunLoop, and draws a placeholder ImGui window to the backbuffer.
--frames N runs headless. Consumes only ARCANE_API + host-boot helpers."
```

---

# Task 3: Dockspace + sim-time toolbar + console panel

Turn the placeholder ImGui into an editor shell: enable docking, host a full-window dockspace, add a Play/Pause/Step + time-scale toolbar wired to `RunLoop`, and a Console panel fed by an engine-logger ring-buffer sink.

**Files:**
- Create: `Arcane/Grimoire/src/ConsoleBuffer.hpp`, `Arcane/Grimoire/src/ConsoleBuffer.cpp`, `Arcane/Grimoire/src/EditorPanels.hpp`, `Arcane/Grimoire/src/EditorPanels.cpp`
- Modify: `Arcane/Grimoire/src/GrimoireApp.hpp`, `Arcane/Grimoire/src/GrimoireApp.cpp`
- Test: `Arcane/Tests/src/GrimoireConsoleTest.cpp` + ArcaneTests `files{}` (source-compile `ConsoleBuffer.cpp`)

**Interfaces:**
- Produces: `Grimoire::ConsoleBuffer` — `void Push(std::string)`, `size_t Size() const`, `size_t Capacity() const`, `void ForEach(FunctionRef<void(const std::string&)>) const`, ctor `explicit ConsoleBuffer(size_t capacity)`. `Grimoire::DrawSimTimeToolbar(Arcane::RunLoop&)`, `Grimoire::DrawConsolePanel(const ConsoleBuffer&)`, `Grimoire::BeginDockSpace()`.

- [ ] **Step 1: Write the failing test for the console ring buffer.** Create `Arcane/Tests/src/GrimoireConsoleTest.cpp`:

```cpp
// Grimoire console ring buffer: bounded FIFO of formatted log lines the Console
// panel renders. CPU-only ([grimoire]).

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ConsoleBuffer.hpp>

TEST_CASE("ConsoleBuffer keeps the newest N lines and drops the oldest", "[grimoire]")
{
    Grimoire::ConsoleBuffer buf(3);
    buf.Push("a"); buf.Push("b"); buf.Push("c");
    CHECK(buf.Size() == 3);
    buf.Push("d");                       // evicts "a"
    CHECK(buf.Size() == 3);

    std::vector<std::string> seen;
    buf.ForEach([&](const std::string& s) { seen.push_back(s); });
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "b");
    CHECK(seen[1] == "c");
    CHECK(seen[2] == "d");
}
```

- [ ] **Step 2: Wire the test into ArcaneTests + build to see it fail.** In `Arcane/premake5.lua` ArcaneTests `files{}`, add `"%{wks.location}/Grimoire/src/ConsoleBuffer.cpp"`, and in ArcaneTests `includedirs{}` add `"%{wks.location}/Grimoire/src"`. Run `cd Arcane && GenerateProjects.bat` then build.
Expected: FAIL to build (`ConsoleBuffer.hpp` not found / undefined).

- [ ] **Step 3: Write `ConsoleBuffer.hpp` + `.cpp`.**

```cpp
// ConsoleBuffer.hpp
#pragma once

#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <deque>
#include <string>

namespace Grimoire
{
    // A bounded FIFO of log lines. Push appends; when full the oldest line is
    // dropped. Not thread-safe on its own -- the spdlog sink that feeds it holds
    // spdlog's sink mutex, and the UI reads it on the same (main) thread that
    // pumps ImGui, so pushes and reads never overlap in Grimoire's single-thread
    // frame. (If a worker ever logs, wrap Push in the sink's lock.)
    class ConsoleBuffer
    {
    public:
        explicit ConsoleBuffer(std::size_t capacity) : m_capacity(capacity ? capacity : 1) {}

        void Push(std::string line);
        [[nodiscard]] std::size_t Size() const noexcept { return m_lines.size(); }
        [[nodiscard]] std::size_t Capacity() const noexcept { return m_capacity; }
        void ForEach(Arcane::FunctionRef<void(const std::string&)> fn) const;

    private:
        std::size_t             m_capacity;
        std::deque<std::string> m_lines;
    };
}
```

```cpp
// ConsoleBuffer.cpp
#include <ConsoleBuffer.hpp>

namespace Grimoire
{
    void ConsoleBuffer::Push(std::string line)
    {
        m_lines.push_back(std::move(line));
        while (m_lines.size() > m_capacity) m_lines.pop_front();
    }

    void ConsoleBuffer::ForEach(Arcane::FunctionRef<void(const std::string&)> fn) const
    {
        for (const std::string& l : m_lines) fn(l);
    }
}
```

- [ ] **Step 4: Build + run the test to green.**

Run: build, then `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "[grimoire]"`
Expected: PASS (1 case).

- [ ] **Step 5: Write the panel helpers `EditorPanels.hpp`/`.cpp`.** These are thin ImGui builders (rendered at the desk, not unit-tested — they call ImGui widgets):

```cpp
// EditorPanels.hpp
#pragma once

namespace Arcane { class RunLoop; }

namespace Grimoire
{
    class ConsoleBuffer;

    // Host a full-viewport dockspace (call once per frame between ImGui BeginFrame
    // and the panel Begin/End calls). Enables initial docking of child windows.
    void BeginDockSpace();

    // Play/Pause/Step buttons + a time-scale slider, driving the RunLoop.
    void DrawSimTimeToolbar(Arcane::RunLoop& loop);

    // Scrolling read-only console of captured log lines (autoscroll).
    void DrawConsolePanel(const ConsoleBuffer& console);
}
```

```cpp
// EditorPanels.cpp
#include "EditorPanels.hpp"
#include "ConsoleBuffer.hpp"

#include <Arcane/Sim/RunLoop.hpp>

#include <imgui.h>

namespace Grimoire
{
    void BeginDockSpace()
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("GrimoireDockHost", nullptr, flags);
        ImGui::PopStyleVar(3);
        ImGui::DockSpace(ImGui::GetID("GrimoireDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
        ImGui::End();
    }

    void DrawSimTimeToolbar(Arcane::RunLoop& loop)
    {
        ImGui::Begin("Sim");
        const bool paused = loop.IsPaused();
        if (ImGui::Button(paused ? "Play" : "Pause")) loop.SetPaused(!paused);
        ImGui::SameLine();
        if (ImGui::Button("Step")) loop.RequestSingleStep();
        ImGui::SameLine();
        float scale = static_cast<float>(loop.TimeScale());
        if (ImGui::SliderFloat("time-scale", &scale, 0.0f, 2.0f, "%.2f"))
            loop.SetTimeScale(scale);
        ImGui::End();
    }

    void DrawConsolePanel(const ConsoleBuffer& console)
    {
        ImGui::Begin("Console");
        console.ForEach([](const std::string& line)
        {
            ImGui::TextUnformatted(line.c_str());
        });
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);   // autoscroll while pinned to bottom
        ImGui::End();
    }
}
```

- [ ] **Step 6: Add the engine-logger sink + docking flag + panels to `GrimoireApp`.** In `GrimoireApp.hpp` add members: `#include "ConsoleBuffer.hpp"`, `ConsoleBuffer m_console{512};` and (private) `void InstallConsoleSink();`. In `GrimoireApp.cpp`:
  - In `Init()`, after `GpuContext::Create` succeeds, enable docking: `ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;` and call `InstallConsoleSink();`.
  - `InstallConsoleSink()` attaches a callback sink to the engine logger that pushes formatted lines into `m_console`:

```cpp
#include <spdlog/sinks/callback_sink.h>

void GrimoireApp::InstallConsoleSink()
{
    auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [this](const spdlog::details::log_msg& m)
        {
            m_console.Push(std::string(m.payload.data(), m.payload.size()));
        });
    Arcane::Log::Engine()->sinks().push_back(cb);
}
```

  - In `MainLoop()`, replace the placeholder `ImGui::Begin("Grimoire")` block with:

```cpp
        m_gpu->Imgui().BeginFrame();
        Grimoire::BeginDockSpace();
        Grimoire::DrawSimTimeToolbar(m_runtime->Loop());
        Grimoire::DrawConsolePanel(m_console);
```

  Add `#include "EditorPanels.hpp"` and `#include <spdlog/spdlog.h>` to `GrimoireApp.cpp`.

- [ ] **Step 7: Regenerate (new EditorPanels.cpp) + build + regression.**

Run: `cd Arcane && GenerateProjects.bat` then build; then `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe ~[gpu]`
Expected: `~[gpu]` 27613/294 + 1 `[grimoire]` case = 27615/295 (console test); build clean.

- [ ] **Step 8: Desk-verify + `--frames` smoke, then commit.**

Desk: `Grimoire.exe` shows a dockspace with dockable Sim + Console panels; Play/Pause toggles the sim (Sandbox freezes/thaws), Step advances one tick while paused, time-scale slows motion; physics WARNs (if any) appear in Console.
Smoke: `.\Grimoire.exe --frames 60` exits clean.

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Grimoire/src/ConsoleBuffer.hpp Arcane/Grimoire/src/ConsoleBuffer.cpp \
        Arcane/Grimoire/src/EditorPanels.hpp Arcane/Grimoire/src/EditorPanels.cpp \
        Arcane/Grimoire/src/GrimoireApp.hpp Arcane/Grimoire/src/GrimoireApp.cpp \
        Arcane/Tests/src/GrimoireConsoleTest.cpp Arcane/premake5.lua
git commit -m "feat(grimoire): dockspace + sim-time toolbar + console panel

Enables ImGui docking, hosts a full-viewport dockspace, adds a Play/Pause/Step +
time-scale toolbar wired to RunLoop, and a Console panel fed by a callback sink on
Arcane::Log::Engine() (so Manifold2D/Astra diagnostics surface in-editor). Console
ring buffer is a unit-tested bounded FIFO. Gate: ~[gpu] +1 [grimoire] case."
```

---

# Task 4: Scene-in-a-panel viewport

Render the hosted scene into an `OffscreenCanvas` and show it via `ImGui::Image` in a dockable **Viewport** window. Resize the canvas to the panel; gate scene input on viewport hover/focus; remap the cursor into viewport-local pixels before handing the plugin its input snapshot.

**Files:**
- Create: `Arcane/Grimoire/src/ViewportInput.hpp`, `Arcane/Grimoire/src/ViewportInput.cpp`
- Modify: `Arcane/Grimoire/src/GrimoireApp.hpp`, `Arcane/Grimoire/src/GrimoireApp.cpp`, `Arcane/Grimoire/src/EditorPanels.hpp`, `Arcane/Grimoire/src/EditorPanels.cpp`
- Test: `Arcane/Tests/src/GrimoireViewportInputTest.cpp` + ArcaneTests `files{}` (source-compile `ViewportInput.cpp`)

**Interfaces:**
- Consumes: `Arcane::OffscreenCanvas::Create/Draw/TextureId/Resize/Width/Height`; `Runtime::SetRenderContext(Batcher2D*)`, `Runtime::Loop().SubmitRender()`.
- Produces: `Grimoire::ViewportRect{ float x,y,w,h }`; `bool Grimoire::SceneInputActive(bool hovered, bool focused) noexcept`; `bool Grimoire::ToViewportLocal(ViewportRect, float mx, float my, float& lx, float& ly) noexcept`; `Grimoire::ViewportPanelResult Grimoire::DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH)`.

- [ ] **Step 1: Write the failing test for the input-gating pure functions.** Create `Arcane/Tests/src/GrimoireViewportInputTest.cpp`:

```cpp
// Grimoire viewport input gating: pure predicates (no ImGui context). CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <ViewportInput.hpp>

TEST_CASE("SceneInputActive is hovered OR focused", "[grimoire]")
{
    CHECK_FALSE(Grimoire::SceneInputActive(false, false));
    CHECK      (Grimoire::SceneInputActive(true,  false));
    CHECK      (Grimoire::SceneInputActive(false, true));
    CHECK      (Grimoire::SceneInputActive(true,  true));
}

TEST_CASE("ToViewportLocal translates inside the rect and rejects outside", "[grimoire]")
{
    const Grimoire::ViewportRect r{ 100.0f, 50.0f, 640.0f, 480.0f };
    float lx = 0, ly = 0;

    CHECK(Grimoire::ToViewportLocal(r, 100.0f, 50.0f, lx, ly));
    CHECK(lx == 0.0f);
    CHECK(ly == 0.0f);

    CHECK(Grimoire::ToViewportLocal(r, 420.0f, 290.0f, lx, ly));
    CHECK(lx == 320.0f);
    CHECK(ly == 240.0f);

    CHECK_FALSE(Grimoire::ToViewportLocal(r, 99.0f,  60.0f, lx, ly));   // left of rect
    CHECK_FALSE(Grimoire::ToViewportLocal(r, 200.0f, 49.0f, lx, ly));   // above rect
    CHECK_FALSE(Grimoire::ToViewportLocal(r, 740.0f, 60.0f, lx, ly));   // right edge (w exclusive)
}
```

- [ ] **Step 2: Wire into ArcaneTests + build to fail.** Add `"%{wks.location}/Grimoire/src/ViewportInput.cpp"` to ArcaneTests `files{}`. Regenerate + build.
Expected: FAIL (`ViewportInput.hpp` not found).

- [ ] **Step 3: Write `ViewportInput.hpp` + `.cpp`.**

```cpp
// ViewportInput.hpp
#pragma once

namespace Grimoire
{
    struct ViewportRect { float x, y, w, h; };

    // Scene input (camera pan/zoom, click-pick) is live only when the Viewport
    // panel owns the cursor: hovered (mouse/wheel) or focused (keys).
    inline bool SceneInputActive(bool hovered, bool focused) noexcept { return hovered || focused; }

    // Map a window-global cursor (mx,my) to viewport-local pixels (lx,ly), origin
    // at the image's top-left. Returns false (and still writes lx/ly) if the cursor
    // is outside the rect. Right/bottom edges are exclusive.
    bool ToViewportLocal(ViewportRect r, float mx, float my, float& lx, float& ly) noexcept;
}
```

```cpp
// ViewportInput.cpp
#include "ViewportInput.hpp"

namespace Grimoire
{
    bool ToViewportLocal(ViewportRect r, float mx, float my, float& lx, float& ly) noexcept
    {
        lx = mx - r.x;
        ly = my - r.y;
        return lx >= 0.0f && ly >= 0.0f && lx < r.w && ly < r.h;
    }
}
```

- [ ] **Step 4: Build + run to green.**

Run: build, then `.\ArcaneTests.exe "[grimoire]"`
Expected: PASS (console + 2 viewport-input cases).

- [ ] **Step 5: Add the Viewport panel builder to `EditorPanels`.** In `EditorPanels.hpp` add:

```cpp
#include "ViewportInput.hpp"
#include <cstdint>

namespace Grimoire
{
    struct ViewportPanelResult
    {
        ViewportRect imageRect{};   // screen-space rect of the drawn image
        bool         hovered = false;
        bool         focused = false;
        uint32_t     desiredW = 0;  // content-region size (for OffscreenCanvas::Resize)
        uint32_t     desiredH = 0;
    };

    // Draw the scene texture into a dockable Viewport window; report its rect,
    // hover/focus, and the content-region size the offscreen canvas should match.
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH);
}
```

In `EditorPanels.cpp`:

```cpp
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH)
    {
        ViewportPanelResult r;
        ImGui::Begin("Viewport");
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        r.desiredW = avail.x > 0 ? static_cast<uint32_t>(avail.x) : 1;
        r.desiredH = avail.y > 0 ? static_cast<uint32_t>(avail.y) : 1;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (textureId != 0 && texW > 0 && texH > 0)
            ImGui::Image((ImTextureID)textureId, ImVec2((float)texW, (float)texH));
        r.imageRect = ViewportRect{ origin.x, origin.y, (float)texW, (float)texH };
        r.hovered = ImGui::IsWindowHovered();
        r.focused = ImGui::IsWindowFocused();
        ImGui::End();
        return r;
    }
```

- [ ] **Step 6: Create the OffscreenCanvas + wire the viewport into `GrimoireApp`.** In `GrimoireApp.hpp` add `#include <Arcane/Render/OffscreenCanvas.hpp>` and members `std::unique_ptr<Arcane::OffscreenCanvas> m_viewport;` and `Grimoire::ViewportRect m_viewportRect{};` and `bool m_viewportActive = false;`. In `GrimoireApp.cpp`:
  - In `Init()` after the runtime is up (device is non-null in an interactive host): `m_viewport = Arcane::OffscreenCanvas::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(), 1280, 720);` (null-check; log ARC_ERROR + return false on null).
  - In `MainLoop()`, render the scene into the offscreen BEFORE building ImGui (so the texture is ready when ImGui samples it), reusing the homogenized render path — set the render context to the offscreen batcher and run the render scheduler:

```cpp
        // Scene -> offscreen canvas (the SAME canvas->batcher->tonemap path Loom
        // drives, but into a panel texture). SetRenderContext writes RenderContext2D
        // in Arcane.dll and applies the plugin's camera; SubmitRender runs the render
        // scheduler (sprite submission + physics debug overlay) into this batcher.
        m_viewport->Draw(
            [&](Arcane::Batcher2D& b)
            {
                m_runtime->SetRenderContext(&b);
                m_runtime->Loop().SubmitRender();
            },
            glm::vec4(0.02f, 0.02f, 0.04f, 1.0f));
```

  - After `BeginDockSpace()`, draw the viewport panel and resize the canvas to the panel, and compute the input gate:

```cpp
        Grimoire::ViewportPanelResult vp =
            Grimoire::DrawViewportPanel(m_viewport->TextureId(),
                                        m_viewport->Width(), m_viewport->Height());
        if (vp.desiredW != m_viewport->Width() || vp.desiredH != m_viewport->Height())
            m_viewport->Resize(vp.desiredW, vp.desiredH);
        m_viewportRect = vp.imageRect;
        m_viewportActive = Grimoire::SceneInputActive(vp.hovered, vp.focused);
```

  - Change the input feed so the plugin only sees scene-relevant input when the viewport is active, and with the mouse remapped into viewport-local pixels. Replace the `SetInputSnapshot(snap)` line in the input block with:

```cpp
            Arcane::InputSnapshot pluginSnap = snap;
            float lx = 0, ly = 0;
            const bool inViewport =
                m_viewportActive &&
                Grimoire::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, lx, ly);
            if (inViewport)
            {
                pluginSnap.mouseX = lx;      // plugin camera works in viewport-local px
                pluginSnap.mouseY = ly;      // (panel size == offscreen size => scale 1)
            }
            else
            {
                // Not in the viewport: neutralize mouse buttons/scroll so the plugin's
                // camera does not pan and spawn/drag does not fire while editing panels.
                pluginSnap.mouseButtons = 0;
                pluginSnap.scrollDelta  = 0.0f;
            }
            m_runtime->SetInputSnapshot(pluginSnap);
```

  **Note:** read `Arcane/Arcane/src/Arcane/Input/InputSnapshot.hpp` for the exact field names (`mouseX`/`mouseY`/`mouseButtons`/`scrollDelta` here are indicative — use the real ones; if the field names differ, match them and keep the semantics: pass viewport-local mouse when active, zero the button/scroll state when not).

- [ ] **Step 7: Regenerate (new ViewportInput.cpp) + build + regression + smoke.**

Run: `cd Arcane && GenerateProjects.bat` then build; `.\ArcaneTests.exe ~[gpu]` (expect 27617/297: +2 viewport-input cases over Task 3); desk `.\Grimoire.exe` shows the Sandbox scene inside the Viewport panel, pan/zoom works only when hovering the viewport, and `.\Grimoire.exe --frames 60` exits clean.

- [ ] **Step 8: Commit.**

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Grimoire/src/ViewportInput.hpp Arcane/Grimoire/src/ViewportInput.cpp \
        Arcane/Grimoire/src/GrimoireApp.hpp Arcane/Grimoire/src/GrimoireApp.cpp \
        Arcane/Grimoire/src/EditorPanels.hpp Arcane/Grimoire/src/EditorPanels.cpp \
        Arcane/Tests/src/GrimoireViewportInputTest.cpp Arcane/premake5.lua
git commit -m "feat(grimoire): scene-in-a-panel viewport

Render the hosted scene into an OffscreenCanvas (the same canvas->batcher->tonemap
path Loom drives) and show it via ImGui::Image in a dockable Viewport window,
resized to the panel. Scene input is gated on viewport hover/focus and the cursor
is remapped into viewport-local pixels before the plugin snapshot. Pure input-gate
functions unit-tested. Gate: ~[gpu] +2 [grimoire] cases."
```

---

# Task 5: Entity hierarchy + selection

Add a Hierarchy panel that enumerates every live entity via the existing `Registry::GetEntityManager()` and a `SelectionContext` shared across panels. Clicking a row selects its entity.

**Files:**
- Create: `Arcane/Grimoire/src/SelectionContext.hpp`, `Arcane/Grimoire/src/EntityList.hpp`, `Arcane/Grimoire/src/EntityList.cpp`
- Modify: `Arcane/Grimoire/src/EditorPanels.hpp`, `Arcane/Grimoire/src/EditorPanels.cpp`, `Arcane/Grimoire/src/GrimoireApp.hpp`, `Arcane/Grimoire/src/GrimoireApp.cpp`
- Test: `Arcane/Tests/src/GrimoireEntityListTest.cpp` + ArcaneTests `files{}` (source-compile `EntityList.cpp`)

**Interfaces:**
- Consumes: `Runtime::Registry()`; `Astra::Registry::GetEntityManager()` (range-for over `Astra::Entity`), `Astra::Registry::Size()`, `Astra::Entity::GetID()/GetVersion()`, `Astra::Registry::GetEntityComponents(Entity)`.
- Produces: `std::vector<Astra::Entity> Grimoire::CollectEntities(Astra::Registry&)`; `Grimoire::SelectionContext{ Astra::Entity selected; std::vector<Astra::Entity> pickCandidates; size_t pickCycle; }` with `void Select(Astra::Entity)`, `void Clear()`, `bool HasSelection() const`; `Grimoire::DrawHierarchyPanel(Astra::Registry&, SelectionContext&)`.

- [ ] **Step 1: Write the failing test for entity collection + selection.** Create `Arcane/Tests/src/GrimoireEntityListTest.cpp`. Build a registry with the shared TypeContext and Scene components registered — **mirror the setup in `Arcane/Tests/src/RenderInterpolationTest.cpp`** (the `[interp]` tests already construct a registry and register `WorldTransform`/`SpriteRenderer`); copy its fixture helper. Then:

```cpp
// Grimoire entity enumeration + selection. CPU-only ([grimoire]).

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>

#include <EntityList.hpp>
#include <SelectionContext.hpp>

// Uses the same registry-with-Scene-components fixture as RenderInterpolationTest.
// (Copy MakeSceneRegistry()/component registration from that test file.)

TEST_CASE("CollectEntities returns every live entity", "[grimoire]")
{
    auto reg = MakeSceneRegistry();          // fixture: fresh registry, Scene comps registered
    const Astra::Entity a = reg->CreateEntity();
    const Astra::Entity b = reg->CreateEntity();
    const Astra::Entity c = reg->CreateEntity();

    std::vector<Astra::Entity> all = Grimoire::CollectEntities(*reg);
    REQUIRE(all.size() == 3);
    // ids present regardless of order
    bool ha=false, hb=false, hc=false;
    for (Astra::Entity e : all) { ha |= (e.GetID()==a.GetID()); hb |= (e.GetID()==b.GetID()); hc |= (e.GetID()==c.GetID()); }
    CHECK((ha && hb && hc));
}

TEST_CASE("SelectionContext tracks and clears selection", "[grimoire]")
{
    Grimoire::SelectionContext sel;
    CHECK_FALSE(sel.HasSelection());
    Grimoire::SelectionContext::EntityT e(42u, 1u);   // see note in SelectionContext.hpp
    sel.Select(e);
    CHECK(sel.HasSelection());
    CHECK(sel.selected.GetID() == 42u);
    sel.Clear();
    CHECK_FALSE(sel.HasSelection());
}
```

- [ ] **Step 2: Wire into ArcaneTests + build to fail.** Add `"%{wks.location}/Grimoire/src/EntityList.cpp"` to ArcaneTests `files{}`. Regenerate + build.
Expected: FAIL (`EntityList.hpp`/`SelectionContext.hpp` not found).

- [ ] **Step 3: Write `SelectionContext.hpp`.**

```cpp
#pragma once

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <vector>

namespace Grimoire
{
    // The one selected-entity source of truth, shared by hierarchy, inspector,
    // and viewport pick. pickCandidates/pickCycle hold the last click's sorted
    // hit stack so alt-click can cycle (Task 7).
    struct SelectionContext
    {
        using EntityT = Astra::Entity;

        Astra::Entity              selected = Astra::Entity::Invalid();
        std::vector<Astra::Entity> pickCandidates;
        std::size_t                pickCycle = 0;

        [[nodiscard]] bool HasSelection() const noexcept { return selected.IsValid(); }
        void Select(Astra::Entity e) noexcept { selected = e; }
        void Clear() noexcept { selected = Astra::Entity::Invalid(); pickCandidates.clear(); pickCycle = 0; }
    };
}
```

- [ ] **Step 4: Write `EntityList.hpp` + `.cpp`.**

```cpp
// EntityList.hpp
#pragma once

#include <Astra/Entity/Entity.hpp>

#include <vector>

namespace Astra { class Registry; }

namespace Grimoire
{
    // Every live entity in the registry, in EntityManager iteration order. A pure
    // read -- the hierarchy panel's data source. (No Name component exists yet, so
    // the panel labels rows by entity id.)
    std::vector<Astra::Entity> CollectEntities(Astra::Registry& registry);
}
```

```cpp
// EntityList.cpp
#include "EntityList.hpp"

#include <Astra/Registry/Registry.hpp>

namespace Grimoire
{
    std::vector<Astra::Entity> CollectEntities(Astra::Registry& registry)
    {
        std::vector<Astra::Entity> out;
        out.reserve(registry.Size());
        for (Astra::Entity e : registry.GetEntityManager())
            out.push_back(e);
        return out;
    }
}
```

- [ ] **Step 5: Build + run to green.**

Run: build, then `.\ArcaneTests.exe "[grimoire]"`
Expected: PASS (console + viewport-input + 2 entity/selection cases).

- [ ] **Step 6: Add the Hierarchy panel.** In `EditorPanels.hpp`:

```cpp
namespace Astra { class Registry; }
namespace Grimoire
{
    struct SelectionContext;
    // List every live entity; clicking a row selects it. Labels rows by id
    // ("Entity <id> (v<version>)") since no Name component exists yet.
    void DrawHierarchyPanel(Astra::Registry& registry, SelectionContext& sel);
}
```

In `EditorPanels.cpp` (add `#include "EntityList.hpp"`, `#include "SelectionContext.hpp"`, `#include <Astra/Registry/Registry.hpp>`):

```cpp
    void DrawHierarchyPanel(Astra::Registry& registry, SelectionContext& sel)
    {
        ImGui::Begin("Hierarchy");
        for (Astra::Entity e : Grimoire::CollectEntities(registry))
        {
            char label[64];
            std::snprintf(label, sizeof(label), "Entity %u (v%u)",
                          (unsigned)e.GetID(), (unsigned)e.GetVersion());
            const bool isSel = sel.HasSelection() && sel.selected.GetValue() == e.GetValue();
            if (ImGui::Selectable(label, isSel))
                sel.Select(e);
        }
        ImGui::End();
    }
```

(Add `#include <cstdio>` for `snprintf`.)

- [ ] **Step 7: Wire the panel + selection into `GrimoireApp`.** In `GrimoireApp.hpp` add `#include "SelectionContext.hpp"` and member `Grimoire::SelectionContext m_selection;`. In `MainLoop()` after the viewport panel, add `Grimoire::DrawHierarchyPanel(m_runtime->Registry(), m_selection);`.

- [ ] **Step 8: Regenerate + build + regression + commit.**

Run: `cd Arcane && GenerateProjects.bat` then build; `.\ArcaneTests.exe ~[gpu]` (expect 27619/299: +2 over Task 4). Desk: the Hierarchy lists Sandbox entities; clicking highlights a row.

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Grimoire/src/SelectionContext.hpp Arcane/Grimoire/src/EntityList.hpp Arcane/Grimoire/src/EntityList.cpp \
        Arcane/Grimoire/src/EditorPanels.hpp Arcane/Grimoire/src/EditorPanels.cpp \
        Arcane/Grimoire/src/GrimoireApp.hpp Arcane/Grimoire/src/GrimoireApp.cpp \
        Arcane/Tests/src/GrimoireEntityListTest.cpp Arcane/premake5.lua
git commit -m "feat(grimoire): entity hierarchy + selection

Hierarchy panel enumerates every live entity via Registry::GetEntityManager()
(labelled by id; no Name component yet) and a shared SelectionContext holds the
selected entity across panels. CollectEntities + SelectionContext unit-tested.
Gate: ~[gpu] +2 [grimoire] cases."
```

---

# Task 6: Reflection inspector

Add an Inspector panel that, for the selected entity, enumerates components via `Registry::InspectEntity` and edits reflected fields in place through an `IFieldVisitor`. Supported scalar/vector/bool types get editors; unsupported types render read-only.

**Files:**
- Create: `Arcane/Grimoire/src/InspectorFields.hpp`, `Arcane/Grimoire/src/InspectorFields.cpp`
- Modify: `Arcane/Grimoire/src/EditorPanels.hpp`, `Arcane/Grimoire/src/EditorPanels.cpp`, `Arcane/Grimoire/src/GrimoireApp.cpp`
- Test: `Arcane/Tests/src/GrimoireInspectorTest.cpp` + ArcaneTests `files{}` (source-compile `InspectorFields.cpp`)

**Interfaces:**
- Consumes: `Astra::Registry::InspectEntity(Entity)` → `std::vector<Astra::ComponentInfo{ const ComponentDescriptor* descriptor; const TypeMeta* meta; void* data }>`; `Astra::ComponentDescriptor::visitFields(void*, IFieldVisitor&)`; `Astra::FieldInfo{ name, typeHash, isArithmetic, isEnum, isPointer, size, ... }` + `FieldInfo::GetPtr<T>(void*)`, `Get<T>`, `IsSerializable()`; `Astra::IFieldVisitor`.
- Produces: `Grimoire::FieldKind Grimoire::ClassifyField(const Astra::FieldInfo&) noexcept` (enum `{ Bool, Int32, Float, Vec2, Vec3, ReadOnly }`); `Grimoire::ApplyFloatEdit(const Astra::FieldInfo&, void* instance, float v)` (and `ApplyIntEdit`, `ApplyBoolEdit`) — pure writers testable without ImGui; `Grimoire::DrawInspectorPanel(Astra::Registry&, const SelectionContext&)`.

- [ ] **Step 1: Write the failing test for classification + write-back.** Create `Arcane/Tests/src/GrimoireInspectorTest.cpp`. Reuse the Scene-component registry fixture (as in Task 5). Test the classification of `SpriteRenderer` fields and a live write-back round-trip:

```cpp
// Grimoire inspector: field classification + reflected write-back. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>

#include <Arcane/Scene/Components.hpp>

#include <InspectorFields.hpp>

TEST_CASE("ClassifyField maps arithmetic/bool fields, ReadOnly otherwise", "[grimoire]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    bool sawFloatOrInt = false;
    for (const Astra::FieldInfo& f : meta->fields)
    {
        const Grimoire::FieldKind k = Grimoire::ClassifyField(f);
        if (f.name == "sortingLayer" || f.name == "orderInLayer")
            CHECK(k == Grimoire::FieldKind::Int32);
        if (k == Grimoire::FieldKind::Float || k == Grimoire::FieldKind::Int32) sawFloatOrInt = true;
    }
    CHECK(sawFloatOrInt);
}

TEST_CASE("ApplyIntEdit writes through reflection to the live component", "[grimoire]")
{
    Arcane::SpriteRenderer sprite;
    sprite.sortingLayer = 0;
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    const Astra::FieldInfo* layer = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "sortingLayer") layer = &f;
    REQUIRE(layer != nullptr);

    Grimoire::ApplyIntEdit(*layer, &sprite, 7);
    CHECK(sprite.sortingLayer == 7);
}
```

- [ ] **Step 2: Wire into ArcaneTests + build to fail.** Add `"%{wks.location}/Grimoire/src/InspectorFields.cpp"` to ArcaneTests `files{}`. Regenerate + build.
Expected: FAIL (`InspectorFields.hpp` not found).

- [ ] **Step 3: Write `InspectorFields.hpp` + `.cpp`.** The classifier keys off `FieldInfo` flags + `typeHash` (match glm vec types by their type hash). Writers use `FieldInfo::GetPtr<T>`:

```cpp
// InspectorFields.hpp
#pragma once

#include <Astra/Reflection/FieldInfo.hpp>

namespace Grimoire
{
    enum class FieldKind { Bool, Int32, Float, Vec2, Vec3, ReadOnly };

    // Classify a reflected field into an editor kind. Unknown/compound types ->
    // ReadOnly (shown disabled, never crashing).
    FieldKind ClassifyField(const Astra::FieldInfo& f) noexcept;

    // Pure write-backs (no ImGui) so the round-trip is unit-testable.
    void ApplyBoolEdit (const Astra::FieldInfo& f, void* instance, bool  v) noexcept;
    void ApplyIntEdit  (const Astra::FieldInfo& f, void* instance, int   v) noexcept;
    void ApplyFloatEdit(const Astra::FieldInfo& f, void* instance, float v) noexcept;
}
```

```cpp
// InspectorFields.cpp
#include "InspectorFields.hpp"

#include <Astra/Core/Reflection.hpp>   // Astra::TypeHash<T>() -- confirm the exact header/name in the Astra tree
#include <glm/glm.hpp>

namespace Grimoire
{
    FieldKind ClassifyField(const Astra::FieldInfo& f) noexcept
    {
        if (f.isPointer) return FieldKind::ReadOnly;
        // bool is arithmetic; check size==1 + a bool type hash to separate from int8.
        static const uint64_t kBool = Astra::TypeHash<bool>();
        static const uint64_t kF32  = Astra::TypeHash<float>();
        static const uint64_t kI32  = Astra::TypeHash<int32_t>();
        static const uint64_t kVec2 = Astra::TypeHash<glm::vec2>();
        static const uint64_t kVec3 = Astra::TypeHash<glm::vec3>();
        if (f.typeHash == kBool) return FieldKind::Bool;
        if (f.typeHash == kF32)  return FieldKind::Float;
        if (f.typeHash == kI32)  return FieldKind::Int32;
        if (f.typeHash == kVec2) return FieldKind::Vec2;
        if (f.typeHash == kVec3) return FieldKind::Vec3;
        return FieldKind::ReadOnly;
    }

    void ApplyBoolEdit(const Astra::FieldInfo& f, void* instance, bool v) noexcept
    { if (bool* p = f.GetPtr<bool>(instance)) *p = v; }

    void ApplyIntEdit(const Astra::FieldInfo& f, void* instance, int v) noexcept
    { if (int32_t* p = f.GetPtr<int32_t>(instance)) *p = static_cast<int32_t>(v); }

    void ApplyFloatEdit(const Astra::FieldInfo& f, void* instance, float v) noexcept
    { if (float* p = f.GetPtr<float>(instance)) *p = v; }
}
```

**Note:** confirm the exact Astra type-hash API (the reflection system computes `typeHash` as `XXHash64` of the type — find the function that produces the same hash, e.g. `Astra::TypeHash<T>()` or the macro the reflection registration uses, in `ThirdParty/Astra/include/Astra/Reflection/`). If the accessor name differs, use the real one so `ClassifyField`'s comparisons match the `FieldInfo::typeHash` values. If `bool` cannot be distinguished from `int8` by hash, fall back to `f.size == 1 && f.isArithmetic` for `Bool`.

- [ ] **Step 4: Build + run to green.**

Run: build, then `.\ArcaneTests.exe "[grimoire]"`
Expected: PASS (+2 inspector cases).

- [ ] **Step 5: Write the Inspector panel with an ImGui field visitor.** In `EditorPanels.hpp`:

```cpp
namespace Grimoire
{
    struct SelectionContext;
    // Show the selected entity's components (via Registry::InspectEntity) and edit
    // reflected fields in place; unsupported types render read-only.
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel);
}
```

In `EditorPanels.cpp`, add an `IFieldVisitor` that renders a widget per field and applies edits (add includes `<Astra/Reflection/FieldVisitor.hpp>`, `"InspectorFields.hpp"`):

```cpp
namespace
{
    struct ImGuiFieldVisitor : Astra::IFieldVisitor
    {
        bool IsWriting() const noexcept override { return true; }
        void Visit(const Astra::FieldInfo& f, void* instance) override
        {
            ImGui::PushID(static_cast<int>(f.nameHash));
            const std::string label(f.name);
            switch (Grimoire::ClassifyField(f))
            {
                case Grimoire::FieldKind::Bool:
                {
                    bool v = f.Get<bool>(instance);
                    if (ImGui::Checkbox(label.c_str(), &v)) Grimoire::ApplyBoolEdit(f, instance, v);
                    break;
                }
                case Grimoire::FieldKind::Int32:
                {
                    int v = f.Get<int32_t>(instance);
                    if (ImGui::DragInt(label.c_str(), &v)) Grimoire::ApplyIntEdit(f, instance, v);
                    break;
                }
                case Grimoire::FieldKind::Float:
                {
                    float v = f.Get<float>(instance);
                    if (ImGui::DragFloat(label.c_str(), &v, 0.1f)) Grimoire::ApplyFloatEdit(f, instance, v);
                    break;
                }
                case Grimoire::FieldKind::Vec2:
                {
                    glm::vec2 v = f.Get<glm::vec2>(instance);
                    if (ImGui::DragFloat2(label.c_str(), &v.x, 0.1f))
                        if (glm::vec2* p = f.GetPtr<glm::vec2>(instance)) *p = v;
                    break;
                }
                case Grimoire::FieldKind::Vec3:
                {
                    glm::vec3 v = f.Get<glm::vec3>(instance);
                    if (ImGui::DragFloat3(label.c_str(), &v.x, 0.1f))
                        if (glm::vec3* p = f.GetPtr<glm::vec3>(instance)) *p = v;
                    break;
                }
                case Grimoire::FieldKind::ReadOnly:
                default:
                    ImGui::BeginDisabled();
                    ImGui::Text("%s (unsupported)", label.c_str());
                    ImGui::EndDisabled();
                    break;
            }
            ImGui::PopID();
        }
    };
}

void Grimoire::DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel)
{
    ImGui::Begin("Inspector");
    if (!sel.HasSelection())
    {
        ImGui::TextDisabled("No selection");
        ImGui::End();
        return;
    }
    for (const Astra::ComponentInfo& ci : registry.InspectEntity(sel.selected))
    {
        if (!ci.descriptor || !ci.descriptor->visitFields || !ci.data) continue;
        const char* typeName = ci.meta ? ci.meta->typeName.data() : "<unreflected>";
        if (ImGui::CollapsingHeader(typeName, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGuiFieldVisitor visitor;
            ci.descriptor->visitFields(ci.data, visitor);
        }
    }
    ImGui::End();
}
```

**Note:** confirm `TypeMeta::typeName`'s exact type (`std::string_view` per the extraction) — use `.data()` only if NUL-terminated, else format into a `std::string`. Confirm `ComponentInfo`/`ComponentDescriptor` field names against `ThirdParty/Astra/include/Astra/Registry/Registry.hpp` and `Component/Component.hpp`.

- [ ] **Step 6: Wire the panel into `GrimoireApp`.** In `MainLoop()` after the Hierarchy panel: `Grimoire::DrawInspectorPanel(m_runtime->Registry(), m_selection);`.

- [ ] **Step 7: Regenerate + build + regression + smoke + commit.**

Run: `cd Arcane && GenerateProjects.bat` then build; `.\ArcaneTests.exe ~[gpu]` (expect 27621/301: +2 over Task 5). Desk: select an entity in the Hierarchy → Inspector shows its components; dragging a float/int field changes the live sim; a `glm::mat3`/enum field shows "(unsupported)" disabled, no crash.

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Grimoire/src/InspectorFields.hpp Arcane/Grimoire/src/InspectorFields.cpp \
        Arcane/Grimoire/src/EditorPanels.hpp Arcane/Grimoire/src/EditorPanels.cpp \
        Arcane/Grimoire/src/GrimoireApp.cpp \
        Arcane/Tests/src/GrimoireInspectorTest.cpp Arcane/premake5.lua
git commit -m "feat(grimoire): reflection-driven inspector

Inspector enumerates the selected entity's components via Registry::InspectEntity
and edits reflected fields in place through an IFieldVisitor (bool/int/float/vec2/
vec3 editors; unsupported types render read-only, never crashing). Field
classification + write-backs are pure and unit-tested. Gate: ~[gpu] +2 [grimoire]."
```

---

# Task 7: Entity pick (`ARCANE_API`) + viewport click-pick + alt-cycle

Add `Arcane::PickEntitiesAt` — a sprite-OBB collect-and-sort pick in the engine (reusable, headless-testable) — and wire the viewport click to select the top hit, with alt-click cycling the stack.

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Scene/EntityPick.hpp`, `Arcane/Arcane/src/Arcane/Scene/EntityPick.cpp`
- Modify: `Arcane/Grimoire/src/GrimoireApp.cpp`, `Arcane/Grimoire/src/EditorPanels.cpp` (Viewport click handling)
- Test: `Arcane/Tests/src/EntityPickTest.cpp` (engine test — `Arcane.dll` globs `EntityPick.cpp` automatically)

**Interfaces:**
- Consumes: `Arcane::WorldTransform{ glm::mat3 matrix }`, `Arcane::SpriteRenderer{ glm::vec2 size; int32_t sortingLayer; int32_t orderInLayer }`; `Astra::Registry::CreateView<...>().ForEach`; `Runtime::CameraOffset()/CameraZoom()`; `SelectionContext`.
- Produces: `ARCANE_API std::vector<Astra::Entity> Arcane::PickEntitiesAt(Astra::Registry&, glm::vec2 worldPoint)` — hits whose sprite OBB contains the point, sorted front-most first by `(sortingLayer, orderInLayer)` descending (stable). `front()` is the top pick.

- [ ] **Step 1: Write the failing engine test.** Create `Arcane/Tests/src/EntityPickTest.cpp`. Reuse the Scene-component registry fixture (as in Task 5/6):

```cpp
// Arcane sprite-OBB entity pick. CPU-only ([pick]).

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/EntityPick.hpp>

// MakeSceneRegistry(): fresh registry with Scene components registered (copy the
// helper from RenderInterpolationTest.cpp).

namespace
{
    Astra::Entity SpawnSprite(Astra::Registry& reg, glm::vec2 pos, glm::vec2 size,
                              int32_t layer, int32_t order)
    {
        const Astra::Entity e = reg.CreateEntity();
        Arcane::WorldTransform wt;
        wt.matrix = glm::mat3(1.0f);
        wt.matrix[2] = glm::vec3(pos, 1.0f);          // column 2 = world position
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::SpriteRenderer sp;
        sp.size = size; sp.sortingLayer = layer; sp.orderInLayer = order;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
        return e;
    }
}

TEST_CASE("PickEntitiesAt hits inside the sprite OBB and misses outside", "[pick]")
{
    auto reg = MakeSceneRegistry();
    SpawnSprite(*reg, {0, 0}, {2, 2}, 0, 0);          // box [-1,1]^2 at origin

    CHECK(Arcane::PickEntitiesAt(*reg, {0.0f, 0.0f}).size() == 1);
    CHECK(Arcane::PickEntitiesAt(*reg, {0.9f, 0.9f}).size() == 1);
    CHECK(Arcane::PickEntitiesAt(*reg, {1.5f, 0.0f}).empty());   // outside half-extent 1
}

TEST_CASE("PickEntitiesAt sorts overlapping hits front-most first", "[pick]")
{
    auto reg = MakeSceneRegistry();
    const Astra::Entity back  = SpawnSprite(*reg, {0, 0}, {4, 4}, 0, 0);
    const Astra::Entity front = SpawnSprite(*reg, {0, 0}, {4, 4}, 5, 0);   // higher layer

    std::vector<Astra::Entity> hits = Arcane::PickEntitiesAt(*reg, {0.0f, 0.0f});
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].GetValue() == front.GetValue());   // front-most first
    CHECK(hits[1].GetValue() == back.GetValue());
}

TEST_CASE("PickEntitiesAt respects a rotated + translated world matrix", "[pick]")
{
    auto reg = MakeSceneRegistry();
    const Astra::Entity e = reg->CreateEntity();
    Arcane::WorldTransform wt;
    // translate (10,0), rotate 90deg: local +x maps to world +y.
    glm::mat3 m(1.0f);
    const float c = 0.0f, s = 1.0f;                   // cos/sin 90deg
    m[0] = glm::vec3( c, s, 0.0f);
    m[1] = glm::vec3(-s, c, 0.0f);
    m[2] = glm::vec3(10.0f, 0.0f, 1.0f);
    wt.matrix = m;
    reg->AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp; sp.size = {2, 6};       // long along local y
    reg->AddComponent<Arcane::SpriteRenderer>(e, sp);

    CHECK(Arcane::PickEntitiesAt(*reg, {10.0f, 2.0f}).size() == 1);   // inside the rotated long axis
    CHECK(Arcane::PickEntitiesAt(*reg, {13.0f, 0.0f}).empty());       // beyond the short axis
}
```

**Note:** confirm the exact `Registry::AddComponent`/`CreateEntity` signatures in the Astra headers (the extraction shows `CreateEntity()` and component add exist; match the real call, e.g. `AddComponent<T>(e, value)` vs `EmplaceComponent`). Mirror whatever `RenderInterpolationTest.cpp` uses to add `WorldTransform`/`SpriteRenderer`.

- [ ] **Step 2: Build to fail.** Regenerate (new test + new engine sources) + build.
Expected: FAIL (`Arcane/Scene/EntityPick.hpp` not found).

- [ ] **Step 3: Write `EntityPick.hpp` + `.cpp` (engine, `ARCANE_API`).**

```cpp
// EntityPick.hpp
#pragma once

#include <Arcane/Base/Api.hpp>

#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Astra { class Registry; }

namespace Arcane
{
    // Collect every entity whose (WorldTransform, SpriteRenderer) oriented box
    // contains `worldPoint`, sorted front-most first by (sortingLayer, orderInLayer)
    // descending (stable among ties). front() is the top pick; the whole list drives
    // alt-click cycling. Pure read over the registry -- headless, deterministic.
    ARCANE_API std::vector<Astra::Entity> PickEntitiesAt(Astra::Registry& registry,
                                                         glm::vec2 worldPoint);
}
```

```cpp
// EntityPick.cpp
#include <Arcane/Scene/EntityPick.hpp>

#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane
{
    std::vector<Astra::Entity> PickEntitiesAt(Astra::Registry& registry, glm::vec2 worldPoint)
    {
        struct Hit { Astra::Entity e; int32_t layer; int32_t order; };
        std::vector<Hit> hits;

        auto view = registry.CreateView<WorldTransform, SpriteRenderer>();
        view.ForEach([&](Astra::Entity e, WorldTransform& xf, SpriteRenderer& sp)
        {
            // world -> sprite-local via the inverse 2D homogeneous world matrix.
            const glm::vec3 local = glm::inverse(xf.matrix) * glm::vec3(worldPoint, 1.0f);
            const glm::vec2 half = sp.size * 0.5f;
            if (std::abs(local.x) <= half.x && std::abs(local.y) <= half.y)
                hits.push_back({ e, sp.sortingLayer, sp.orderInLayer });
        });

        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b)
        {
            if (a.layer != b.layer) return a.layer > b.layer;   // higher layer = front-most
            return a.order > b.order;
        });

        std::vector<Astra::Entity> out;
        out.reserve(hits.size());
        for (const Hit& h : hits) out.push_back(h.e);
        return out;
    }
}
```

- [ ] **Step 4: Build + run to green.**

Run: build, then `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "[pick]"`
Expected: PASS (3 `[pick]` cases).

- [ ] **Step 5: Wire viewport click → pick → selection (with alt-cycle).** In `EditorPanels.hpp`, extend `ViewportPanelResult` with the click:

```cpp
    struct ViewportPanelResult
    {
        ViewportRect imageRect{};
        bool         hovered = false;
        bool         focused = false;
        uint32_t     desiredW = 0;
        uint32_t     desiredH = 0;
        bool         clicked = false;       // left-click landed inside the image this frame
        bool         altHeld  = false;      // alt modifier at click time (cycle stack)
        float        clickLocalX = 0.0f;    // viewport-local px of the click
        float        clickLocalY = 0.0f;
    };
```

In `DrawViewportPanel` (after the `ImGui::Image` call), capture a click inside the image:

```cpp
        if (r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 m = ImGui::GetMousePos();
            const float lx = m.x - origin.x, ly = m.y - origin.y;
            if (lx >= 0 && ly >= 0 && lx < (float)texW && ly < (float)texH)
            {
                r.clicked = true;
                r.altHeld = ImGui::GetIO().KeyAlt;
                r.clickLocalX = lx; r.clickLocalY = ly;
            }
        }
```

In `GrimoireApp::MainLoop`, after drawing the viewport panel, handle the click by unprojecting through the plugin camera and picking:

```cpp
        if (vp.clicked)
        {
            const glm::vec2 camOff = m_runtime->CameraOffset();
            const float     camZoom = m_runtime->CameraZoom();
            const glm::vec2 world =
                (glm::vec2(vp.clickLocalX, vp.clickLocalY) - camOff) / camZoom;
            std::vector<Astra::Entity> hits = Arcane::PickEntitiesAt(m_runtime->Registry(), world);
            if (hits.empty())
            {
                m_selection.Clear();
            }
            else if (vp.altHeld && !m_selection.pickCandidates.empty() &&
                     m_selection.pickCandidates == hits)
            {
                // alt-click on the same stack: cycle to the next entity under the cursor.
                m_selection.pickCycle = (m_selection.pickCycle + 1) % hits.size();
                m_selection.Select(hits[m_selection.pickCycle]);
            }
            else
            {
                m_selection.pickCandidates = hits;
                m_selection.pickCycle = 0;
                m_selection.Select(hits.front());
            }
        }
```

Add `#include <Arcane/Scene/EntityPick.hpp>` and `#include <glm/glm.hpp>` to `GrimoireApp.cpp`.

- [ ] **Step 6: Regenerate + build + regression.**

Run: `cd Arcane && GenerateProjects.bat` then build; `.\ArcaneTests.exe ~[gpu]` (expect 27624/304: +3 `[pick]` over Task 6).

- [ ] **Step 7: Desk-verify note + commit.** Desk: in a scene with sprite-bearing entities, clicking a sprite selects it (Inspector updates); alt-click cycles stacked sprites. **Known limitation (record, don't fix):** the stock Sandbox's physics entities carry no `SpriteRenderer`, so viewport click-pick selects nothing there — select those via the Hierarchy panel. Viewport pick targets sprite scenes; a physics-body→entity secondary pick is a future add.

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Arcane/src/Arcane/Scene/EntityPick.hpp Arcane/Arcane/src/Arcane/Scene/EntityPick.cpp \
        Arcane/Grimoire/src/GrimoireApp.cpp Arcane/Grimoire/src/EditorPanels.hpp Arcane/Grimoire/src/EditorPanels.cpp \
        Arcane/Tests/src/EntityPickTest.cpp
git commit -m "feat(arcane): sprite-OBB entity pick + grimoire viewport click-pick

Arcane::PickEntitiesAt collects entities whose (WorldTransform, SpriteRenderer) OBB
contains a world point, sorted front-most first by (sortingLayer, orderInLayer);
pure, headless-tested. Grimoire unprojects a viewport click through the plugin
camera, selects the top hit, and alt-click cycles the stack. Stock Sandbox entities
lack sprites (select via Hierarchy); GPU id-buffer pick deferred. Gate: +3 [pick]."
```

---

# Task 8: Play-in-editor (snapshot on Play, restore on Stop)

Add an `EditorMode` (Edit | Play). Play snapshots the registry via `Runtime::SnapshotRegistry` and unpauses the RunLoop; Stop restores via `RestoreRegistry` and re-pauses. Edit-mode edits are authored state and persist; play-time mutation is discarded on Stop.

**Files:**
- Create: `Arcane/Grimoire/src/PlayMode.hpp`, `Arcane/Grimoire/src/PlayMode.cpp`
- Modify: `Arcane/Grimoire/src/GrimoireApp.hpp`, `Arcane/Grimoire/src/GrimoireApp.cpp`, `Arcane/Grimoire/src/EditorPanels.cpp` (toolbar Play/Stop)
- Test: `Arcane/Tests/src/GrimoirePlayModeTest.cpp` + ArcaneTests `files{}` (source-compile `PlayMode.cpp`)

**Interfaces:**
- Consumes: `Runtime::SnapshotRegistry()` → `Astra::Result<std::vector<std::byte>, Astra::SerializationError>`; `Runtime::RestoreRegistry(std::span<const std::byte>)` → `bool`; `RunLoop::SetPaused`.
- Produces: `enum class Grimoire::EditorMode { Edit, Play }`; `Grimoire::PlaySession` with `bool Play(Arcane::Runtime&)` (snapshot + unpause, → Play), `bool Stop(Arcane::Runtime&)` (restore + pause, → Edit), `EditorMode Mode() const`, `bool IsPlaying() const`.

- [ ] **Step 1: Write the failing round-trip test.** Create `Arcane/Tests/src/GrimoirePlayModeTest.cpp`. Drive a real `Arcane::Runtime` with the shared TypeContext (mirror how `SandboxHudTest`/`RenderInterpolationTest` construct a Runtime/registry). Spawn an entity, snapshot via Play, mutate, Stop, assert the mutation is gone:

```cpp
// Grimoire play-in-editor: snapshot on Play, restore on Stop. CPU-only ([grimoire]).

#include <catch2/catch_test_macros.hpp>

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>

#include <PlayMode.hpp>

TEST_CASE("Play snapshots and Stop restores the authored registry", "[grimoire]")
{
    Astra::TypeContext ctx;                       // local (test TU owns the reflected thunks)
    Arcane::Runtime runtime(&ctx, /*audio*/false);
    // Register Scene components + spawn an authored entity with a known field value.
    // (Mirror the component registration RenderInterpolationTest uses.)
    Astra::Registry& reg = runtime.Registry();
    const Astra::Entity e = reg.CreateEntity();
    Arcane::SpriteRenderer sp; sp.sortingLayer = 3;
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

    Grimoire::PlaySession play;
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);

    REQUIRE(play.Play(runtime));                  // snapshot + unpause
    CHECK(play.IsPlaying());
    CHECK_FALSE(runtime.Loop().IsPaused());

    // Mutate during play: bump the field on the (possibly re-fetched) entity.
    {
        Astra::Registry& live = runtime.Registry();
        for (Astra::Entity le : live.GetEntityManager())
            if (Arcane::SpriteRenderer* s = live.GetComponent<Arcane::SpriteRenderer>(le))
                s->sortingLayer = 99;
    }

    REQUIRE(play.Stop(runtime));                  // restore + pause
    CHECK(play.Mode() == Grimoire::EditorMode::Edit);
    CHECK(runtime.Loop().IsPaused());

    // The play-time mutation is gone -- back to the authored value.
    Astra::Registry& restored = runtime.Registry();
    bool found = false;
    for (Astra::Entity le : restored.GetEntityManager())
        if (Arcane::SpriteRenderer* s = restored.GetComponent<Arcane::SpriteRenderer>(le))
        { CHECK(s->sortingLayer == 3); found = true; }
    CHECK(found);
}
```

**Note:** confirm `Registry::GetComponent<T>(Entity)` and `AddComponent<T>` names/signatures in the Astra headers; match `RenderInterpolationTest.cpp`. Confirm the component registration required for `SnapshotRegistry` to serialize `SpriteRenderer` (the reflected component must be registered on the shared `ComponentRegistry`) — reuse that test's registration path.

- [ ] **Step 2: Wire into ArcaneTests + build to fail.** Add `"%{wks.location}/Grimoire/src/PlayMode.cpp"` to ArcaneTests `files{}`. Regenerate + build.
Expected: FAIL (`PlayMode.hpp` not found).

- [ ] **Step 3: Write `PlayMode.hpp` + `.cpp`.**

```cpp
// PlayMode.hpp
#pragma once

namespace Arcane { class Runtime; }

namespace Grimoire
{
    enum class EditorMode { Edit, Play };

    // Play-in-editor state machine. Play() snapshots the registry and unpauses the
    // sim; Stop() restores the snapshot and re-pauses -- play-time mutation is
    // discarded, edit-mode edits (the snapshot content) are the authored state.
    class PlaySession
    {
    public:
        [[nodiscard]] EditorMode Mode() const noexcept { return m_mode; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_mode == EditorMode::Play; }

        bool Play(Arcane::Runtime& runtime);   // Edit -> Play (snapshot + unpause)
        bool Stop(Arcane::Runtime& runtime);   // Play -> Edit (restore + pause)

    private:
        EditorMode              m_mode = EditorMode::Edit;
        std::vector<std::byte>  m_snapshot;
    };
}
```

(Add `#include <cstddef>` + `#include <vector>`.)

```cpp
// PlayMode.cpp
#include "PlayMode.hpp"

#include <Arcane/Base/Runtime.hpp>

namespace Grimoire
{
    bool PlaySession::Play(Arcane::Runtime& runtime)
    {
        if (m_mode == EditorMode::Play) return true;
        auto snap = runtime.SnapshotRegistry();
        if (!snap) return false;                 // Result is falsy on a Save error
        m_snapshot = std::move(snap.value());    // confirm the Astra::Result value accessor name
        runtime.Loop().SetPaused(false);
        m_mode = EditorMode::Play;
        return true;
    }

    bool PlaySession::Stop(Arcane::Runtime& runtime)
    {
        if (m_mode == EditorMode::Edit) return true;
        const bool ok = runtime.RestoreRegistry(m_snapshot);
        runtime.Loop().SetPaused(true);
        m_mode = EditorMode::Edit;
        return ok;
    }
}
```

**Note:** confirm the `Astra::Result` API — the boolean-ish check and value accessor (extraction shows `Runtime::SnapshotRegistry()` returns `Astra::Result<std::vector<std::byte>, Astra::SerializationError>`; find whether it exposes `operator bool`/`.HasValue()`/`.IsOk()` and `.value()`/`.Value()` in `ThirdParty/Astra/include/Astra/Core/Result.hpp`, and use the real names).

- [ ] **Step 4: Build + run to green.**

Run: build, then `.\ArcaneTests.exe "[grimoire]"`
Expected: PASS (+1 play-mode case).

- [ ] **Step 5: Default to Edit mode + wire Play/Stop into the toolbar.** In `GrimoireApp.hpp` add `#include "PlayMode.hpp"` + member `Grimoire::PlaySession m_play;`. In `GrimoireApp::Init()`, after the plugin loads, start paused (Edit mode): `m_runtime->Loop().SetPaused(true);`. Change `DrawSimTimeToolbar` to take the play session and drive Play/Stop; in `EditorPanels.hpp` update the signature:

```cpp
    void DrawSimTimeToolbar(Arcane::RunLoop& loop, class PlaySession& play, Arcane::Runtime& runtime);
```

In `EditorPanels.cpp` (add `#include "PlayMode.hpp"`, `#include <Arcane/Base/Runtime.hpp>`):

```cpp
    void DrawSimTimeToolbar(Arcane::RunLoop& loop, PlaySession& play, Arcane::Runtime& runtime)
    {
        ImGui::Begin("Sim");
        if (play.IsPlaying())
        {
            if (ImGui::Button("Stop")) play.Stop(runtime);
        }
        else
        {
            if (ImGui::Button("Play")) play.Play(runtime);
        }
        ImGui::SameLine();
        if (ImGui::Button(loop.IsPaused() ? "Resume" : "Pause")) loop.SetPaused(!loop.IsPaused());
        ImGui::SameLine();
        if (ImGui::Button("Step")) loop.RequestSingleStep();
        ImGui::SameLine();
        float scale = static_cast<float>(loop.TimeScale());
        if (ImGui::SliderFloat("time-scale", &scale, 0.0f, 2.0f, "%.2f"))
            loop.SetTimeScale(scale);
        ImGui::End();
    }
```

Update the `MainLoop()` call: `Grimoire::DrawSimTimeToolbar(m_runtime->Loop(), m_play, *m_runtime);`.

- [ ] **Step 6: Regenerate + build + regression.**

Run: `cd Arcane && GenerateProjects.bat` then build; `.\ArcaneTests.exe ~[gpu]` (expect 27625/305: +1 over Task 7).

- [ ] **Step 7: Desk-verify + commit.** Desk: Grimoire starts in Edit (sim paused). Play runs the sim; move/spawn during play; Stop returns the scene to the authored state (play-time changes gone). Inspector edits while stopped persist across a Play/Stop cycle.

```bash
cd /d/dev/starworks/Gacha
git add Arcane/Grimoire/src/PlayMode.hpp Arcane/Grimoire/src/PlayMode.cpp \
        Arcane/Grimoire/src/GrimoireApp.hpp Arcane/Grimoire/src/GrimoireApp.cpp \
        Arcane/Grimoire/src/EditorPanels.hpp Arcane/Grimoire/src/EditorPanels.cpp \
        Arcane/Tests/src/GrimoirePlayModeTest.cpp Arcane/premake5.lua
git commit -m "feat(grimoire): play-in-editor (snapshot on Play, restore on Stop)

EditorMode Edit|Play. Play snapshots the registry via Runtime::SnapshotRegistry and
unpauses; Stop restores via RestoreRegistry and re-pauses, discarding play-time
mutation. Edit-mode edits are the authored state and persist. Grimoire boots in
Edit (paused). Round-trip unit-tested (snapshot->mutate->stop->authored value).
Gate: ~[gpu] +1 [grimoire]. Completes Epic 04 (M7 Grimoire shell)."
```

---

## Self-Review

**Spec coverage** (spec §1–§8 → tasks):
- §Modularity principle → Task 1 lift (engine API) + Global Constraints; every editor need routed through `ARCANE_API`/existing Astra API. ✔
- §1 plugin-host lift → Task 1. ✔
- §2 host shell (dockspace, sim-time toolbar, console) → Task 2 (scaffold/spine/`--frames`) + Task 3 (dockspace/toolbar/console). ✔
- §3 scene-in-a-panel viewport (OffscreenCanvas, resize, input gating, unprojection) → Task 4. ✔
- §4 hierarchy + selection → Task 5. ✔ (Spec's "engine-side enumeration surface" satisfied by existing `Registry::GetEntityManager()` — no redundant wrapper added, per YAGNI; noted in Task 5.)
- §5 reflection inspector (supported editors + read-only fallback) → Task 6. ✔
- §6 pick (CPU-OBB collect-and-sort, alt-cycle, `ARCANE_API`; GPU id-buffer deferred) → Task 7. ✔
- §7 play-in-editor (snapshot on Play, restore on Stop, Edit-persist) → Task 8. ✔
- §Testing (headless CPU bulk + `[gpu]` smoke + desk-verify) → each task's gate + `--frames` smoke in Tasks 2/3/4. ✔
- §Scope-out (gizmos, multi-select, disk scene format, undo/redo, asset browser, GPU id-buffer, Problems tab) → not implemented; called out in Task 7 (id-buffer) and left absent. ✔

**Placeholder scan:** No TBD/TODO. The three `**Note:**` blocks (Astra `TypeHash`/`Result` accessor names, `InputSnapshot` field names, `Registry::AddComponent`/`GetComponent` signatures) are **verification instructions with a named fallback**, not placeholders — each names the exact header to confirm against and the behavior to preserve, because the Astra API names could not be quoted verbatim from this session's extraction. Every code step ships real code.

**Type consistency:** `PickEntitiesAt(Astra::Registry&, glm::vec2)` (Task 7) matches its Task-7 call in `GrimoireApp`. `SelectionContext` fields (`selected`/`pickCandidates`/`pickCycle`) defined in Task 5 are used consistently in Tasks 6/7. `ViewportPanelResult` is defined in Task 4 and extended (not renamed) in Task 7. `ConsoleBuffer`/`EditorMode`/`PlaySession`/`FieldKind` names are stable across their defining and consuming steps. `DrawSimTimeToolbar` signature change in Task 8 updates both the declaration and the `MainLoop` call. `LoomConfig` is the reused host config throughout (Tasks 2–8).

**Test-count arithmetic:** baseline 27613/294 → +1 (T3) +2 (T4) +2 (T5) +2 (T6) +3 (T7 `[pick]`) +1 (T8) = 27624 assertions? Cases: 294 → 295, 297, 299, 301, 304, 305. (Assertion totals are indicative — the `[pick]`/`[grimoire]` cases carry multiple `CHECK`s; the gate is "baseline `~[gpu]` unchanged + the new cases pass", not an exact assertion count.)
