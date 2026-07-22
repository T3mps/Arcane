# Arcane Project Format — Slice 1 (Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the engine-side project-format core — an `Arcane::Project` you can open, create, and resolve assets through — so later slices (GUID identity, config, plugins, SDK) and the host wiring have a tested foundation to build on.

**Architecture:** A new engine module `Arcane/Project/` in `Arcane.dll` (`ARCANE_API`), pure CPU (no device/GPU). `ProjectManifest` parses `.arcproj` JSON; `MountTable` maps mount schemes (`game://`) to physical roots and resolves logical paths to filesystem paths; `AssetId` is the opaque, GUID-ready reference handle whose Slice-1 backing is a logical mount path; `Project` ties them together (`Open` / `Create` / `ResolveAsset`). All unit-tested headlessly via `ArcaneTests` (`[project]` tag).

**Tech Stack:** C++23, nlohmann/json (included as `<Json.hpp>`), Catch2, premake5 + MSBuild, `std::filesystem`.

**Scope note:** This plan is the **engine-side core only** — fully headless-testable, no GPU. Wiring `Project::Open` into the Arcane Editor / runtime host (the "open a project instead of `data/`-next-to-exe" payoff) is **Slice 1b**, its own follow-up plan, because it is GPU/desk integration that depends on this core. Slices 2–5 (GUID/AssetRegistry, config, plugin-content, engine-as-SDK) are separate plans per the spec's §11.

Spec: `docs/superpowers/specs/2026-07-22-arcane-project-format-design.md`.

## Global Constraints

- **Language/CRT:** C++23, `/MD` (dynamic CRT). No `/fp:fast`. UTF-8 without BOM, ASCII-only comments.
- **Placement:** engine module under `Arcane/Arcane/src/Arcane/Project/`. Engine-public types use `ARCANE_API` (include `<Arcane/Base/Api.hpp>`); `namespace Arcane`. Consumers include via `<Arcane/Project/...>`. C4251 dll-interface warnings on `std::` members are pre-existing/benign (see `PluginHost.hpp`).
- **JSON:** `#include <Json.hpp>`; type is `nlohmann::json`; parse via `nlohmann::json::parse(...)` inside try/catch.
- **Tests:** Catch2, `#include <catch2/catch_test_macros.hpp>`, tag **`[project]`**, CPU-only (no `[gpu]`). Include the module under test via `<Arcane/Project/...>` (ArcaneTests links `Arcane` and has `Arcane/src` on its include path). New test files live in `Arcane/Tests/src/`.
- **New files need regeneration:** premake globs `src/**` at generation time, so after adding any new `.hpp`/`.cpp`/test file, run GenerateProjects **before** building.

### Build & Test commands (referenced by tasks; run from repo root `D:\dev\starworks\Gacha`)

PowerShell (primary shell):

```powershell
# (G) Regenerate projects — run after adding any NEW file:
& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"

# (B) Build the test exe (also builds Arcane.dll, which the Project module compiles into):
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /t:ArcaneTests /p:Configuration=Debug /m /nologo /v:minimal

# (T) Run the project tests FROM THE EXE DIR (relative asset/plugin paths break from repo root):
Set-Location "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"
.\ArcaneTests.exe "[project]"
Set-Location "D:\dev\starworks\Gacha"
```

If the MSBuild path differs, resolve it once with:
`& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find "MSBuild\**\Bin\MSBuild.exe"`

---

## File Structure

Created by this plan:

- `Arcane/Arcane/src/Arcane/Project/AssetId.hpp` — opaque, GUID-ready asset handle (header-only value type).
- `Arcane/Arcane/src/Arcane/Project/MountTable.hpp` / `.cpp` — mount registry + logical-path resolution.
- `Arcane/Arcane/src/Arcane/Project/ProjectManifest.hpp` / `.cpp` — `.arcproj` schema struct + JSON parse/validate.
- `Arcane/Arcane/src/Arcane/Project/Project.hpp` / `.cpp` — `Open` / `Create` / `ResolveAsset`; owns manifest + mounts.
- `Arcane/Tests/src/AssetIdTest.cpp`, `MountTableTest.cpp`, `ProjectManifestTest.cpp`, `ProjectTest.cpp` — `[project]` unit tests.

Each file has one responsibility; `Project` is the only type that composes the others.

---

## Task 1: AssetId — opaque, GUID-ready asset handle

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Project/AssetId.hpp`
- Test: `Arcane/Tests/src/AssetIdTest.cpp`

**Interfaces:**
- Produces:
  - `Arcane::AssetId` — default-constructs invalid; `static AssetId FromMountPath(std::string_view)`; `bool IsValid() const`; `const std::string& Key() const` (seam-only accessor); `operator==`/`operator!=`; `std::hash<Arcane::AssetId>` specialization.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/AssetIdTest.cpp`:

```cpp
// Arcane::AssetId: opaque, GUID-ready asset handle. Slice 1 backing = a logical
// mount path; the swap to a Guid (Slice 2) must not change this interface. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/AssetId.hpp>

#include <unordered_set>

TEST_CASE("AssetId default-constructs invalid; FromMountPath is valid", "[project]")
{
    Arcane::AssetId nil;
    CHECK_FALSE(nil.IsValid());

    Arcane::AssetId id = Arcane::AssetId::FromMountPath("game://characters/hero.png");
    CHECK(id.IsValid());
    CHECK(id.Key() == "game://characters/hero.png");
}

TEST_CASE("AssetId equality and hashing key off the same identity", "[project]")
{
    auto a = Arcane::AssetId::FromMountPath("game://a.png");
    auto b = Arcane::AssetId::FromMountPath("game://a.png");
    auto c = Arcane::AssetId::FromMountPath("game://b.png");

    CHECK(a == b);
    CHECK(a != c);

    std::unordered_set<Arcane::AssetId> set;
    set.insert(a);
    set.insert(b);   // same identity as a
    set.insert(c);
    CHECK(set.size() == 2);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (B) then (T). Expected: FAIL to compile — `Arcane/Project/AssetId.hpp` not found.

- [ ] **Step 3: Write the implementation**

Create `Arcane/Arcane/src/Arcane/Project/AssetId.hpp`:

```cpp
#pragma once

// AssetId: an opaque handle to an asset. Callers hold an AssetId and never see a
// path or a raw id. Slice 1 backing = a logical mount path ("game://a.png"); Slice 2
// swaps the backing to a Guid + AssetRegistry WITHOUT changing this interface.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace Arcane
{
    class AssetId
    {
    public:
        AssetId() = default;

        static AssetId FromMountPath(std::string_view mountPath)
        {
            return AssetId(std::string(mountPath));
        }

        bool IsValid() const { return !m_key.empty(); }

        // Seam-only accessor (MountTable / Project use it to resolve). NOT a public
        // path API for callers -- treat AssetId as opaque everywhere else.
        const std::string& Key() const { return m_key; }

        bool operator==(const AssetId& o) const { return m_key == o.m_key; }
        bool operator!=(const AssetId& o) const { return !(*this == o); }

    private:
        explicit AssetId(std::string key) : m_key(std::move(key)) {}
        std::string m_key;   // Slice 1: the logical mount path; opaque to callers
    };
}

template <>
struct std::hash<Arcane::AssetId>
{
    std::size_t operator()(const Arcane::AssetId& id) const noexcept
    {
        return std::hash<std::string>{}(id.Key());
    }
};
```

- [ ] **Step 4: Regenerate + build + run to verify it passes**

Run (G), then (B), then (T). Expected: PASS — "All tests passed" including the 2 new `AssetId` cases.

- [ ] **Step 5: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Project/AssetId.hpp Arcane/Tests/src/AssetIdTest.cpp
git commit -m "feat(arcane): AssetId -- opaque GUID-ready asset handle (project format)"
```

---

## Task 2: MountTable — mount registry + logical-path resolution

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Project/MountTable.hpp`, `Arcane/Arcane/src/Arcane/Project/MountTable.cpp`
- Test: `Arcane/Tests/src/MountTableTest.cpp`

**Interfaces:**
- Produces:
  - `class ARCANE_API Arcane::MountTable`:
    - `void Mount(std::string scheme, std::filesystem::path root)`
    - `bool HasMount(std::string_view scheme) const`
    - `std::optional<std::filesystem::path> Resolve(std::string_view mountPath) const` — parses `scheme://relative`, returns `root / relative`; `nullopt` if malformed or scheme unmounted.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/MountTableTest.cpp`:

```cpp
// Arcane::MountTable: scheme -> physical root, and "game://a/b" -> root/a/b. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/MountTable.hpp>

TEST_CASE("MountTable resolves a mounted scheme to root + relative", "[project]")
{
    Arcane::MountTable mt;
    mt.Mount("game", "C:/proj/Content");

    CHECK(mt.HasMount("game"));
    auto p = mt.Resolve("game://characters/hero.png");
    REQUIRE(p.has_value());
    CHECK(p->generic_string() == "C:/proj/Content/characters/hero.png");
}

TEST_CASE("MountTable rejects unmounted schemes and malformed paths", "[project]")
{
    Arcane::MountTable mt;
    mt.Mount("game", "C:/proj/Content");

    CHECK_FALSE(mt.HasMount("engine"));
    CHECK_FALSE(mt.Resolve("engine://x.png").has_value());   // scheme not mounted
    CHECK_FALSE(mt.Resolve("no-scheme-here").has_value());   // missing "://"
    CHECK_FALSE(mt.Resolve("game://").has_value());          // empty relative
    CHECK_FALSE(mt.Resolve("://x.png").has_value());         // empty scheme
}

TEST_CASE("MountTable Mount replaces an existing root", "[project]")
{
    Arcane::MountTable mt;
    mt.Mount("game", "C:/old");
    mt.Mount("game", "C:/new");
    auto p = mt.Resolve("game://a.png");
    REQUIRE(p.has_value());
    CHECK(p->generic_string() == "C:/new/a.png");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (G), (B), (T). Expected: FAIL to compile — `MountTable.hpp` not found.

- [ ] **Step 3: Write the header**

Create `Arcane/Arcane/src/Arcane/Project/MountTable.hpp`:

```cpp
#pragma once

// MountTable: maps a mount scheme ("game", "engine", "plugin/<name>") to a physical
// root directory, and resolves a logical mount path ("game://a/b.png") to a filesystem
// path. Decouples logical asset addresses from where the project sits on disk.

#include <Arcane/Base/Api.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Arcane
{
    class ARCANE_API MountTable
    {
    public:
        // Register (or replace) a mount root. `scheme` is the text before "://".
        void Mount(std::string scheme, std::filesystem::path root);

        bool HasMount(std::string_view scheme) const;

        // "scheme://relative" -> root / relative. nullopt if the string is not a
        // well-formed mount path (needs a non-empty scheme, "://", and a non-empty
        // relative part) or the scheme is not mounted.
        std::optional<std::filesystem::path> Resolve(std::string_view mountPath) const;

    private:
        std::unordered_map<std::string, std::filesystem::path> m_roots;
    };
}
```

- [ ] **Step 4: Write the implementation**

Create `Arcane/Arcane/src/Arcane/Project/MountTable.cpp`:

```cpp
#include <Arcane/Project/MountTable.hpp>

namespace Arcane
{
    void MountTable::Mount(std::string scheme, std::filesystem::path root)
    {
        m_roots[std::move(scheme)] = std::move(root);
    }

    bool MountTable::HasMount(std::string_view scheme) const
    {
        return m_roots.find(std::string(scheme)) != m_roots.end();
    }

    std::optional<std::filesystem::path> MountTable::Resolve(std::string_view mountPath) const
    {
        const std::size_t sep = mountPath.find("://");
        if (sep == std::string_view::npos)
            return std::nullopt;

        const std::string_view scheme = mountPath.substr(0, sep);
        const std::string_view rel    = mountPath.substr(sep + 3);
        if (scheme.empty() || rel.empty())
            return std::nullopt;

        const auto it = m_roots.find(std::string(scheme));
        if (it == m_roots.end())
            return std::nullopt;

        return it->second / std::filesystem::path(rel);
    }
}
```

- [ ] **Step 5: Regenerate + build + run to verify it passes**

Run (G), (B), (T). Expected: PASS — the 3 new `MountTable` cases pass.

- [ ] **Step 6: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Project/MountTable.hpp Arcane/Arcane/src/Arcane/Project/MountTable.cpp Arcane/Tests/src/MountTableTest.cpp
git commit -m "feat(arcane): MountTable -- mount schemes + logical-path resolution (project format)"
```

---

## Task 3: ProjectManifest — .arcproj schema + JSON parse/validate

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Project/ProjectManifest.hpp`, `Arcane/Arcane/src/Arcane/Project/ProjectManifest.cpp`
- Test: `Arcane/Tests/src/ProjectManifestTest.cpp`

**Interfaces:**
- Produces:
  - `struct Arcane::ProjectManifest` with fields: `int formatVersion`, `std::string name`, `std::string description`, `int engineAbi`, `std::string gameModule`, `std::vector<PluginRef> plugins` (`PluginRef{ std::string name; bool enabled; }`), `std::string bootScene`.
  - `static ARCANE_API std::optional<ProjectManifest> FromJson(const nlohmann::json&)` — nullopt on missing/invalid required fields (`formatVersion` int > 0, non-empty `name`, `engine.abi` int).
  - `static ARCANE_API std::optional<ProjectManifest> LoadFile(const std::filesystem::path&)` — reads + parses + validates; nullopt on IO/parse/schema failure.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/ProjectManifestTest.cpp`:

```cpp
// Arcane::ProjectManifest: parse + validate a .arcproj JSON document. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/ProjectManifest.hpp>

#include <Json.hpp>

TEST_CASE("ProjectManifest parses a full valid document", "[project]")
{
    const auto doc = nlohmann::json::parse(R"({
        "formatVersion": 1,
        "name": "Aphelyon",
        "description": "test",
        "engine": { "abi": 4 },
        "gameModule": "Aphelyon.dll",
        "plugins": [ { "name": "Sandbox", "enabled": false } ],
        "bootScene": "game://scenes/main.ascene"
    })");

    auto m = Arcane::ProjectManifest::FromJson(doc);
    REQUIRE(m.has_value());
    CHECK(m->formatVersion == 1);
    CHECK(m->name == "Aphelyon");
    CHECK(m->description == "test");
    CHECK(m->engineAbi == 4);
    CHECK(m->gameModule == "Aphelyon.dll");
    REQUIRE(m->plugins.size() == 1);
    CHECK(m->plugins[0].name == "Sandbox");
    CHECK(m->plugins[0].enabled == false);
    CHECK(m->bootScene == "game://scenes/main.ascene");
}

TEST_CASE("ProjectManifest defaults optional fields", "[project]")
{
    const auto doc = nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "Bare", "engine": { "abi": 4 }
    })");
    auto m = Arcane::ProjectManifest::FromJson(doc);
    REQUIRE(m.has_value());
    CHECK(m->description.empty());
    CHECK(m->gameModule.empty());
    CHECK(m->plugins.empty());
    CHECK(m->bootScene.empty());
}

TEST_CASE("ProjectManifest rejects missing required fields", "[project]")
{
    // missing name
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({ "formatVersion": 1, "engine": { "abi": 4 } })")).has_value());
    // missing engine.abi
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({ "formatVersion": 1, "name": "X" })")).has_value());
    // formatVersion not > 0
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({ "formatVersion": 0, "name": "X", "engine": { "abi": 4 } })")).has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (G), (B), (T). Expected: FAIL to compile — `ProjectManifest.hpp` not found.

- [ ] **Step 3: Write the header**

Create `Arcane/Arcane/src/Arcane/Project/ProjectManifest.hpp`:

```cpp
#pragma once

// ProjectManifest: the parsed form of a .arcproj file -- the project's identity and
// engine/module/plugin declarations. Required fields: formatVersion (>0), name,
// engine.abi. Everything else is optional (a content-only project omits gameModule).

#include <Arcane/Base/Api.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <Json.hpp>   // nlohmann::json (the vendored single header)

namespace Arcane
{
    struct ProjectManifest
    {
        struct PluginRef
        {
            std::string name;
            bool        enabled = true;
        };

        int                    formatVersion = 0;
        std::string            name;
        std::string            description;
        int                    engineAbi = 0;      // "engine": { "abi": N }
        std::string            gameModule;         // may be empty (content-only)
        std::vector<PluginRef> plugins;
        std::string            bootScene;           // e.g. "game://scenes/main.ascene"

        // Parse + validate a JSON document. nullopt on schema violation.
        static ARCANE_API std::optional<ProjectManifest> FromJson(const nlohmann::json& doc);

        // Read + parse + validate a .arcproj file. nullopt on IO/parse/schema failure.
        static ARCANE_API std::optional<ProjectManifest> LoadFile(const std::filesystem::path& file);
    };
}
```

> **Note:** the vendored nlohmann is a **single header** included as `<Json.hpp>` (confirmed: `Assets.hpp` includes it the same way; there is no `nlohmann/json_fwd.hpp` layout). `nlohmann::json` is available from it.

- [ ] **Step 4: Write the implementation**

Create `Arcane/Arcane/src/Arcane/Project/ProjectManifest.cpp`:

```cpp
#include <Arcane/Project/ProjectManifest.hpp>

#include <Arcane/Base/Log.hpp>   // ARC_WARN (defined at Log.hpp:40)

#include <Json.hpp>

#include <fstream>
#include <sstream>

namespace Arcane
{
    std::optional<ProjectManifest> ProjectManifest::FromJson(const nlohmann::json& doc)
    {
        if (!doc.is_object())
            return std::nullopt;

        ProjectManifest m;

        // Required: formatVersion (> 0), name (non-empty), engine.abi (int).
        if (!doc.contains("formatVersion") || !doc["formatVersion"].is_number_integer())
            return std::nullopt;
        m.formatVersion = doc["formatVersion"].get<int>();
        if (m.formatVersion <= 0)
            return std::nullopt;

        if (!doc.contains("name") || !doc["name"].is_string())
            return std::nullopt;
        m.name = doc["name"].get<std::string>();
        if (m.name.empty())
            return std::nullopt;

        if (!doc.contains("engine") || !doc["engine"].is_object()
            || !doc["engine"].contains("abi") || !doc["engine"]["abi"].is_number_integer())
            return std::nullopt;
        m.engineAbi = doc["engine"]["abi"].get<int>();

        // Optional.
        m.description = doc.value("description", std::string{});
        m.gameModule  = doc.value("gameModule", std::string{});
        m.bootScene   = doc.value("bootScene", std::string{});

        if (doc.contains("plugins") && doc["plugins"].is_array())
        {
            for (const auto& p : doc["plugins"])
            {
                if (!p.is_object() || !p.contains("name") || !p["name"].is_string())
                    continue;
                PluginRef ref;
                ref.name    = p["name"].get<std::string>();
                ref.enabled = p.value("enabled", true);
                m.plugins.push_back(std::move(ref));
            }
        }

        return m;
    }

    std::optional<ProjectManifest> ProjectManifest::LoadFile(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            ARC_WARN("ProjectManifest: cannot open '{}'", file.generic_string());
            return std::nullopt;
        }
        std::stringstream ss;
        ss << in.rdbuf();

        nlohmann::json doc;
        try
        {
            doc = nlohmann::json::parse(ss.str());
        }
        catch (const std::exception& e)
        {
            ARC_WARN("ProjectManifest: parse failed for '{}': {}", file.generic_string(), e.what());
            return std::nullopt;
        }
        return FromJson(doc);
    }
}
```

> **Note:** `ARC_WARN(...)` is defined in `<Arcane/Base/Log.hpp>` (confirmed: `Assets.cpp:4` includes it; `Log.hpp:40` -> `::Arcane::Log::Engine()->warn(...)`). Use that exact include.

- [ ] **Step 5: Regenerate + build + run to verify it passes**

Run (G), (B), (T). Expected: PASS — the 3 new `ProjectManifest` cases pass.

- [ ] **Step 6: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Project/ProjectManifest.hpp Arcane/Arcane/src/Arcane/Project/ProjectManifest.cpp Arcane/Tests/src/ProjectManifestTest.cpp
git commit -m "feat(arcane): ProjectManifest -- .arcproj schema parse/validate (project format)"
```

---

## Task 4: Project::Open — discover + load + default mounts

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Project/Project.hpp`, `Arcane/Arcane/src/Arcane/Project/Project.cpp`
- Test: `Arcane/Tests/src/ProjectTest.cpp`

**Interfaces:**
- Consumes: `ProjectManifest::LoadFile`, `MountTable::Mount`, `AssetId`.
- Produces:
  - `class ARCANE_API Arcane::Project`:
    - `static std::optional<Project> Open(const std::filesystem::path& pathOrFile)` — accepts a project folder (finds the single `*.arcproj`) or a direct `.arcproj` path; loads the manifest; mounts `game://` -> `<root>/Content`. nullopt on: no/multiple `.arcproj`, or bad manifest.
    - `const ProjectManifest& Manifest() const`
    - `const std::filesystem::path& Root() const`
    - `const MountTable& Mounts() const`

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/ProjectTest.cpp`:

```cpp
// Arcane::Project: open/create a project + resolve assets through its mounts. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/Project.hpp>

#include <filesystem>
#include <fstream>

namespace
{
    // A unique temp dir for a test, cleaned before use. (Date/random are unavailable
    // in some sandboxes; a fixed per-test name + remove_all is deterministic.)
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / "arcane_project_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }

    void WriteFile(const std::filesystem::path& p, const std::string& text)
    {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
}

TEST_CASE("Project::Open loads a folder's manifest and mounts game://", "[project]")
{
    const auto dir = TempDir("open_ok");
    WriteFile(dir / "Aphelyon.arcproj",
              R"({ "formatVersion": 1, "name": "Aphelyon", "engine": { "abi": 4 } })");
    std::filesystem::create_directories(dir / "Content");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK(proj->Manifest().name == "Aphelyon");
    CHECK(proj->Root() == dir);
    CHECK(proj->Mounts().HasMount("game"));

    auto p = proj->Mounts().Resolve("game://x.png");
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "x.png");
}

TEST_CASE("Project::Open fails on missing or ambiguous manifest", "[project]")
{
    const auto none = TempDir("open_none");
    CHECK_FALSE(Arcane::Project::Open(none).has_value());   // no .arcproj

    const auto many = TempDir("open_many");
    WriteFile(many / "A.arcproj", R"({ "formatVersion": 1, "name": "A", "engine": { "abi": 4 } })");
    WriteFile(many / "B.arcproj", R"({ "formatVersion": 1, "name": "B", "engine": { "abi": 4 } })");
    CHECK_FALSE(Arcane::Project::Open(many).has_value());   // ambiguous
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (G), (B), (T). Expected: FAIL to compile — `Project.hpp` not found.

- [ ] **Step 3: Write the header**

Create `Arcane/Arcane/src/Arcane/Project/Project.hpp`:

```cpp
#pragma once

// Project: an opened Arcane project -- its manifest, root directory, and mount table.
// Open() discovers/loads a .arcproj and registers default mounts; ResolveAsset() is the
// asset-reference seam (AssetId -> filesystem path via the mounts). Create() scaffolds a
// new project skeleton on disk.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/MountTable.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace Arcane
{
    class ARCANE_API Project
    {
    public:
        // Open a project folder (finds the single *.arcproj inside) or a direct
        // .arcproj file. Loads the manifest and mounts game:// -> <root>/Content.
        // nullopt on: no/multiple .arcproj, or an invalid manifest.
        static std::optional<Project> Open(const std::filesystem::path& pathOrFile);

        // Scaffold a new project at `dir` named `name`: creates the folder skeleton
        // (Source/ Content/ Config/ Plugins/), writes <name>.arcproj and .gitignore,
        // then opens it. nullopt if `dir` exists non-empty or on IO error.
        static std::optional<Project> Create(const std::filesystem::path& dir, std::string name);

        const ProjectManifest&       Manifest() const { return m_manifest; }
        const std::filesystem::path& Root()     const { return m_root; }
        const MountTable&            Mounts()   const { return m_mounts; }

        // The asset-reference seam: AssetId -> physical file. Slice 1 resolves via the
        // MountTable (AssetId backing = a logical mount path). Slice 2 reroutes this
        // through the AssetRegistry (GUID) WITHOUT changing the signature.
        std::optional<std::filesystem::path> ResolveAsset(const AssetId& id) const;

    private:
        std::filesystem::path m_root;
        ProjectManifest       m_manifest;
        MountTable            m_mounts;
    };
}
```

> **Note:** `ResolveAsset` is declared here but implemented in Task 5 (kept out of this task's build so the two behaviors gate independently). Add its definition in Task 5.

- [ ] **Step 4: Write the implementation (Open only)**

Create `Arcane/Arcane/src/Arcane/Project/Project.cpp`:

```cpp
#include <Arcane/Project/Project.hpp>

#include <Arcane/Base/Log.hpp>   // ARC_WARN (confirmed path, see Task 3)

#include <system_error>

namespace Arcane
{
    std::optional<Project> Project::Open(const std::filesystem::path& pathOrFile)
    {
        std::error_code ec;

        std::filesystem::path root;
        std::filesystem::path manifestFile;

        if (std::filesystem::is_regular_file(pathOrFile, ec)
            && pathOrFile.extension() == ".arcproj")
        {
            manifestFile = pathOrFile;
            root         = pathOrFile.parent_path();
        }
        else if (std::filesystem::is_directory(pathOrFile, ec))
        {
            root = pathOrFile;
            // Find exactly one *.arcproj in the folder.
            for (const auto& entry : std::filesystem::directory_iterator(pathOrFile, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".arcproj")
                {
                    if (!manifestFile.empty())
                    {
                        ARC_WARN("Project::Open: multiple .arcproj in '{}'", pathOrFile.generic_string());
                        return std::nullopt;   // ambiguous
                    }
                    manifestFile = entry.path();
                }
            }
        }

        if (manifestFile.empty())
        {
            ARC_WARN("Project::Open: no .arcproj at '{}'", pathOrFile.generic_string());
            return std::nullopt;
        }

        auto manifest = ProjectManifest::LoadFile(manifestFile);
        if (!manifest)
            return std::nullopt;   // LoadFile already logged

        Project proj;
        proj.m_root     = root;
        proj.m_manifest = std::move(*manifest);
        // Default mounts. engine:// and plugin://<name>/ are registered in later slices.
        proj.m_mounts.Mount("game", root / "Content");
        return proj;
    }
}
```

- [ ] **Step 5: Regenerate + build + run to verify it passes**

Run (G), (B), (T). Expected: PASS — the 2 new `Project::Open` cases pass.

- [ ] **Step 6: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Project/Project.hpp Arcane/Arcane/src/Arcane/Project/Project.cpp Arcane/Tests/src/ProjectTest.cpp
git commit -m "feat(arcane): Project::Open -- discover .arcproj + default mounts (project format)"
```

---

## Task 5: Project::ResolveAsset — the reference seam

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Project/Project.cpp` (add `ResolveAsset` definition)
- Test: `Arcane/Tests/src/ProjectTest.cpp` (add a case)

**Interfaces:**
- Consumes: `Project::Open`, `AssetId::FromMountPath`, `MountTable::Resolve`.
- Produces: `std::optional<std::filesystem::path> Project::ResolveAsset(const AssetId&) const` (declared in Task 4's header).

- [ ] **Step 1: Add the failing test**

Append to `Arcane/Tests/src/ProjectTest.cpp`:

```cpp
TEST_CASE("Project::ResolveAsset maps an AssetId through the mounts", "[project]")
{
    const auto dir = TempDir("resolve");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 4 } })");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    auto id = Arcane::AssetId::FromMountPath("game://characters/hero.png");
    auto p  = proj->ResolveAsset(id);
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "characters" / "hero.png");

    // Unmounted scheme -> no resolution.
    CHECK_FALSE(proj->ResolveAsset(
        Arcane::AssetId::FromMountPath("engine://x.png")).has_value());

    // Invalid id -> no resolution.
    CHECK_FALSE(proj->ResolveAsset(Arcane::AssetId{}).has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (B), (T). Expected: FAIL to link — `Project::ResolveAsset` unresolved external symbol.

- [ ] **Step 3: Add the implementation**

Append to `Arcane/Arcane/src/Arcane/Project/Project.cpp` (inside `namespace Arcane`):

```cpp
    std::optional<std::filesystem::path> Project::ResolveAsset(const AssetId& id) const
    {
        if (!id.IsValid())
            return std::nullopt;
        // Slice 1: the AssetId key IS the logical mount path. Slice 2 replaces this body
        // with an AssetRegistry lookup (GUID -> mount path) before the mount resolve.
        return m_mounts.Resolve(id.Key());
    }
```

- [ ] **Step 4: Build + run to verify it passes**

Run (B), (T). Expected: PASS — the new `ResolveAsset` case passes (no new files, so (G) not needed).

- [ ] **Step 5: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Project/Project.cpp Arcane/Tests/src/ProjectTest.cpp
git commit -m "feat(arcane): Project::ResolveAsset -- AssetId -> filesystem seam (project format)"
```

---

## Task 6: Project::Create — scaffold a new project skeleton

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Project/Project.cpp` (add `Create` definition)
- Test: `Arcane/Tests/src/ProjectTest.cpp` (add a case)

**Interfaces:**
- Consumes: `Project::Open`.
- Produces: `static std::optional<Project> Project::Create(const std::filesystem::path& dir, std::string name)` (declared in Task 4's header) — creates `Source/ Content/ Config/ Plugins/`, `<name>.arcproj`, `.gitignore`; then `Open(dir)`.

- [ ] **Step 1: Add the failing test**

First add `#include <Arcane/Plugin/PluginABI.hpp>` to the includes at the top of `Arcane/Tests/src/ProjectTest.cpp` (for `kGamePluginABIVersion`), then append:

```cpp
TEST_CASE("Project::Create scaffolds the skeleton, manifest, and .gitignore", "[project]")
{
    const auto dir = TempDir("create");

    auto proj = Arcane::Project::Create(dir, "Aphelyon");
    REQUIRE(proj.has_value());

    // Skeleton dirs.
    CHECK(std::filesystem::is_directory(dir / "Source"));
    CHECK(std::filesystem::is_directory(dir / "Content"));
    CHECK(std::filesystem::is_directory(dir / "Config"));
    CHECK(std::filesystem::is_directory(dir / "Plugins"));
    // Manifest + gitignore.
    CHECK(std::filesystem::is_regular_file(dir / "Aphelyon.arcproj"));
    CHECK(std::filesystem::is_regular_file(dir / ".gitignore"));

    // The created project is valid and re-openable.
    CHECK(proj->Manifest().name == "Aphelyon");
    CHECK(proj->Manifest().engineAbi == static_cast<int>(Arcane::kGamePluginABIVersion));
    CHECK(Arcane::Project::Open(dir).has_value());

    auto id = Arcane::AssetId::FromMountPath("game://a.png");
    CHECK(proj->ResolveAsset(id) == dir / "Content" / "a.png");
}

TEST_CASE("Project::Create refuses a non-empty target directory", "[project]")
{
    const auto dir = TempDir("create_nonempty");
    WriteFile(dir / "existing.txt", "x");
    CHECK_FALSE(Arcane::Project::Create(dir, "X").has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (B), (T). Expected: FAIL to link — `Project::Create` unresolved external symbol.

- [ ] **Step 3: Add the implementation**

Add to the top of `Arcane/Arcane/src/Arcane/Project/Project.cpp` includes:

```cpp
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion
#include <fstream>
```

Append inside `namespace Arcane` (uses the engine's ABI constant; see the note below):

```cpp
    std::optional<Project> Project::Create(const std::filesystem::path& dir, std::string name)
    {
        std::error_code ec;

        // Refuse a non-empty existing dir (avoid clobbering); an absent dir is fine.
        if (std::filesystem::exists(dir, ec) && !std::filesystem::is_empty(dir, ec))
        {
            ARC_WARN("Project::Create: target '{}' exists and is not empty", dir.generic_string());
            return std::nullopt;
        }

        for (const char* sub : { "Source", "Content", "Config", "Plugins" })
        {
            std::filesystem::create_directories(dir / sub, ec);
            if (ec)
            {
                ARC_WARN("Project::Create: mkdir '{}' failed: {}", (dir / sub).generic_string(), ec.message());
                return std::nullopt;
            }
        }

        // Minimal manifest. ABI comes from the engine's plugin-ABI constant so a freshly
        // created project always targets the engine that created it.
        const std::string manifest =
            "{\n"
            "  \"formatVersion\": 1,\n"
            "  \"name\": \"" + name + "\",\n"
            "  \"engine\": { \"abi\": " + std::to_string(Arcane::kGamePluginABIVersion) + " },\n"
            "  \"gameModule\": \"\",\n"
            "  \"plugins\": [],\n"
            "  \"bootScene\": \"\"\n"
            "}\n";
        std::ofstream(dir / (name + ".arcproj"), std::ios::binary) << manifest;

        static const char* kGitignore =
            "# Derived / generated -- never commit\n"
            "Binaries/\n"
            "Intermediate/\n"
            "Saved/\n"
            "\n"
            "# Plugin generated output\n"
            "Plugins/**/Binaries/\n"
            "Plugins/**/Intermediate/\n"
            "\n"
            "# Generated IDE project files\n"
            "*.sln\n"
            "*.vcxproj\n"
            "*.vcxproj.filters\n"
            "*.vcxproj.user\n"
            ".vs/\n";
        std::ofstream(dir / ".gitignore", std::ios::binary) << kGitignore;

        return Open(dir);
    }
```

> **Note (ABI source):** the engine's canonical plugin ABI is `Arcane::kGamePluginABIVersion` (confirmed in `Arcane/Plugin/PluginABI.hpp:32`, currently `5`). `Project::Create` writes that value so a new project always targets the engine that created it, and the two never drift. The manifest *parser* (Task 3) accepts any integer `abi`; only `Create` pins the current value.

- [ ] **Step 4: Build + run to verify it passes**

Run (B), (T). Expected: PASS — the 2 new `Project::Create` cases pass; full `[project]` suite green.

- [ ] **Step 5: Full-suite regression check**

Run the whole CPU suite to confirm no regression:

```powershell
Set-Location "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"
.\ArcaneTests.exe "~[gpu]"
Set-Location "D:\dev\starworks\Gacha"
```

Expected: "All tests passed" — the prior baseline count **plus the new `[project]` assertions/cases**.

- [ ] **Step 6: Commit**

```powershell
git add Arcane/Arcane/src/Arcane/Project/Project.cpp Arcane/Tests/src/ProjectTest.cpp
git commit -m "feat(arcane): Project::Create -- scaffold project skeleton + .gitignore (project format)"
```

---

## Self-Review

- **Spec coverage (Slice 1 scope):** `.arcproj` manifest schema + loader (Task 3) ✓; on-disk skeleton + `.gitignore` (Task 6 scaffolds it) ✓; mount points / addressing (Task 2) ✓; `AssetId` + resolver seam scaffold, path-backed (Tasks 1, 5) ✓; project discovery/open (Task 4) ✓. **Deferred to Slice 1b (host wiring):** editor/runtime actually opening a project instead of `data/`-next-to-exe — called out in the scope note, its own plan. **Deferred to later slices per spec §11:** `Arcane::Guid`/AssetRegistry (Slice 2), layered config (Slice 3), `.arcplugin`/`plugin://` mounts (Slice 4), engine-as-SDK (Slice 5).
- **Placeholder scan:** no TBD/TODO/"add error handling"; every code step shows complete code; every run step shows the exact command + expected result. Two `> Note:` blocks flag *verifiable-at-implementation* details (the exact `ARC_WARN` include path; whether a named ABI constant exists) with an explicit fallback each — not placeholders.
- **Type consistency:** `AssetId::FromMountPath`/`Key`/`IsValid`, `MountTable::Mount`/`HasMount`/`Resolve`, `ProjectManifest::FromJson`/`LoadFile` + fields, `Project::Open`/`Create`/`Manifest`/`Root`/`Mounts`/`ResolveAsset` are used identically across tasks and match the header declarations. `engineAbi` (field) vs `engine.abi` (JSON key) is intentional and consistent.

---

## Execution Handoff

Follow the writing-plans skill's handoff: offer subagent-driven vs inline execution once the plan is approved.
