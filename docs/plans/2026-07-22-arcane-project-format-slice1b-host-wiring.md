# Arcane Project Format — Slice 1b Host/Editor Wiring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Loom runtime host and the Arcane Editor open a real `.arcproj` project — loading its `gameModule` through the existing ABI-versioned plugin host and resolving content through the project's mounts — instead of today's `--plugin <path>` + `data/`-next-to-exe.

**Architecture:** `Runtime` (in `Arcane.dll`) gains a `std::optional<Project>` with a validate-then-commit `OpenProject` that also sets the `Assets` content-root. Both hosts share tiny boot helpers (`HostBoot::LoadInputConfig` / `GameModule`) that turn an open project into the two boot decisions (which input config, which game module). The editor adds File→Open Project as an in-session soft-restart (unload plugin → `OpenProject` → reload plugin) behind an SDL folder-dialog seam on `Window`. A committed `SampleProject/` (gameModule = `Sandbox.dll`) is the thing to open. Everything is additive: no `--project` ⇒ today's behavior.

**Tech Stack:** C++23, NVRHI/SDL3 (via `Arcane.dll`), enkiTS, Catch2 v3, premake5 (`Arcane.slnx`), MSBuild (VS18).

## Global Constraints

- **/MD everywhere** (dynamic CRT); **UTF-8 without BOM**, **ASCII-only comments**; no `/fp:fast`.
- **Engine ABI constant:** `Arcane::kGamePluginABIVersion` = **5** (`Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp`). Any hand-authored manifest in this plan uses `"engine":{"abi":5}`; if the ABI is ever bumped, the `SampleProject.arcproj` manifest **and** the two `[loom]` manifest fixtures in Task 3 must bump with it.
- **Build:** VS18 MSBuild on `Arcane\Arcane.slnx` (e.g. `msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m`). **Any new `.cpp` file OR any `premake5.lua` change ⇒ run `Arcane\GenerateProjects.bat` FIRST** (regenerates the `.slnx`/vcxproj). New header-only `.hpp` files that are `#include`d by an already-compiled TU do **not** need a regen.
- **Run tests from the test exe's own directory:** `cd bin\Debug-windows-x86_64-md\ArcaneTests` then `ArcaneTests.exe "<tag>"` (it loads `Arcane.dll` + fixtures from there).
- **Baseline gates to hold:** `[project]` 76/18 (grows with new cases); full `~[gpu]` 27951/366 (no regression).
- **Non-breaking:** with no `--project`, both hosts must behave exactly as today (host `Sandbox.dll` + load `data/input_actions.json`).
- **Desk-verify, not headless, for windowed runs:** windowed D3D12 runs can SIGSEGV headless under Parsec/virtual-display on this machine — the `[gpu]`/host-exe verifications in Tasks 5–7 are run by the human at the desk (optionally `--backend vulkan`), not scripted in CI here.

---

### Task 1: `Assets::SetContentRoot`

Add a content-root to the `Assets` facade so relative loose-file loads resolve under a base directory (the project's `Content/`); absolute paths pass through. Pure CPU, headless-TDD.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Assets/Assets.hpp` (add the pure-virtual)
- Modify: `Arcane/Arcane/src/Arcane/Assets/Assets.cpp` (implement)
- Test: `Arcane/Tests/src/AssetsTest.cpp` (add a `[assets]` case)

**Interfaces:**
- Produces: `void Arcane::Assets::SetContentRoot(const std::filesystem::path& root)` — base dir for relative `GetTexture/GetBytes/GetJson`; empty (default) = legacy exe-relative.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/AssetsTest.cpp` (it already has `#include <fstream>`; if not, add it near the top):

```cpp
TEST_CASE("Assets content-root anchors relative loads", "[assets]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_contentroot";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "probe.json", std::ios::binary) << R"({"ok":true})";

    auto assets = Arcane::Assets::Create(nullptr);
    assets->SetContentRoot(root);

    auto doc = assets->GetJson("probe.json");   // relative -> resolves under root
    REQUIRE(doc != nullptr);
    REQUIRE((*doc)["ok"].get<bool>() == true);

    auto abs = assets->GetJson(root / "probe.json");   // absolute bypasses the root
    REQUIRE(abs != nullptr);

    fs::remove_all(root, ec);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[assets]"`
Expected: FAILS to compile — `Assets` has no member `SetContentRoot`.

- [ ] **Step 3: Add the pure-virtual to the interface**

In `Arcane/Arcane/src/Arcane/Assets/Assets.hpp`, immediately after the `SetDevice(...)` declaration (the `= 0;` line), add:

```cpp
        // Base directory prepended to RELATIVE paths in GetTexture/GetBytes/GetJson;
        // absolute paths pass through unchanged. The host sets this from the open
        // project's game:// mount (project Content/) so loose-file loads resolve under
        // the project instead of exe-relative. Empty (default) == the legacy
        // exe-relative behavior. Slice 2 (AssetRegistry/GUID) resolves behind the
        // AssetId seam and this becomes the fallback for legacy path loads.
        virtual void SetContentRoot(const std::filesystem::path& root) = 0;
```

- [ ] **Step 4: Implement in `Assets.cpp`**

In `Arcane/Arcane/src/Arcane/Assets/Assets.cpp`:

(a) Rename the free anonymous-namespace helper `ResolveAssetPath` to `ExeRelative` (rename the function name only; body unchanged):

```cpp
        // Resolve a path exe-relative when relative -- mirrors ShaderLibrary. Relative
        // paths anchor to the executable directory so tests pass regardless of CWD.
        std::filesystem::path ExeRelative(const std::filesystem::path& path)
        {
            if (path.is_absolute())
                return path;
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                return std::filesystem::path(modulePath).parent_path() / path;
#endif
            return path;
        }
```

(b) Inside `class AssetsImpl`, add the override + a content-root-aware member resolver + the member field. Add the override right after `SetDevice(...)`:

```cpp
            void SetContentRoot(const std::filesystem::path& root) override
            {
                m_contentRoot = root;
            }
```

Add this private member function (e.g. just above the `m_device` member declarations):

```cpp
            // Content-root-aware resolve, shadowing the free ExeRelative helper for
            // the three Get* methods: absolute paths pass through; a set content root
            // anchors relatives under it; otherwise the legacy exe-relative anchor.
            std::filesystem::path ResolveAssetPath(const std::filesystem::path& path) const
            {
                if (path.is_absolute())
                    return path;
                if (!m_contentRoot.empty())
                    return m_contentRoot / path;
                return ExeRelative(path);
            }
```

Add the field next to `m_device`:

```cpp
            std::filesystem::path m_contentRoot;   // empty => exe-relative (legacy)
```

The three `Get*` methods already call `ResolveAssetPath(path)`; that unqualified name now binds to the new member (class scope wins over the enclosing namespace), so no call-site edits are needed.

- [ ] **Step 5: Run test to verify it passes**

Run: `cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[assets]"`
Expected: PASS (all `[assets]` cases, including the new one).

- [ ] **Step 6: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Assets/Assets.hpp Arcane/Arcane/src/Arcane/Assets/Assets.cpp Arcane/Tests/src/AssetsTest.cpp
git commit -m "feat(arcane): Assets::SetContentRoot -- anchor relative loads (project format S1b)"
```

---

### Task 2: `Runtime::OpenProject` + `CurrentProject` (+ private `Project` ctor)

Runtime becomes the project owner: validate-then-commit open, ABI gate, sets the Assets content-root. Headless-TDD.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.hpp` (forward-decl + 2 methods + `#include <filesystem>`)
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.cpp` (Impl member + method bodies + includes)
- Modify: `Arcane/Arcane/src/Arcane/Project/Project.hpp` (default ctor → private)
- Create: `Arcane/Tests/src/RuntimeProjectTest.cpp`

**Interfaces:**
- Consumes: `Arcane::Assets::SetContentRoot` (Task 1); `Arcane::Project::Open` / `Project::Manifest()` / `Project::Root()`; `Arcane::kGamePluginABIVersion`.
- Produces: `bool Arcane::Runtime::OpenProject(const std::filesystem::path&)` — true = adopted + content-root set; false = state untouched. `const Arcane::Project* Arcane::Runtime::CurrentProject() const noexcept` — nullptr when none.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/RuntimeProjectTest.cpp`:

```cpp
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Assets/Assets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const char* tag)
    {
        std::error_code ec;
        fs::path d = fs::temp_directory_path() / (std::string("arcane_proj_") + tag);
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    void WriteFile(const fs::path& p, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary) << text;
    }
}

TEST_CASE("Runtime::OpenProject adopts a valid project", "[project]")
{
    const fs::path dir = MakeTempDir("valid");
    REQUIRE(Arcane::Project::Create(dir / "Game", "MyGame").has_value());  // abi == this engine

    Arcane::Runtime rt;
    REQUIRE(rt.CurrentProject() == nullptr);
    REQUIRE(rt.OpenProject(dir / "Game") == true);
    REQUIRE(rt.CurrentProject() != nullptr);
    REQUIRE(rt.CurrentProject()->Manifest().name == "MyGame");

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject refuses a mismatched engine ABI", "[project]")
{
    const fs::path dir = MakeTempDir("badabi");
    WriteFile(dir / "Bad.arcproj",
        R"({"formatVersion":1,"name":"Bad","engine":{"abi":9999},)"
        R"("gameModule":"","plugins":[],"bootScene":""})");
    std::error_code ec; fs::create_directories(dir / "Content", ec);

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir) == false);
    REQUIRE(rt.CurrentProject() == nullptr);   // state untouched

    fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject sets the Assets content root", "[project]")
{
    const fs::path dir = MakeTempDir("content");
    REQUIRE(Arcane::Project::Create(dir / "Game", "G").has_value());
    WriteFile(dir / "Game" / "Content" / "probe.json", R"({"ok":true})");

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir / "Game") == true);
    auto doc = rt.AssetsFacade().GetJson("probe.json");   // relative -> under Content/
    REQUIRE(doc != nullptr);
    REQUIRE((*doc)["ok"].get<bool>() == true);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("Runtime::OpenProject switches projects on re-open", "[project]")
{
    const fs::path dir = MakeTempDir("switch");
    REQUIRE(Arcane::Project::Create(dir / "A", "Alpha").has_value());
    REQUIRE(Arcane::Project::Create(dir / "B", "Beta").has_value());

    Arcane::Runtime rt;
    REQUIRE(rt.OpenProject(dir / "A") == true);
    REQUIRE(rt.CurrentProject()->Manifest().name == "Alpha");
    REQUIRE(rt.OpenProject(dir / "B") == true);
    REQUIRE(rt.CurrentProject()->Manifest().name == "Beta");

    std::error_code ec; fs::remove_all(dir, ec);
}
```

- [ ] **Step 2: Regenerate + run test to verify it fails**

Run: `cd Arcane && GenerateProjects.bat` (new `.cpp` file), then build + run:
`msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m`
Expected: FAILS to compile — `Runtime` has no `OpenProject` / `CurrentProject`.

- [ ] **Step 3: Declare the API in `Runtime.hpp`**

In `Arcane/Arcane/src/Arcane/Base/Runtime.hpp`: add `#include <filesystem>` with the other includes. In the forward-declaration block (the `class Assets; struct ITaskExecutor; ...` lines), add:

```cpp
    class Project;
```

In the `public:` section (after the `Assets& AssetsFacade() noexcept;` accessor is a good home), add:

```cpp
        // --- project (Slice 1b) ---
        // Open a project folder or .arcproj: validate-then-commit. On success the
        // Project is adopted and the Assets facade's content root is set to the
        // project's game:// mount (root/Content); returns false and leaves ALL state
        // untouched on a missing/invalid manifest OR an engineAbi that does not match
        // this engine (kGamePluginABIVersion). Both hosts open a project through here.
        bool OpenProject(const std::filesystem::path& pathOrFile);

        // The open project, or nullptr when none is open (no-project fallback mode).
        const Project* CurrentProject() const noexcept;
```

- [ ] **Step 4: Implement in `Runtime.cpp`**

Add includes near the top of `Arcane/Arcane/src/Arcane/Base/Runtime.cpp`:

```cpp
#include <Arcane/Project/Project.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion

#include <optional>
```

In `struct Runtime::Impl`, add a member (next to `assets`):

```cpp
        std::optional<Project>                      project;   // open project (Slice 1b); empty = none
```

Add the two method bodies inside `namespace Arcane` (e.g. after the audio methods, before the closing brace of the file):

```cpp
    bool Runtime::OpenProject(const std::filesystem::path& pathOrFile)
    {
        auto proj = Project::Open(pathOrFile);
        if (!proj)
            return false;   // Project::Open already logged the cause

        // Engine/ABI binding: refuse a project built against a different engine ABI.
        // Belt-and-suspenders over PluginHost's own DLL-ABI gate -- this catches a
        // stale manifest before we even try to load the game module.
        if (proj->Manifest().engineAbi != static_cast<int>(kGamePluginABIVersion))
        {
            ARC_ERROR("Runtime::OpenProject: project '{}' targets engine ABI {} but this "
                      "engine is ABI {}", proj->Manifest().name,
                      proj->Manifest().engineAbi, static_cast<int>(kGamePluginABIVersion));
            return false;   // leave state untouched
        }

        m_impl->project = std::move(*proj);
        // Route loose-file content loads under the project's game:// mount (Content/).
        m_impl->assets->SetContentRoot(m_impl->project->Root() / "Content");
        return true;
    }

    const Project* Runtime::CurrentProject() const noexcept
    {
        return m_impl->project ? &*m_impl->project : nullptr;
    }
```

- [ ] **Step 5: Make `Project`'s default ctor private**

In `Arcane/Arcane/src/Arcane/Project/Project.hpp`, in the existing `private:` section (just above `std::filesystem::path m_root;`), add:

```cpp
        // Open()/Create() are the only construction paths (they are static members and
        // can reach this). std::optional<Project> in Runtime move-constructs, never
        // default-constructs, so a private default ctor is safe.
        Project() = default;
```

- [ ] **Step 6: Build + run to verify it passes**

Run: `cd Arcane && msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m`
then `cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[project]"`
Expected: PASS — the 4 new cases plus the existing `[project]` suite (count grows past 76/18).

- [ ] **Step 7: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Base/Runtime.hpp Arcane/Arcane/src/Arcane/Base/Runtime.cpp Arcane/Arcane/src/Arcane/Project/Project.hpp Arcane/Tests/src/RuntimeProjectTest.cpp
git commit -m "feat(arcane): Runtime::OpenProject/CurrentProject -- project owner + ABI gate (project format S1b)"
```

---

### Task 3: `LoomConfig --project` + shared `HostBoot` helpers

Add the `--project` CLI option and the two host-boot helpers both hosts use. Headless-TDD.

**Files:**
- Modify: `Arcane/Loom/src/LoomConfig.hpp` (add field)
- Modify: `Arcane/Loom/src/LoomConfig.cpp` (parse option)
- Create: `Arcane/Loom/src/ProjectBoot.hpp` (header-only helpers)
- Create: `Arcane/Tests/src/HostBootTest.cpp`

**Interfaces:**
- Consumes: `Arcane::Project::Open` / `Manifest().gameModule` / `ResolveAsset`; `Arcane::AssetId::FromMountPath`; `Arcane::InputActions::LoadFile` / `SetBaseContext`.
- Produces: `LoomConfig::projectPath` (`std::string`, default `""`); `bool Arcane::HostBoot::LoadInputConfig(Arcane::InputActions&, const Arcane::Project*)`; `std::string Arcane::HostBoot::GameModule(const Arcane::Project*, const std::string& fallback)`.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/HostBootTest.cpp`:

```cpp
#include <LoomConfig.hpp>
#include <ProjectBoot.hpp>

#include <Arcane/Project/Project.hpp>
#include <Arcane/Input/InputActions.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace { namespace fs = std::filesystem; }

TEST_CASE("LoomConfig parses --project", "[loom]")
{
    const char* argv[] = { "loom", "--project", "MyGame" };
    auto out = LoomConfig::Parse(3, const_cast<char**>(argv));
    REQUIRE(out.config.has_value());
    REQUIRE(out.config->projectPath == "MyGame");
}

TEST_CASE("LoomConfig defaults --project to empty", "[loom]")
{
    const char* argv[] = { "loom" };
    auto out = LoomConfig::Parse(1, const_cast<char**>(argv));
    REQUIRE(out.config.has_value());
    REQUIRE(out.config->projectPath.empty());
}

TEST_CASE("HostBoot::GameModule falls back with no/empty gameModule", "[loom]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm";
    std::error_code ec; fs::remove_all(dir, ec);
    REQUIRE(Arcane::Project::Create(dir, "G").has_value());   // Create writes gameModule ""
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Sandbox.dll") == "Sandbox.dll");
    REQUIRE(Arcane::HostBoot::GameModule(nullptr, "Sandbox.dll") == "Sandbox.dll");
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::GameModule returns the manifest gameModule when set", "[loom]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_gm2";
    std::error_code ec; fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);
    std::ofstream(dir / "P.arcproj", std::ios::binary) <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":5},)"
        R"("gameModule":"Foo.dll","plugins":[],"bootScene":""})";
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    REQUIRE(Arcane::HostBoot::GameModule(&*proj, "Sandbox.dll") == "Foo.dll");
    fs::remove_all(dir, ec);
}

TEST_CASE("HostBoot::LoadInputConfig loads through the project mount", "[loom]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_hostboot_input";
    std::error_code ec; fs::remove_all(dir, ec);
    REQUIRE(Arcane::Project::Create(dir, "G").has_value());
    std::ofstream(dir / "Content" / "input_actions.json", std::ios::binary) <<
        R"({"actionMaps":[{"name":"demo","actions":[{"name":"quit","type":"Button",)"
        R"("bindings":[{"path":"<Keyboard>/escape"}]}]}]})";
    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    auto input = Arcane::InputActions::Create();
    REQUIRE(Arcane::HostBoot::LoadInputConfig(*input, &*proj) == true);
    REQUIRE(input->ActiveContext() == "demo");
    fs::remove_all(dir, ec);
}
```

- [ ] **Step 2: Regenerate + run test to verify it fails**

Run: `cd Arcane && GenerateProjects.bat` (new `.cpp`), then build ArcaneTests.
Expected: FAILS to compile — no `projectPath` member and no `ProjectBoot.hpp`.

- [ ] **Step 3: Add `projectPath` to `LoomConfig`**

In `Arcane/Loom/src/LoomConfig.hpp`, add after the `pluginPath` member:

```cpp
    std::string             projectPath = "";   // .arcproj or project folder; "" = data/-next-to-exe
```

- [ ] **Step 4: Parse `--project` in `LoomConfig.cpp`**

In `Arcane/Loom/src/LoomConfig.cpp`, add the option (after the `plugin` option line):

```cpp
    cli.Option("project", "", "project folder or .arcproj to open (empty = data/-next-to-exe)");
```

and the mapping (after `cfg.pluginPath = r.Get("plugin");`):

```cpp
    cfg.projectPath = r.Get("project");
```

- [ ] **Step 5: Create the shared `ProjectBoot.hpp`**

Create `Arcane/Loom/src/ProjectBoot.hpp`:

```cpp
#pragma once

// Host-boot helpers shared by Loom and the Arcane Editor (source-shared, like
// GpuContext/LoomConfig). They translate an open Project into the two boot
// decisions a host makes: which input config to load, and which game module to
// host. No project => the legacy data/-next-to-exe + --plugin behavior.

#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>

#include <filesystem>
#include <string>

namespace Arcane::HostBoot
{
    // Load the input-actions config: through the open project's game:// mount when a
    // project is open, else data/input_actions.json (exe-relative, the legacy path).
    // Sets the "demo" base context on success. Returns false if the file is
    // missing/unreadable/malformed (the host logs and continues -- input stays inert).
    // The project and no-project paths are mutually exclusive: an open project's input
    // comes ONLY from its game:// mount -- never a silent fall-through to the legacy
    // data/ file (see the Task 3 review fix).
    inline bool LoadInputConfig(Arcane::InputActions& input, const Arcane::Project* project)
    {
        std::filesystem::path file;
        if (project)
        {
            // A project is open: its input config comes from the project's game://
            // mount, never the legacy exe-relative data/. If it does not resolve
            // (today unreachable -- Project::Open always registers game://; reachable
            // once Slice 2's AssetRegistry lookup can legitimately miss), fail loudly
            // rather than silently loading whatever data/input_actions.json sits by the
            // exe.
            auto resolved = project->ResolveAsset(
                Arcane::AssetId::FromMountPath("game://input_actions.json"));
            if (!resolved)
                return false;
            file = *resolved;
        }
        else
        {
            file = "data/input_actions.json";   // no project: legacy exe-relative fallback
        }
        if (!input.LoadFile(file))
            return false;
        input.SetBaseContext("demo");
        return true;
    }

    // The game module to host: the project's gameModule when a project is open and it
    // names one, else the fallback --plugin path.
    inline std::string GameModule(const Arcane::Project* project, const std::string& fallback)
    {
        if (project && !project->Manifest().gameModule.empty())
            return project->Manifest().gameModule;
        return fallback;
    }
}
```

- [ ] **Step 6: Build + run to verify it passes**

Run: `cd Arcane && msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m`
then `cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[loom]"`
Expected: PASS — the 5 new cases plus the existing `[loom]` suite.

- [ ] **Step 7: Commit**

```bash
git add Arcane/Loom/src/LoomConfig.hpp Arcane/Loom/src/LoomConfig.cpp Arcane/Loom/src/ProjectBoot.hpp Arcane/Tests/src/HostBootTest.cpp
git commit -m "feat(arcane): LoomConfig --project + HostBoot input/gameModule helpers (project format S1b)"
```

---

### Task 4: `SampleProject/` demo artifact + premake copies

Commit a demo project (gameModule = `Sandbox.dll`) and copy it next to the Loom + Editor exes. Data + build-verify.

**Files:**
- Create: `Arcane/SampleProject/SampleProject.arcproj`
- Create: `Arcane/SampleProject/Content/input_actions.json`
- Create: `Arcane/SampleProject/.gitignore`
- Create: `Arcane/SampleProject/Source/.gitkeep`, `Arcane/SampleProject/Config/.gitkeep`, `Arcane/SampleProject/Plugins/.gitkeep`
- Modify: `Arcane/premake5.lua` (Loom + ArcaneEditor postbuild copy)

**Interfaces:**
- Produces: `<Loom-exe-dir>/SampleProject/` and `<ArcaneEditor-exe-dir>/SampleProject/` after build; opened via `--project SampleProject` from the exe dir (Tasks 5–7).

- [ ] **Step 1: Write the manifest**

Create `Arcane/SampleProject/SampleProject.arcproj`:

```json
{
  "formatVersion": 1,
  "name": "SampleProject",
  "description": "Slice 1b demo: proves Project::Open host wiring (gameModule = Sandbox.dll).",
  "engine": { "abi": 5 },
  "gameModule": "Sandbox.dll",
  "plugins": [],
  "bootScene": ""
}
```

- [ ] **Step 2: Write the project's input config (copy the existing file verbatim)**

The project's input config is a byte-for-byte copy of the existing `demo` map. Create the folder and copy:

```bash
mkdir -p Arcane/SampleProject/Content
cp Arcane/Loom/data/input_actions.json Arcane/SampleProject/Content/input_actions.json
```

(Do not hand-retype it — it must stay identical to `Arcane/Loom/data/input_actions.json`, which is still the no-project fallback source.)

- [ ] **Step 3: Write the project `.gitignore` + skeleton keepers**

Create `Arcane/SampleProject/.gitignore`:

```gitignore
# Derived / generated -- never commit
Binaries/
Intermediate/
Saved/
```

Create three empty files so the skeleton dirs are tracked: `Arcane/SampleProject/Source/.gitkeep`, `Arcane/SampleProject/Config/.gitkeep`, `Arcane/SampleProject/Plugins/.gitkeep` (empty content).

- [ ] **Step 4: Add the Loom postbuild copy**

In `Arcane/premake5.lua`, in the **Loom** project's `postbuildcommands` block (the one ending with the `data/input_actions.json` copy near line 402), add this line after the input_actions copy:

```lua
        '{COPYDIR} "%{wks.location}/SampleProject" "%{cfg.buildtarget.directory}/SampleProject"',
```

- [ ] **Step 5: Add the ArcaneEditor postbuild copy**

In the **ArcaneEditor** project's `postbuildcommands` block (the one with the font copies near line 456), add the same line after the `data/input_actions.json` copy:

```lua
        '{COPYDIR} "%{wks.location}/SampleProject" "%{cfg.buildtarget.directory}/SampleProject"',
```

- [ ] **Step 6: Regenerate, build, verify the copy landed**

Run: `cd Arcane && GenerateProjects.bat` (premake changed), then:
`msbuild Arcane.slnx /t:Loom /p:Configuration=Debug /m` and
`msbuild Arcane.slnx /t:ArcaneEditor /p:Configuration=Debug /m`
Then verify both destinations exist:
`dir bin\Debug-windows-x86_64-md\Loom\SampleProject\SampleProject.arcproj`
`dir bin\Debug-windows-x86_64-md\ArcaneEditor\SampleProject\Content\input_actions.json`
Expected: both paths exist.

- [ ] **Step 7: Commit**

```bash
git add Arcane/SampleProject Arcane/premake5.lua
git commit -m "feat(arcane): SampleProject demo + postbuild copy next to Loom/Editor (project format S1b)"
```

---

### Task 5: Wire the Loom runtime host to open a project

Move input loading out of `GpuContext`; open the project and load its game module in `Loom::Init`. Desk-verified (windowed).

**Files:**
- Modify: `Arcane/Loom/src/GpuContext.cpp` (stop loading the input file)
- Modify: `Arcane/Loom/src/Loom.cpp` (open project, load input, resolve game module)

**Interfaces:**
- Consumes: `Runtime::OpenProject`/`CurrentProject` (Task 2); `HostBoot::LoadInputConfig`/`GameModule` (Task 3); `GpuContext::Input()`.

- [ ] **Step 1: Stop loading the input file in `GpuContext::Create`**

In `Arcane/Loom/src/GpuContext.cpp`, replace the input-actions load block (the `m_input->LoadFile("data/input_actions.json")` guard and the `SetBaseContext("demo")` line) so only creation remains:

```cpp
    ctx->m_input = Arcane::InputActions::Create();
    if (!ctx->m_input) { ARC_ERROR("GpuContext: input actions create failed"); return nullptr; }
    // The input-actions CONFIG is loaded by the host AFTER it opens a project, so the
    // config can resolve through the project's game:// mount (or data/ when no project).
    // GpuContext only creates the empty action system here. See HostBoot::LoadInputConfig.
```

- [ ] **Step 2: Open the project + load input + resolve the game module in `Loom::Init`**

In `Arcane/Loom/src/Loom.cpp`, add includes near the top:

```cpp
#include <ProjectBoot.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Project/Project.hpp>
```

After the `m_runtime->SetRenderResources(...)` call and before the ImGui-handoff block, insert:

```cpp
    // Open the project (if any) BEFORE loading input + the game module: both come from
    // the project when one is given. No --project => CurrentProject() stays null and the
    // legacy data/ + --plugin path is used (non-breaking).
    if (!m_config.projectPath.empty())
    {
        if (!m_runtime->OpenProject(m_config.projectPath))
            ARC_WARN("Loom: --project '{}' failed to open; using data/ + --plugin fallback",
                     m_config.projectPath);
    }
    if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->CurrentProject()))
        ARC_WARN("Loom: input actions failed to load");
```

Then replace the plugin-load block (`m_plugin.emplace(*m_runtime, std::filesystem::path(m_config.pluginPath));` and its `Load()` guard) with:

```cpp
    const std::string gameModule =
        Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
    m_plugin.emplace(*m_runtime, std::filesystem::path(gameModule));
    if (!m_plugin->Load())
    {
        ARC_ERROR("Loom: failed to load game module '{}'", gameModule);
        return false;
    }
```

- [ ] **Step 3: Build**

Run: `cd Arcane && msbuild Arcane.slnx /t:Loom /p:Configuration=Debug /m`
Expected: builds clean.

- [ ] **Step 4: Desk-verify — no project (unchanged behavior)** *(human, at the desk)*

Run:
```
cd bin\Debug-windows-x86_64-md\Loom
Loom.exe --frames 120
```
Expected: window opens, Sandbox physics scene renders, ESC/Tab/WASD/F5 input works, exits 0. Console shows no project-open line. Identical to pre-1b behavior. *(Optionally add `--backend vulkan`.)*

- [ ] **Step 5: Desk-verify — with the project** *(human, at the desk)*

Run (from the same dir):
```
Loom.exe --project SampleProject --frames 120
```
Expected: same scene renders, input works (loaded via `game://input_actions.json`), exits 0. No "failed to open" warning in the console.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Loom/src/GpuContext.cpp Arcane/Loom/src/Loom.cpp
git commit -m "feat(arcane): Loom opens --project (game module + input via mount) (project format S1b)"
```

---

### Task 6: Wire the Arcane Editor to open a project (startup)

Mirror Loom's boot in `EditorApp::Init`, plus the window title and the Assets panel project readout. Desk-verified.

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (Init: open project, load input, game module, title)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (`DrawAssetsPanel` signature + fwd-decl)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`DrawAssetsPanel` shows the project)

**Interfaces:**
- Consumes: `Runtime::OpenProject`/`CurrentProject`; `HostBoot::LoadInputConfig`/`GameModule`; `Project::Manifest()`/`Root()`.
- Produces: `void Arcane::Editor::DrawAssetsPanel(const Arcane::Project* project)`; `std::string Arcane::Editor::EditorTitle(const Arcane::Project*)` (file-local helper in EditorApp.cpp).

- [ ] **Step 1: Add includes + a title helper in `EditorApp.cpp`**

In `Arcane/ArcaneEditor/src/EditorApp.cpp`, add includes:

```cpp
#include <ProjectBoot.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Project/Project.hpp>
```

In the file's anonymous namespace (the one holding the scancode constants), add:

```cpp
        std::string EditorTitle(const Arcane::Project* project)
        {
            if (project)
                return "Arcane Editor -- " + project->Manifest().name;
            return "Arcane Editor";
        }
```

- [ ] **Step 2: Open the project + load input + resolve the game module in `Init`**

In `EditorApp::Init`, after `m_runtime->SetRenderResources(...)` and before the `m_gameImgui = ...` block, insert the same boot block as Loom:

```cpp
    // Open the project (if any) BEFORE loading input + the game module (mirrors Loom).
    if (!m_config.projectPath.empty())
    {
        if (!m_runtime->OpenProject(m_config.projectPath))
            ARC_WARN("Arcane Editor: --project '{}' failed to open; using data/ + --plugin fallback",
                     m_config.projectPath);
    }
    if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->CurrentProject()))
        ARC_WARN("Arcane Editor: input actions failed to load");
    m_gpu->Win().SetTitle(EditorTitle(m_runtime->CurrentProject()));
```

Then replace the plugin-load block (`m_plugin.emplace(*m_runtime, std::filesystem::path(m_config.pluginPath));` and its `Load()` guard) with:

```cpp
    const std::string gameModule =
        Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
    m_plugin.emplace(*m_runtime, std::filesystem::path(gameModule));
    if (!m_plugin->Load())
    {
        ARC_ERROR("Arcane Editor: failed to load game module '{}'", gameModule);
        return false;
    }
```

(The existing early `m_gpu->Win().SetTitle("Arcane Editor");` near the top of `Init` can stay — it is superseded by the `EditorTitle(...)` call above once the project is known.)

- [ ] **Step 3: Update `DrawAssetsPanel` to show the project**

In `Arcane/ArcaneEditor/src/EditorPanels.hpp`, add a forward declaration inside `namespace Arcane { ... }` (or above the `Arcane::Editor` namespace):

```cpp
namespace Arcane { class Project; }
```

and change the `DrawAssetsPanel` declaration to:

```cpp
    void DrawAssetsPanel(const Arcane::Project* project);
```

In `Arcane/ArcaneEditor/src/EditorPanels.cpp`, add `#include <Arcane/Project/Project.hpp>` and replace `DrawAssetsPanel`:

```cpp
    void DrawAssetsPanel(const Arcane::Project* project)
    {
        ImGui::Begin("Assets");
        if (project)
        {
            ImGui::Text("Project: %s", project->Manifest().name.c_str());
            ImGui::TextDisabled("%s", project->Root().generic_string().c_str());
        }
        else
        {
            ImGui::TextDisabled("No project open (data/-next-to-exe)");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Assets browser -- coming soon");
        ImGui::End();
    }
```

- [ ] **Step 4: Pass the project at the call site**

In `EditorApp::MainLoop`, change the `Arcane::Editor::DrawAssetsPanel();` call to:

```cpp
            Arcane::Editor::DrawAssetsPanel(m_runtime->CurrentProject());
```

- [ ] **Step 5: Build**

Run: `cd Arcane && msbuild Arcane.slnx /t:ArcaneEditor /p:Configuration=Debug /m`
Expected: builds clean.

- [ ] **Step 6: Desk-verify** *(human, at the desk)*

Run:
```
cd bin\Debug-windows-x86_64-md\ArcaneEditor
ArcaneEditor.exe --project SampleProject
```
Expected: title reads **"Arcane Editor -- SampleProject"**; the Sandbox scene renders in the Viewport; the **Assets** panel shows `Project: SampleProject` + the project root path; camera pan/zoom (RMB/wheel) works (input loaded via `game://`). Then run `ArcaneEditor.exe` with no args: title reads **"Arcane Editor"**, Assets panel shows **"No project open"**, everything else unchanged. Close the window to exit.

- [ ] **Step 7: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorApp.cpp Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(arcane): Arcane Editor opens --project at startup + Assets panel readout (project format S1b)"
```

---

### Task 7: Editor File → Open Project (soft-restart)

Add an SDL folder-dialog seam on `Window`, a File→Open Project menu item, and an in-session soft-restart. Desk-verified (interactive).

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Platform/Window.hpp` (dialog seam declaration)
- Modify: `Arcane/Arcane/src/Arcane/Platform/Window.cpp` (SDL folder dialog)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (`BeginDockSpace` signature)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (menu item)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (members + methods)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (dialog launch + `SwitchProject`)

**Interfaces:**
- Consumes: `Runtime::OpenProject`/`CurrentProject`; `HostBoot::LoadInputConfig`/`GameModule`; `Project::Open`; `PlaySession::Stop`; `CommandStack::Clear`; `SelectionContext::Clear`; `PluginHost::reset`/`emplace`/`Load`/`Vtable`; `EditorTitle` (Task 6).
- Produces: `void Arcane::Window::ShowOpenFolderDialog(FolderPickedCallback, void*)`; `void Arcane::Editor::BeginDockSpace(CommandStack&, bool& openProjectRequested)`.

- [ ] **Step 1: Declare the dialog seam on `Window`**

In `Arcane/Arcane/src/Arcane/Platform/Window.hpp`, add inside the `public:` section (after `SdlWindow()`):

```cpp
        // Native folder-picker (editor "Open Project"). Async: SDL surfaces the result
        // during a later PumpEvents by invoking `cb` on the main thread with the chosen
        // folder path, or nullptr on cancel/error. `cb`/SDL types stay out of this
        // header (same opaque pattern as NativeEventTap). Uses THIS window's SDL
        // instance -- the only one that owns the video subsystem.
        using FolderPickedCallback = void (*)(const char* path, void* user);
        void ShowOpenFolderDialog(FolderPickedCallback cb, void* user) const;
```

- [ ] **Step 2: Implement the dialog in `Window.cpp`**

In `Arcane/Arcane/src/Arcane/Platform/Window.cpp`, add near the SDL includes:

```cpp
#include <SDL3/SDL_dialog.h>

#include <memory>
```

Add an anonymous-namespace trampoline (near the top of the file's `namespace Arcane` or in a file-local anonymous namespace):

```cpp
    namespace
    {
        struct FolderCbCtx { Arcane::Window::FolderPickedCallback cb; void* user; };

        // SDL hands us the full result list; the editor wants only the first folder
        // (allow_many=false), or nullptr on cancel/error. Invoked exactly once -> we
        // own and free the heap ctx here.
        void SDLCALL FolderDialogTrampoline(void* userdata, const char* const* filelist, int /*filter*/)
        {
            std::unique_ptr<FolderCbCtx> ctx(static_cast<FolderCbCtx*>(userdata));
            const char* picked = (filelist && filelist[0]) ? filelist[0] : nullptr;
            if (ctx->cb) ctx->cb(picked, ctx->user);
        }
    }
```

Add the method body:

```cpp
    void Window::ShowOpenFolderDialog(FolderPickedCallback cb, void* user) const
    {
        auto* ctx = new FolderCbCtx{ cb, user };   // freed by the trampoline
        SDL_ShowOpenFolderDialog(&FolderDialogTrampoline, ctx, m_window, nullptr, false);
    }
```

- [ ] **Step 3: Add the menu item + out-flag to `BeginDockSpace`**

In `Arcane/ArcaneEditor/src/EditorPanels.hpp`, change the `BeginDockSpace` declaration to:

```cpp
    void BeginDockSpace(Arcane::CommandStack& undo, bool& openProjectRequested);
```

In `Arcane/ArcaneEditor/src/EditorPanels.cpp`, change the signature to match and add the menu item as the first `File` entry:

```cpp
    void BeginDockSpace(Arcane::CommandStack& undo, bool& openProjectRequested)
    {
```

and inside `if (ImGui::BeginMenu("File"))`, before `ImGui::MenuItem("New Scene");`:

```cpp
                if (ImGui::MenuItem("Open Project...")) openProjectRequested = true;
                ImGui::Separator();
```

- [ ] **Step 4: Add editor members + method declarations**

In `Arcane/ArcaneEditor/src/EditorApp.hpp`, add to the `private:` members:

```cpp
        // File -> Open Project (soft-restart). The menu sets m_openProjectRequested;
        // MainLoop launches the async folder dialog; SDL invokes FolderPickedThunk (main
        // thread) during a later PumpEvents, which stashes the chosen path in
        // m_pendingProjectPath; the next frame's top runs SwitchProject and clears it.
        bool        m_openProjectRequested = false;
        std::string m_pendingProjectPath;

        static void FolderPickedThunk(const char* path, void* user);   // -> m_pendingProjectPath
        void        SwitchProject(const std::filesystem::path& path);  // validate-then-soft-restart
```

Add `#include <string>` and `#include <filesystem>` to `EditorApp.hpp` if not already present (via existing includes).

- [ ] **Step 5: Implement the thunk + `SwitchProject` in `EditorApp.cpp`**

Add both methods to `EditorApp.cpp`:

```cpp
    void EditorApp::FolderPickedThunk(const char* path, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (path) self->m_pendingProjectPath = path;
    }

    void EditorApp::SwitchProject(const std::filesystem::path& path)
    {
        // Validate FIRST -- never tear down a live session for a bad project.
        if (!Arcane::Project::Open(path))
        {
            ARC_ERROR("Open Project: '{}' is not a valid Arcane project", path.generic_string());
            return;
        }

        // Return to Edit + clear editor state that references the outgoing scene.
        if (m_play.IsPlaying())
            m_play.Stop(*m_runtime, m_plugin ? m_plugin->Vtable() : nullptr);
        m_selection.Clear();
        if (m_undo) m_undo->Clear();

        // Idle the GPU before freeing plugin-owned GPU resources, then unload the plugin
        // (dtor: Unload -> ClearSystems + ResetRegistry, DLL still mapped).
        m_gpu->Device().Nvrhi()->waitForIdle();
        m_plugin.reset();

        // Commit the new project (sets Assets content-root) + reload input via its mount.
        if (!m_runtime->OpenProject(path))
        {
            ARC_ERROR("Open Project: OpenProject('{}') failed after validation", path.generic_string());
            return;   // editor left with no plugin; user can Open another project
        }
        if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->CurrentProject()))
            ARC_WARN("Open Project: input actions failed to load");

        // Load the new game module through the same ABI-versioned plugin host.
        const std::string gameModule =
            Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
        m_plugin.emplace(*m_runtime, std::filesystem::path(gameModule));
        if (!m_plugin->Load())
            ARC_ERROR("Open Project: failed to load game module '{}'", gameModule);

        m_runtime->Loop().SetPaused(true);   // back to Edit
        m_gpu->Win().SetTitle(EditorTitle(m_runtime->CurrentProject()));
    }
```

- [ ] **Step 6: Run a pending switch at a safe point + launch the dialog on request**

In `EditorApp::MainLoop`, right after the minimized-`continue` guard and before the input block, add:

```cpp
            // File->Open Project: run a pending soft-restart at a safe point (top of
            // frame, never mid-render). Set by FolderPickedThunk during PumpEvents.
            if (!m_pendingProjectPath.empty())
            {
                const std::string p = m_pendingProjectPath;
                m_pendingProjectPath.clear();
                SwitchProject(p);
            }
```

Change the `BeginDockSpace` call to pass the request flag:

```cpp
            Arcane::Editor::BeginDockSpace(*m_undo, m_openProjectRequested);
```

Immediately after `Arcane::Editor::EndDockSpace();`, add:

```cpp
            if (m_openProjectRequested)
            {
                m_openProjectRequested = false;
                m_gpu->Win().ShowOpenFolderDialog(&EditorApp::FolderPickedThunk, this);
            }
```

- [ ] **Step 7: Build**

Run: `cd Arcane && msbuild Arcane.slnx /t:ArcaneEditor /p:Configuration=Debug /m`
Expected: builds clean (this also rebuilds `Arcane.dll` for the `Window` change; `msbuild` resolves the dependency).

- [ ] **Step 8: Desk-verify — interactive** *(human, at the desk)*

Run:
```
cd bin\Debug-windows-x86_64-md\ArcaneEditor
ArcaneEditor.exe
```
Then in the app:
1. **File → Open Project...** → OS folder dialog opens. Pick `bin\Debug-windows-x86_64-md\ArcaneEditor\SampleProject`.
   Expected: title changes to **"Arcane Editor -- SampleProject"**, the plugin reloads (Sandbox scene appears), the Assets panel updates to the project, input still works.
2. **File → Open Project... → Cancel** → no change, no crash.
3. **File → Open Project...** → pick a folder with **no** `.arcproj` → Console shows `Open Project: '<path>' is not a valid Arcane project`, the current session stays intact (no teardown), no crash.
Close the window to exit.

- [ ] **Step 9: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Platform/Window.hpp Arcane/Arcane/src/Arcane/Platform/Window.cpp Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorApp.cpp
git commit -m "feat(arcane): Editor File->Open Project soft-restart via SDL folder dialog (project format S1b)"
```

> **Shipped amendments (post-review fixes, commit `67545b90`).** Task 7's code review
> found two Critical UB defects in the design above; the shipped code differs from the
> Step 4–6 snippets in these ways:
> - **ABI pre-validation before teardown.** `SwitchProject` captures `Project::Open(path)`
>   into `probe` and, *before any teardown*, also rejects `probe->Manifest().engineAbi !=
>   kGamePluginABIVersion` (log + return). The original snippet only checked `Project::Open`
>   success, so an ABI-mismatched pick passed validation, tore down the plugin, then failed
>   inside `Runtime::OpenProject` — leaving `m_plugin` disengaged mid-`MainLoop` (crash).
>   (Needs `#include <Arcane/Plugin/PluginABI.hpp>` in `EditorApp.cpp`.)
> - **Guarded `m_plugin` dereferences.** Every `m_plugin->Vtable()` / `m_plugin->Poll()` in
>   `MainLoop` is optional-guarded (`m_plugin ? m_plugin->Vtable() : nullptr`, `if (m_plugin)
>   m_plugin->Poll();`) as defense-in-depth against any disengaged-plugin state.
> - **`m_pendingProjectPath` is mutex-guarded.** The SDL folder-dialog callback runs on an
>   SDL-owned **background thread** (verified against SDL3 3.4.0's Windows backend), *not*
>   the main thread during `PumpEvents` as Step 4/5 assumed. `EditorApp` gains a
>   `std::mutex m_pendingProjectMutex` (+ `#include <mutex>`); `FolderPickedThunk` writes the
>   path under `lock_guard`; `MainLoop` swaps it out under `lock_guard` into a local, then
>   calls `SwitchProject` on that local *outside* the lock (so the slow reload never stalls
>   the callback thread). This replaces the unsynchronized bare write/read.

---

## Post-implementation verification

- [ ] **Full headless suite (no regression):** `cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "~[gpu]"` — expect all pass, `[project]` and `[loom]` grown, `[assets]` grown, total ≥ the 27951/366 baseline.
- [ ] **Desk gauntlet (human):** the Task 5/6/7 desk-verifies, plus a no-arg `Loom.exe` and `ArcaneEditor.exe` to confirm the non-breaking fallback.

## Self-review notes (author)

- **Spec coverage:** ownership→Runtime (T2); `--project`+File→Open (T3/T7); gameModule via PluginHost + ABI cross-check (T2/T5/T6); Thick content routing — `Assets::SetContentRoot` (T1) + input via `game://` (T3/T5/T6); input load moved out of `GpuContext` (T5); demo project (T4); soft-restart (T7); `engine://` reserved-empty (untouched — no code); §13.3 deferred (no code); `Project` default ctor private (T2); non-breaking fallback (T5/T6 verifies). All spec sections map to a task.
- **ABI coupling:** the hardcoded `"abi":5` in `SampleProject.arcproj` (T4) and the two `[loom]` fixtures (T3) is called out in Global Constraints — bump with `kGamePluginABIVersion`.
