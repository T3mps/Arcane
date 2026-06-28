# Arcane Architecture Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the approved Arcane architecture-hardening design into enforceable module boundaries, non-breaking Runtime facades, and explicit physics/asset ownership notes without destabilizing the active physics branch.

**Architecture:** Start with durable documentation plus a source-scanning test that keeps `Arcane/Core` presentation-free. Then add grouped `Runtime` facade value objects that forward to the existing root methods, preserving all current callers. Finish by documenting the physics ownership model and deferring deep `PhysicsWorld` extraction until the current physics behavior work is green.

**Tech Stack:** C++23, MSVC/Visual Studio 2026, premake5, Catch2, Arcane Core, Arcane.dll Runtime/Scene/Render modules.

**Spec:** `docs/superpowers/specs/2026-06-28-arcane-architecture-hardening-design.md`

## Global Constraints

- Work from repo root: `D:\dev\starworks\Gacha`.
- Preserve unrelated dirty work. As of planning, the branch already has local Arcane physics/sandbox edits plus untracked files; do not stage or commit them.
- Core remains presentation-free: no SDL3, NVRHI, ImGui, platform windows, render code, or audio includes in `Arcane/Core`.
- Keep the transition non-breaking. Existing `Runtime` methods remain public and continue to forward/behave exactly as before.
- No physics solver behavior changes in this architecture phase.
- No public plugin ABI break. Existing plugins should continue to build.
- ASCII comments only. No `/fp:fast`. Keep `/MD` Arcane workspace behavior unchanged.
- Commands are PowerShell.
- Regenerate only if a new `.cpp`/`.hpp` file is added and the generated project does not pick it up automatically:
  ```powershell
  Push-Location Arcane
  & "..\ThirdParty\premake5\premake5.exe" vs2026
  Pop-Location
  ```
- Build:
  ```powershell
  msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
  ```
- Run tests from the test exe directory:
  ```powershell
  Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
  .\ArcaneTests.exe "[architecture],[runtime],[hotreload]"
  Pop-Location
  ```
- If `[physics]` is still red from the active sleep/stiffness branch, record that separately; do not hide it inside this work.

---

## File Structure

- Create: `docs/architecture/arcane-module-boundaries.md` - durable boundary contract for Core, Arcane.dll, Loom, plugins, tests, and explicit exceptions.
- Create: `docs/architecture/arcane-asset-render-ownership.md` - future asset/render ownership requirements for Grimoire-era content.
- Create: `Arcane/Tests/src/CoreDependencyBoundaryTest.cpp` - Catch2 source scan that fails when Core includes presentation-layer dependencies.
- Create: `Arcane/Arcane/src/Arcane/Base/RuntimeFacades.hpp` - small non-owning grouped facade value objects.
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.hpp` - include facade header and expose grouped accessors.
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.cpp` - implement facade accessors and forwarding methods.
- Modify: `Arcane/Tests/src/RuntimeTest.cpp` - verify grouped facades forward to existing root methods.
- Create: `Arcane/Tests/src/PhysicsOwnershipBoundaryTest.cpp` - compile-time guard that `DrawPhysicsDebug` consumes `const PhysicsWorld&`.
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` - tighten the ownership comments around `PhysicsResource`.
- Modify: `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp` - state that debug rendering is a const-inspection boundary, not world ownership.

---

## Task 1: Module Boundary Doc and Core Dependency Guard

**Files:**
- Create: `docs/architecture/arcane-module-boundaries.md`
- Create: `Arcane/Tests/src/CoreDependencyBoundaryTest.cpp`

**Interfaces:**
- Produces: durable boundary rules consumed by reviews and later task docs.
- Produces: Catch2 tag `[architecture][core]`.

- [ ] **Step 1: Create the boundary contract**

Create `docs/architecture/arcane-module-boundaries.md`:

```markdown
# Arcane Module Boundaries

Date: 2026-06-28
Status: Active architecture contract
Source spec: `docs/superpowers/specs/2026-06-28-arcane-architecture-hardening-design.md`

## Purpose

Arcane is intentionally split across a presentation-free Core static library, one
engine DLL, a thin host, and hot-reloadable gameplay plugins. This document
defines which module owns which kind of state and which dependencies may cross a
boundary.

## Modules

### Arcane/Core

`Arcane/Core` owns presentation-free algorithms, data structures, simulation
code, networking primitives, serialization helpers, and shared utility code.

Allowed dependency families:

- C++ standard library
- Header-only math/data dependencies already allowed by the workspace, such as
  glm, nlohmann/json, picosha2, and spdlog
- Sibling Core headers under `<Arcane/...>`

Forbidden dependency families:

- SDL or platform window/event headers
- NVRHI, D3D12, Vulkan presentation/device ownership, shader-library, or render
  resource headers
- ImGui
- Audio backends
- Arcane.dll-only module headers such as `<Arcane/Render/...>`,
  `<Arcane/Platform/...>`, `<Arcane/ImGui/...>`, and `<Arcane/Audio/...>`

Core may be linked into multiple production modules only for stateless/value
code, per-module state that never crosses ownership boundaries, or explicitly
documented stateful services that are safe as one copy per module.

### Arcane.dll

`Arcane.dll` owns platform integration, render resources, engine runtime
services, exported plugin-facing APIs, first-party ImGui integration, and
engine-owned scene/runtime state.

Arcane.dll may include Core. Core must not include Arcane.dll-only modules.

### Loom.exe

`Loom.exe` is a thin host. It owns command-line parsing, backend selection, boot
order, plugin path selection, and host diagnostics. Engine behavior belongs in
Arcane.dll or Core, not in Loom.

### Gameplay Plugins

Gameplay plugins own gameplay systems and plugin-local state. If a plugin
directly instantiates a Core type, the plugin owns that instance unless an
Arcane.dll facade explicitly says otherwise.

Plugin-facing engine access should go through `Arcane::Runtime` and its grouped
facades.

### Tests

Tests may duplicate Core and fixture code intentionally. Test module layout is
not precedent for production ownership.

## Cross-DLL Ownership Rule

If an object crosses a DLL boundary by pointer or reference and has non-trivial
ownership, one module must clearly own and destroy it. Other modules consume
handles, IDs, snapshots, facades, or const views.

## Physics Rule

A plugin-created `Arcane::Physics::PhysicsWorld` is plugin-owned. Arcane.dll may
inspect it through const views or snapshot/draw-command data. Arcane.dll-created
scene physics is engine-owned through `PhysicsResource` and scene systems.

## Exceptions

Exceptions must be listed here with:

- The file/path.
- The dependency or ownership rule being violated.
- Why it is temporary.
- The follow-up that removes or narrows it.

Current exceptions: none.
```

- [ ] **Step 2: Add the failing Core dependency guard test**

Create `Arcane/Tests/src/CoreDependencyBoundaryTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::filesystem::path FindRepoRoot()
    {
        std::filesystem::path p = std::filesystem::current_path();
        for (int i = 0; i < 12; ++i)
        {
            if (std::filesystem::exists(p / "Arcane" / "Core" / "src"))
                return p;
            if (!p.has_parent_path())
                break;
            p = p.parent_path();
        }
        throw std::runtime_error("could not locate repo root from ArcaneTests cwd");
    }

    bool IsSourceFile(const std::filesystem::path& p)
    {
        const std::string e = p.extension().string();
        return e == ".hpp" || e == ".h" || e == ".cpp" || e == ".ipp";
    }

    std::string TrimLeft(std::string s)
    {
        const auto it = std::find_if_not(s.begin(), s.end(), [](unsigned char c) {
            return c == ' ' || c == '\t';
        });
        s.erase(s.begin(), it);
        return s;
    }

    bool IncludeLineHasForbiddenDependency(std::string_view line)
    {
        static constexpr std::string_view kForbidden[] = {
            "<SDL", "\"SDL",
            "<nvrhi", "\"nvrhi",
            "<imgui", "\"imgui",
            "<Arcane/Render/",
            "<Arcane/Platform/",
            "<Arcane/ImGui/",
            "<Arcane/Audio/"
        };

        for (const std::string_view token : kForbidden)
        {
            if (line.find(token) != std::string_view::npos)
                return true;
        }
        return false;
    }
}

TEST_CASE("Arcane Core stays presentation-free", "[architecture][core]")
{
    const std::filesystem::path repo = FindRepoRoot();
    const std::filesystem::path core = repo / "Arcane" / "Core" / "src";
    REQUIRE(std::filesystem::exists(core));

    std::vector<std::string> violations;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(core))
    {
        if (!entry.is_regular_file() || !IsSourceFile(entry.path()))
            continue;

        std::ifstream in(entry.path());
        REQUIRE(in.good());

        std::string line;
        int lineNo = 0;
        while (std::getline(in, line))
        {
            ++lineNo;
            const std::string trimmed = TrimLeft(line);
            if (!trimmed.starts_with("#include"))
                continue;

            if (IncludeLineHasForbiddenDependency(trimmed))
            {
                std::ostringstream msg;
                msg << std::filesystem::relative(entry.path(), repo).string()
                    << ":" << lineNo << ": " << trimmed;
                violations.push_back(msg.str());
            }
        }
    }

    INFO("Core forbidden include violations:\n" << [&] {
        std::ostringstream out;
        for (const std::string& v : violations)
            out << v << '\n';
        return out.str();
    }());
    CHECK(violations.empty());
}
```

- [ ] **Step 3: Build and run the architecture guard**

Run:

```powershell
msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[architecture][core]"
Pop-Location
```

Expected: PASS. If the new `.cpp` is not picked up by the generated project, run premake regeneration, rebuild, and rerun.

- [ ] **Step 4: Commit Task 1**

```powershell
git add docs/architecture/arcane-module-boundaries.md Arcane/Tests/src/CoreDependencyBoundaryTest.cpp
git commit -m "test(arcane): guard core presentation boundary"
```

---

## Task 2: Non-Breaking Runtime Facades

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Base/RuntimeFacades.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.cpp`
- Modify: `Arcane/Tests/src/RuntimeTest.cpp`

**Interfaces:**
- Produces: `Runtime::Scene() -> RuntimeScene`
- Produces: `Runtime::Jobs() -> RuntimeJobs`
- Produces: `Runtime::InputState() -> RuntimeInput`
- Produces: `Runtime::Render() -> RuntimeRender`
- Produces: `Runtime::PluginServices() -> RuntimePluginServices`
- Preserves: all existing `Runtime` methods.

- [ ] **Step 1: Write the failing Runtime facade test**

Append to `Arcane/Tests/src/RuntimeTest.cpp`:

```cpp
TEST_CASE("Runtime grouped facades forward to existing root capabilities", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());

    CHECK(&rt.Scene().Registry() == &rt.Registry());
    CHECK(&rt.Scene().Schedulers() == &rt.Schedulers());
    CHECK(&rt.Scene().Loop() == &rt.Loop());
    CHECK(rt.Scene().TypeContext() == rt.TypeContext());
    CHECK(rt.Scene().Components() == rt.Components());

    CHECK(rt.Jobs().WorkScheduler() == rt.WorkScheduler());
    CHECK(rt.Jobs().TaskExecutor() == rt.TaskExecutor());

    Arcane::InputSnapshot snap;
    snap.mouseX = 123.0f;
    snap.mouseY = 45.0f;
    snap.wantCaptureMouse = true;
    rt.InputState().SetSnapshot(snap);
    CHECK(rt.InputState().Snapshot().mouseX == 123.0f);
    CHECK(rt.Input().mouseY == 45.0f);
    CHECK(rt.Input().wantCaptureMouse);

    rt.Render().SetCamera(glm::vec2(10.0f, 20.0f), 2.5f);
    CHECK(rt.Render().CameraOffset().x == 10.0f);
    CHECK(rt.Render().CameraOffset().y == 20.0f);
    CHECK(rt.Render().CameraZoom() == 2.5f);

    rt.Render().SetResources(nullptr, nullptr);
    CHECK(rt.Render().Device() == nullptr);
    CHECK(rt.Render().Shaders() == nullptr);

    rt.PluginServices().ClearSystems();
    CHECK(rt.Schedulers().fixedUpdate.Empty());
    CHECK(rt.Schedulers().update.Empty());
    CHECK(rt.Schedulers().render.Empty());
}
```

- [ ] **Step 2: Build to verify it fails**

Run:

```powershell
msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
```

Expected: compile failure because `Runtime::Scene`, `Jobs`, `InputState`, `Render`, and `PluginServices` do not exist.

- [ ] **Step 3: Add `RuntimeFacades.hpp`**

Create `Arcane/Arcane/src/Arcane/Base/RuntimeFacades.hpp`:

```cpp
#pragma once

// Non-owning grouped Runtime capability facades. Runtime remains the root plugin
// handle; these value objects organize the surface without breaking old callers.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <glm/vec2.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace nvrhi { class IDevice; }
namespace Astra
{
    class ComponentRegistry;
    class IWorkScheduler;
    class Registry;
    class TypeContext;
}

namespace Arcane
{
    class Batcher2D;
    class Runtime;
    class ShaderLibrary;
    class RunLoop;
    class SystemSchedulers;
    struct ITaskExecutor;

    class ARCANE_API RuntimeScene
    {
    public:
        explicit RuntimeScene(Runtime& runtime) noexcept;
        Astra::Registry& Registry() const noexcept;
        SystemSchedulers& Schedulers() const noexcept;
        RunLoop& Loop() const noexcept;
        Astra::TypeContext* TypeContext() const noexcept;
        std::shared_ptr<Astra::ComponentRegistry> Components() const noexcept;
    private:
        Runtime* m_runtime = nullptr;
    };

    class ARCANE_API RuntimeJobs
    {
    public:
        explicit RuntimeJobs(Runtime& runtime) noexcept;
        Astra::IWorkScheduler* WorkScheduler() const noexcept;
        ITaskExecutor* TaskExecutor() const noexcept;
    private:
        Runtime* m_runtime = nullptr;
    };

    class ARCANE_API RuntimeInput
    {
    public:
        explicit RuntimeInput(Runtime& runtime) noexcept;
        void SetSnapshot(const InputSnapshot& snap) const noexcept;
        const InputSnapshot& Snapshot() const noexcept;
    private:
        Runtime* m_runtime = nullptr;
    };

    class ARCANE_API RuntimeRender
    {
    public:
        explicit RuntimeRender(Runtime& runtime) noexcept;
        void SetContext(Batcher2D* batcher) const;
        void SetResources(nvrhi::IDevice* device, ShaderLibrary* shaders) const noexcept;
        nvrhi::IDevice* Device() const noexcept;
        ShaderLibrary* Shaders() const noexcept;
        void SetCamera(glm::vec2 offset, float zoom) const noexcept;
        glm::vec2 CameraOffset() const noexcept;
        float CameraZoom() const noexcept;
    private:
        Runtime* m_runtime = nullptr;
    };

    class ARCANE_API RuntimePluginServices
    {
    public:
        explicit RuntimePluginServices(Runtime& runtime) noexcept;
        void SetImGui(void* context, void* alloc, void* freeFn, void* userData) const noexcept;
        void* ImGuiContext() const noexcept;
        void* ImGuiAlloc() const noexcept;
        void* ImGuiFree() const noexcept;
        void* ImGuiUserData() const noexcept;
        std::vector<std::byte> SnapshotRegistry() const;
        bool RestoreRegistry(std::span<const std::byte> bytes) const;
        void ResetRegistry() const;
        void ClearSystems() const;
    private:
        Runtime* m_runtime = nullptr;
    };
}
```

- [ ] **Step 4: Add grouped accessors to `Runtime.hpp`**

In `Arcane/Arcane/src/Arcane/Base/Runtime.hpp`, add:

```cpp
#include <Arcane/Base/RuntimeFacades.hpp>
```

Then add these public methods near the top of the `Runtime` public section, after copy/delete declarations:

```cpp
        // Grouped capability facades. These are cheap non-owning value objects
        // over this Runtime; existing root methods remain for ABI compatibility.
        RuntimeScene Scene() noexcept;
        RuntimeJobs Jobs() noexcept;
        RuntimeInput InputState() noexcept;
        RuntimeRender Render() noexcept;
        RuntimePluginServices PluginServices() noexcept;
```

- [ ] **Step 5: Implement accessors and forwarding methods in `Runtime.cpp`**

Add after `Runtime::~Runtime()`:

```cpp
    RuntimeScene Runtime::Scene() noexcept { return RuntimeScene(*this); }
    RuntimeJobs Runtime::Jobs() noexcept { return RuntimeJobs(*this); }
    RuntimeInput Runtime::InputState() noexcept { return RuntimeInput(*this); }
    RuntimeRender Runtime::Render() noexcept { return RuntimeRender(*this); }
    RuntimePluginServices Runtime::PluginServices() noexcept { return RuntimePluginServices(*this); }

    RuntimeScene::RuntimeScene(Runtime& runtime) noexcept : m_runtime(&runtime) {}
    Astra::Registry& RuntimeScene::Registry() const noexcept { return m_runtime->Registry(); }
    SystemSchedulers& RuntimeScene::Schedulers() const noexcept { return m_runtime->Schedulers(); }
    RunLoop& RuntimeScene::Loop() const noexcept { return m_runtime->Loop(); }
    Astra::TypeContext* RuntimeScene::TypeContext() const noexcept { return m_runtime->TypeContext(); }
    std::shared_ptr<Astra::ComponentRegistry> RuntimeScene::Components() const noexcept
    {
        return m_runtime->Components();
    }

    RuntimeJobs::RuntimeJobs(Runtime& runtime) noexcept : m_runtime(&runtime) {}
    Astra::IWorkScheduler* RuntimeJobs::WorkScheduler() const noexcept { return m_runtime->WorkScheduler(); }
    ITaskExecutor* RuntimeJobs::TaskExecutor() const noexcept { return m_runtime->TaskExecutor(); }

    RuntimeInput::RuntimeInput(Runtime& runtime) noexcept : m_runtime(&runtime) {}
    void RuntimeInput::SetSnapshot(const InputSnapshot& snap) const noexcept { m_runtime->SetInputSnapshot(snap); }
    const InputSnapshot& RuntimeInput::Snapshot() const noexcept { return m_runtime->Input(); }

    RuntimeRender::RuntimeRender(Runtime& runtime) noexcept : m_runtime(&runtime) {}
    void RuntimeRender::SetContext(Batcher2D* batcher) const { m_runtime->SetRenderContext(batcher); }
    void RuntimeRender::SetResources(nvrhi::IDevice* device, ShaderLibrary* shaders) const noexcept
    {
        m_runtime->SetRenderResources(device, shaders);
    }
    nvrhi::IDevice* RuntimeRender::Device() const noexcept { return m_runtime->Device(); }
    ShaderLibrary* RuntimeRender::Shaders() const noexcept { return m_runtime->Shaders(); }
    void RuntimeRender::SetCamera(glm::vec2 offset, float zoom) const noexcept { m_runtime->SetCamera(offset, zoom); }
    glm::vec2 RuntimeRender::CameraOffset() const noexcept { return m_runtime->CameraOffset(); }
    float RuntimeRender::CameraZoom() const noexcept { return m_runtime->CameraZoom(); }

    RuntimePluginServices::RuntimePluginServices(Runtime& runtime) noexcept : m_runtime(&runtime) {}
    void RuntimePluginServices::SetImGui(void* context, void* alloc, void* freeFn, void* userData) const noexcept
    {
        m_runtime->SetImGui(context, alloc, freeFn, userData);
    }
    void* RuntimePluginServices::ImGuiContext() const noexcept { return m_runtime->ImGuiContext(); }
    void* RuntimePluginServices::ImGuiAlloc() const noexcept { return m_runtime->ImGuiAlloc(); }
    void* RuntimePluginServices::ImGuiFree() const noexcept { return m_runtime->ImGuiFree(); }
    void* RuntimePluginServices::ImGuiUserData() const noexcept { return m_runtime->ImGuiUserData(); }
    std::vector<std::byte> RuntimePluginServices::SnapshotRegistry() const { return m_runtime->SnapshotRegistry(); }
    bool RuntimePluginServices::RestoreRegistry(std::span<const std::byte> bytes) const
    {
        return m_runtime->RestoreRegistry(bytes);
    }
    void RuntimePluginServices::ResetRegistry() const { m_runtime->ResetRegistry(); }
    void RuntimePluginServices::ClearSystems() const { m_runtime->ClearSystems(); }
```

- [ ] **Step 6: Build and run Runtime/hotreload tests**

Run:

```powershell
msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[runtime],[hotreload]"
Pop-Location
```

Expected: PASS.

- [ ] **Step 7: Commit Task 2**

```powershell
git add Arcane/Arcane/src/Arcane/Base/RuntimeFacades.hpp Arcane/Arcane/src/Arcane/Base/Runtime.hpp Arcane/Arcane/src/Arcane/Base/Runtime.cpp Arcane/Tests/src/RuntimeTest.cpp
git commit -m "feat(arcane): add grouped runtime facades"
```

---

## Task 3: Physics Ownership Boundary Notes and Test

**Files:**
- Create: `Arcane/Tests/src/PhysicsOwnershipBoundaryTest.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp`

**Interfaces:**
- Produces: Catch2 tag `[architecture][physics]`.
- Preserves: `DrawPhysicsDebug(const PhysicsWorld&, Batcher2D&, ...)`.

- [ ] **Step 1: Add the ownership boundary test**

Create `Arcane/Tests/src/PhysicsOwnershipBoundaryTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>

#include <type_traits>

TEST_CASE("Physics debug rendering consumes a const world view", "[architecture][physics]")
{
    using Expected = void (*)(const Arcane::Physics::PhysicsWorld&,
                             Arcane::Batcher2D&,
                             const Arcane::PhysicsDebugDrawOptions&);
    static_assert(std::is_same_v<decltype(&Arcane::DrawPhysicsDebug), Expected>);
    SUCCEED("DrawPhysicsDebug does not own or mutate PhysicsWorld");
}
```

- [ ] **Step 2: Tighten `PhysicsResource` ownership comments**

In `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp`, replace the `PhysicsResource` block comment with:

```cpp
    // -------------------------------------------------------------------------
    // PhysicsResource (M6 P3.3)
    // -------------------------------------------------------------------------
    // Transient Registry resource: owns an engine-created PhysicsWorld and the
    // entity<->handle map maintained by PhysicsSystem.
    //
    // Ownership boundary:
    // - When Arcane.dll creates the world through this resource, Arcane.dll owns
    //   and destroys it.
    // - When a gameplay plugin creates its own PhysicsWorld, the plugin owns it.
    //   Arcane.dll may inspect that plugin-owned world only through const views,
    //   snapshots, or draw-command data supplied by the owner.
    // - A PhysicsWorld pointer/reference must not imply cross-DLL destruction
    //   rights. The creating module destroys the world.
    //
    // Not reflected; not serialized (Registry::Save excludes resources --
    // transient runtime state only).
```

Keep the existing `WHY unique_ptr<PhysicsWorld>` paragraph immediately after this block.

- [ ] **Step 3: Tighten `PhysicsDebugDraw` boundary comments**

In `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp`, replace the `PRESENTATION BOUNDARY` paragraph with:

```cpp
// PRESENTATION + OWNERSHIP BOUNDARY: this file lives in Arcane.dll (Render/).
// It includes PhysicsWorld.hpp (Core) and Batcher2D.hpp. Core never includes
// Render -- the boundary is one-way. DrawPhysicsDebug consumes a const
// PhysicsWorld reference and submits draw primitives; it never owns, mutates, or
// destroys the world. For plugin-created worlds, the plugin remains the owner.
// No Astra / SDL3 / NVRHI headers in the options struct itself; only
// Batcher2D.hpp is included here.
```

- [ ] **Step 4: Build and run architecture physics guard**

Run:

```powershell
msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[architecture][physics]"
Pop-Location
```

Expected: PASS.

- [ ] **Step 5: Commit Task 3**

```powershell
git add Arcane/Tests/src/PhysicsOwnershipBoundaryTest.cpp Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp
git commit -m "docs(arcane): clarify physics ownership boundary"
```

---

## Task 4: Asset/Render Ownership Note

**Files:**
- Create: `docs/architecture/arcane-asset-render-ownership.md`

**Interfaces:**
- Produces: durable design note for future Grimoire/asset pipeline work.

- [ ] **Step 1: Create the asset/render ownership note**

Create `docs/architecture/arcane-asset-render-ownership.md`:

```markdown
# Arcane Asset and Render Ownership

Date: 2026-06-28
Status: Future-facing architecture note
Source spec: `docs/superpowers/specs/2026-06-28-arcane-architecture-hardening-design.md`

## Purpose

The current renderer and asset facade are sufficient for Sandbox-era content,
but Grimoire and authored game content need stable asset identities and explicit
lifetime rules. This note records the requirement before the editor/content
milestones begin.

## Current Direction

- Runtime code should move toward asset handles or IDs rather than ad hoc raw
  render-resource ownership.
- Asset cache lifetime, hot reload, and failure memoization should be expressed
  through an Arcane facade.
- Plugins may request render resources through Runtime/asset facades, but the
  creating module must remain clear.
- Render resources created by the host or Arcane.dll are not destroyed by
  plugins unless the API explicitly transfers ownership.

## Future Design Must Answer

1. How authored content IDs map to runtime asset handles.
2. How texture/shader/font hot reload interacts with plugin hot reload.
3. How failed asset loads are memoized and invalidated.
4. Which module owns GPU resource creation and destruction.
5. How Grimoire previews share runtime asset loading behavior without becoming a
   parallel asset system.

## Out of Scope for Architecture Hardening

This phase does not build the final asset graph, bindless material system,
renderer rewrite, or Grimoire integration. It only records the ownership
constraint so later plans start from the right boundary.
```

- [ ] **Step 2: Commit Task 4**

```powershell
git add docs/architecture/arcane-asset-render-ownership.md
git commit -m "docs(arcane): record asset render ownership direction"
```

---

## Task 5: Final Verification and Handoff

**Files:**
- Read only unless verification uncovers a defect in Tasks 1-4.

**Interfaces:**
- Produces: final recorded status.

- [ ] **Step 1: Run focused architecture/runtime/hotreload tests**

Run:

```powershell
msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[architecture],[runtime],[hotreload]"
Pop-Location
```

Expected: PASS.

- [ ] **Step 2: Run broader non-GPU sanity if the active physics branch is green**

Only if the active physics branch is not already known-red, run:

```powershell
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "~[gpu]"
Pop-Location
```

Expected: PASS. If `[physics]` is still red from the sleep/stiffness work, record the exact failing tests and keep this architecture phase focused.

- [ ] **Step 3: Confirm staged/committed files**

Run:

```powershell
git status --short
```

Expected: only pre-existing unrelated dirty files remain. The architecture task files should be committed.

- [ ] **Step 4: Final handoff summary**

Report:

- Commits created.
- Tests run and pass/fail status.
- Any pre-existing unrelated dirty files left untouched.
- Whether `[physics]` remains red for the existing branch work.
