# Arcane Editor Scene Authoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make scenes first-class `.arcscene` assets so the Arcane Editor can save what you build, and so a project remembers which scene to open.

**Architecture:** A new engine-side file layer (`SceneAsset`) wraps the existing in-memory `Scene::SaveJson`/`LoadJson` with a Guid, version validation, and a read-then-apply split that makes "validate before destroying the current scene" structural rather than a convention. The editor owns a pure `SceneSession` (path, Guid, dirty, confirm machine) modelled on `DocumentHost`; the scene is the *session*, not a `DocumentHost` document, because the Viewport is already the scene view. Dirty tracking rides a new `CommandStack::StateId()`.

**Tech Stack:** C++23, Astra ECS, nlohmann/json 3.12.0, Catch2, ImGui (docking), premake5.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-26-arcane-editor-scene-authoring-design.md`. Every decision there is locked; do not re-open.
- **Extension is `.arcscene`.** Never `.ascene`, never `.arcscn`.
- **Exception-free engine code.** Return `false`/`std::nullopt` with a written reason; never let an exception escape. `nlohmann::json` calls must be wrapped in `try`/`catch (const nlohmann::json::exception&)`.
- **UTF-8 without BOM, ASCII-only comments.** No `/fp:fast`.
- **Data wins:** loading a scene replaces the registry. Never merge, never marker-scope.
- **Validate before mutating.** A failed load leaves the caller's registry and the editor session exactly as they were.
- **`ArcaneTests` does NOT glob editor sources.** `Arcane/premake5.lua` lists each `ArcaneEditor/src/*.cpp` explicitly (lines ~549-583). A new editor `.cpp` that a test drives MUST be added there, followed by `GenerateProjects.bat`. The `Arcane` DLL and `ArcaneEditor` projects DO glob `src/**`, and `ArcaneTests` globs `Tests/src/**`, so new files in those three places need no premake edit.
- **`ArcaneTests` runs in random order under a time-based seed.** One green run is a sample, not a proof. Capture the "Randomness seeded to: N" banner and re-run under at least two fixed seeds (`--rng-seed 6`, `--rng-seed 17`) before calling a gate green.
- **Never write a bare `Arcane::Runtime rt;` in a test.** It steals `Arcane.dll`'s TypeContext slot and `Edit::` operations then silently report zero changes. Always `Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);`.
- **Run the gate from the exe directory:** `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "~[gpu]"`.
- Build: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`.

## File Structure

**Engine (`Arcane.dll`) — reusable by Loom, the editor, and a future game runtime:**

| File | Responsibility |
|---|---|
| `Arcane/Arcane/src/Arcane/Serialization/SceneAsset.hpp` (create) | `.arcscene` constants, `SceneDocument`, `ReadSceneFile` / `ApplySceneDocument` / `SaveSceneFile`, `CreateEmpty`. Header-only, matching `SceneSerializer.hpp`. |
| `Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp` (modify ~line 155) | Add `.arcscene` to the native-JSON extension list. |
| `Arcane/Arcane/src/Arcane/Project/Project.hpp` / `.cpp` (modify) | `SetBootScene(const Guid&)` — manifest mutate + atomic rewrite. |
| `Arcane/Arcane/src/Arcane/Edit/CommandStack.hpp` / `.cpp` (modify) | `StateId()` + an id on `Transaction`. |
| `Arcane/Loom/src/ProjectBoot.hpp` (modify) | `HostBoot::BootScene`. |

**Editor (`ArcaneEditor`):**

| File | Responsibility |
|---|---|
| `Arcane/ArcaneEditor/src/SceneSession.hpp` / `.cpp` (create) | Pure session state: path, Guid, dirty, the unsaved-changes confirm machine. No ImGui. |
| `Arcane/ArcaneEditor/src/EditorPanels.hpp` / `.cpp` (modify) | `MenuRequests` gains four scene entries; menu items wired; Save gated during Play. |
| `Arcane/ArcaneEditor/src/EditorApp.hpp` / `.cpp` (modify) | Dialogs, thunks, top-of-frame handling, the confirm modal, project-switch and exit guards. |
| `Arcane/ArcaneEditor/src/AssetBrowser.hpp` / `.cpp` (modify) | `AssetKind::Scene`, `.arcscene` classification, open-scene and set-boot-scene actions. |

**Tests (all headless, no `[gpu]`):** `SceneAssetTest.cpp`, `EditorSceneSessionTest.cpp` (create); `AssetBrowserTest.cpp`, `ProjectManifestTest.cpp`, `CommandStackTest.cpp` (extend).

**Content:** `Arcane/SampleProject/Content/scenes/main.arcscene` (create), `SampleProject.arcproj` (modify).

---

### Task 1: The scene file layer

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Serialization/SceneAsset.hpp`
- Test: `Arcane/Tests/src/SceneAssetTest.cpp`

**Interfaces:**
- Consumes: `Arcane::Scene::SaveJson(const Astra::Registry&) -> nlohmann::json` and `Arcane::Scene::LoadJson(Astra::Registry&, const nlohmann::json&) -> bool` from `SceneSerializer.hpp`; `Arcane::Guid` from `<Arcane/Guid.hpp>`; `Arcane::SceneRoot` from `SceneResources.hpp`.
- Produces:
  - `constexpr const char* Arcane::Scene::kSceneExt = ".arcscene"`
  - `struct Arcane::Scene::SceneDocument { Arcane::Guid id; nlohmann::json doc; }`
  - `std::optional<SceneDocument> Arcane::Scene::ReadSceneFile(const std::filesystem::path&, std::string* error)`
  - `bool Arcane::Scene::ApplySceneDocument(const SceneDocument&, Astra::Registry&)`
  - `bool Arcane::Scene::SaveSceneFile(const std::filesystem::path&, const Astra::Registry&, const Arcane::Guid&, std::string* error)`
  - `Astra::Entity Arcane::Scene::CreateEmpty(Astra::Registry&)`

The read/apply split is the point: `ReadSceneFile` touches no registry, so a caller physically cannot destroy the current scene before knowing the file is good.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/SceneAssetTest.cpp`:

```cpp
// SceneAsset: the .arcscene FILE layer over Scene::SaveJson/LoadJson -- id +
// version validation, and the read-then-apply split that lets a caller validate
// a file before destroying the scene it already has.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

namespace
{
    // A registry with the scene roster registered and a two-entity scene:
    // root "Root" at (100, 0) with child "Child" at (5, 7).
    struct Fixture
    {
        std::shared_ptr<Astra::ComponentRegistry> components =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Astra::Entity   root{};
        Astra::Entity   child{};

        Fixture()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            Arcane::Transform rt; rt.position = glm::vec2(100.0f, 0.0f);
            reg.AddComponent<Arcane::Transform>(root, rt);
            reg.AddComponent<Arcane::EntityInfo>(root, Arcane::EntityInfo{Arcane::Guid::Generate(), "Root"});

            child = reg.CreateEntity();
            Arcane::Transform ct; ct.position = glm::vec2(5.0f, 7.0f);
            reg.AddComponent<Arcane::Transform>(child, ct);
            reg.AddComponent<Arcane::EntityInfo>(child, Arcane::EntityInfo{Arcane::Guid::Generate(), "Child"});
            reg.SetParent(child, root);

            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        }
    };

    std::filesystem::path TempDir(const char* leaf)
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / leaf;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
}

TEST_CASE("a scene round-trips through a file, preserving its id", "[scene][json]")
{
    const std::filesystem::path dir  = TempDir("arcane_scene_asset_roundtrip");
    const std::filesystem::path file = dir / ("level" + std::string(Arcane::Scene::kSceneExt));
    const Arcane::Guid id = Arcane::Guid::Generate();

    {
        Fixture f;
        std::string err;
        REQUIRE(Arcane::Scene::SaveSceneFile(file, f.reg, id, &err));
        CHECK(err.empty());
    }
    REQUIRE(std::filesystem::exists(file));

    std::string err;
    const auto read = Arcane::Scene::ReadSceneFile(file, &err);
    REQUIRE(read.has_value());
    CHECK(err.empty());
    CHECK(read->id == id);

    // Apply into a FRESH registry -- the load path a scene open takes.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry fresh{components};
    Arcane::RegisterSceneComponents(fresh);
    REQUIRE(Arcane::Scene::ApplySceneDocument(*read, fresh));

    const Arcane::SceneRoot* sr = fresh.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    const Arcane::Transform* rootT = fresh.GetComponent<Arcane::Transform>(sr->entity);
    REQUIRE(rootT != nullptr);
    CHECK(rootT->position.x == 100.0f);

    const auto kids = fresh.GetChildren(sr->entity);
    REQUIRE(kids.size() == 1);
    const Arcane::EntityInfo* kidInfo = fresh.GetComponent<Arcane::EntityInfo>(kids[0]);
    REQUIRE(kidInfo != nullptr);
    CHECK(kidInfo->name == "Child");
}

TEST_CASE("a file the reader rejects leaves the target registry untouched", "[scene][json]")
{
    // THE ordering guarantee: Open Scene must not empty the editor on a bad file.
    const std::filesystem::path dir = TempDir("arcane_scene_asset_reject");

    SECTION("missing file")
    {
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(dir / "nope.arcscene", &err).has_value());
        CHECK_FALSE(err.empty());
    }
    SECTION("not JSON")
    {
        const std::filesystem::path f = dir / "bad.arcscene";
        std::ofstream(f) << "this is not json";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(f, &err).has_value());
        CHECK_FALSE(err.empty());
    }
    SECTION("wrong schema version")
    {
        const std::filesystem::path f = dir / "old.arcscene";
        std::ofstream(f) << R"({"id":"00000000-0000-0000-0000-000000000001","version":1,"entities":[]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(f, &err).has_value());
        CHECK(err.find("version") != std::string::npos);
    }
    SECTION("malformed id")
    {
        const std::filesystem::path f = dir / "badid.arcscene";
        std::ofstream(f) << R"({"id":"not-a-guid","version":2,"entities":[]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(f, &err).has_value());
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("CreateEmpty yields a saveable one-entity scene", "[scene][json]")
{
    // SaveJson walks the SceneRoot subtree and returns an EMPTY document when the
    // resource is absent, so New Scene has to establish a root or the first save
    // silently writes nothing.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity root = Arcane::Scene::CreateEmpty(reg);
    CHECK(root.IsValid());

    const Arcane::SceneRoot* sr = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    CHECK(sr->entity == root);
    CHECK(reg.GetComponent<Arcane::Transform>(root) != nullptr);
    const Arcane::EntityInfo* info = reg.GetComponent<Arcane::EntityInfo>(root);
    REQUIRE(info != nullptr);
    CHECK(info->name == "Scene");
    CHECK(info->id.IsValid());

    const nlohmann::json doc = Arcane::Scene::SaveJson(reg);
    REQUIRE(doc.contains("entities"));
    CHECK(doc["entities"].size() == 1);
}

TEST_CASE("SaveSceneFile reports an unwritable path instead of throwing", "[scene][json]")
{
    Fixture f;
    std::string err;
    // A directory that does not exist -- SaveSceneFile does not create parents.
    const std::filesystem::path bad =
        std::filesystem::temp_directory_path() / "arcane_no_such_dir_xyz" / "s.arcscene";
    CHECK_FALSE(Arcane::Scene::SaveSceneFile(bad, f.reg, Arcane::Guid::Generate(), &err));
    CHECK_FALSE(err.empty());
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `Arcane/Serialization/SceneAsset.hpp` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Arcane/Arcane/src/Arcane/Serialization/SceneAsset.hpp`:

```cpp
#pragma once

// SceneAsset: the .arcscene FILE layer over SceneSerializer's in-memory
// SaveJson/LoadJson. On disk a scene is a native JSON asset -- a top-level "id"
// (the Guid AssetRegistry mints and resolves by) wrapped around the same
// {version, entities} document SaveJson already produces.
//
// The READ and the APPLY are deliberately separate calls. Every caller must
// validate a file BEFORE destroying the scene it already has: a failed Open
// Scene must leave the editor exactly as it was, not drop the user into an
// empty registry with an error. Splitting the API makes that structural rather
// than a rule someone has to remember.
//
// Exception-free: every failure returns nullopt/false and writes a reason to
// the caller's `error` string.

#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace Arcane::Scene
{
    // Consistent with .arcmat. NOT ".ascene" -- the example in
    // ProjectManifest.hpp predates this and is stale.
    inline constexpr const char* kSceneExt = ".arcscene";

    // A validated scene file, parsed but NOT yet applied to any registry.
    struct SceneDocument
    {
        Arcane::Guid   id;
        nlohmann::json doc;   // { "version", "entities" } -- LoadJson's input
    };

    namespace Detail
    {
        inline void SetError(std::string* error, std::string msg)
        {
            if (error) *error = std::move(msg);
        }
    }

    // Read + validate a .arcscene. Touches no registry, so a rejection costs the
    // caller nothing. nullopt on IO failure, parse failure, a missing/malformed
    // id, or a schema version this build does not speak.
    inline std::optional<SceneDocument> ReadSceneFile(const std::filesystem::path& file,
                                                      std::string* error)
    {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec))
        {
            Detail::SetError(error, "no such file: " + file.generic_string());
            return std::nullopt;
        }

        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            Detail::SetError(error, "could not open " + file.generic_string());
            return std::nullopt;
        }

        try
        {
            nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/true,
                                                       /*ignore_comments*/false);
            if (!doc.is_object())
            {
                Detail::SetError(error, file.generic_string() + " is not a JSON object");
                return std::nullopt;
            }

            // Version FIRST: a v1 file parsed as v2 is the failure mode worth
            // naming precisely, and LoadJson would only tell us "false".
            const auto vit = doc.find("version");
            if (vit == doc.end() || !vit->is_number_integer())
            {
                Detail::SetError(error, file.generic_string() + " has no schema version");
                return std::nullopt;
            }
            if (vit->get<int>() != kSceneJsonVersion)
            {
                Detail::SetError(error, file.generic_string() + " is scene schema version " +
                                        std::to_string(vit->get<int>()) + "; this engine reads " +
                                        std::to_string(kSceneJsonVersion));
                return std::nullopt;
            }

            const auto iit = doc.find("id");
            if (iit == doc.end() || !iit->is_string())
            {
                Detail::SetError(error, file.generic_string() + " has no asset id");
                return std::nullopt;
            }
            const std::optional<Arcane::Guid> id = Arcane::Guid::FromString(iit->get<std::string>());
            if (!id || !id->IsValid())
            {
                Detail::SetError(error, file.generic_string() + " has a malformed asset id");
                return std::nullopt;
            }

            if (!doc.contains("entities") || !doc["entities"].is_array())
            {
                Detail::SetError(error, file.generic_string() + " has no entity list");
                return std::nullopt;
            }

            return SceneDocument{*id, std::move(doc)};
        }
        catch (const nlohmann::json::exception& e)
        {
            Detail::SetError(error, std::string("could not parse ") + file.generic_string() +
                                    ": " + e.what());
            return std::nullopt;
        }
    }

    // Populate `reg` from an already-validated document. The caller is expected
    // to have emptied the registry first (Runtime::ResetRegistry) -- this does
    // not clear, because clearing is the caller's decision and its timing is the
    // whole point of the read/apply split.
    inline bool ApplySceneDocument(const SceneDocument& scene, Astra::Registry& reg)
    {
        return LoadJson(reg, scene.doc);
    }

    // Serialize `reg`'s SceneRoot subtree to `file`, stamping `id` as the
    // top-level asset id. The parent directory must already exist.
    inline bool SaveSceneFile(const std::filesystem::path& file, const Astra::Registry& reg,
                              const Arcane::Guid& id, std::string* error)
    {
        if (!id.IsValid())
        {
            Detail::SetError(error, "refusing to save a scene with no asset id");
            return false;
        }
        try
        {
            nlohmann::json doc = SaveJson(reg);
            // Inserted after SaveJson so the id survives even though SaveJson
            // owns the rest of the document's shape.
            doc["id"] = id.ToString();

            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                Detail::SetError(error, "could not open " + file.generic_string() + " for writing");
                return false;
            }
            out << doc.dump(2);
            if (!out)
            {
                Detail::SetError(error, "could not write " + file.generic_string());
                return false;
            }
            return true;
        }
        catch (const nlohmann::json::exception& e)
        {
            Detail::SetError(error, std::string("could not serialize the scene: ") + e.what());
            return false;
        }
    }

    // The New Scene registry shape: one root entity carrying Transform +
    // EntityInfo, published as the SceneRoot resource.
    //
    // Not optional. SaveJson walks the SceneRoot subtree and returns an EMPTY
    // document when the resource is absent, so a New Scene without this would
    // save nothing at all and report success.
    inline Astra::Entity CreateEmpty(Astra::Registry& reg)
    {
        const Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.AddComponent<Arcane::EntityInfo>(root,
                                             Arcane::EntityInfo{Arcane::Guid::Generate(), "Scene"});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        return root;
    }
}
```

- [ ] **Step 4: Run to verify it passes**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[scene]"
```
Expected: all `[scene]` cases PASS, including the four pre-existing `SceneJsonTest` cases.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Serialization/SceneAsset.hpp Arcane/Tests/src/SceneAssetTest.cpp
git commit -m "feat(arcane): .arcscene file layer -- read/apply split, id + version validation"
```

---

### Task 2: AssetRegistry learns `.arcscene`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp:155`
- Test: `Arcane/Tests/src/AssetRegistryTest.cpp` (create if absent) or extend `Arcane/Tests/src/AssetBrowserTest.cpp`

**Interfaces:**
- Consumes: `Arcane::Scene::kSceneExt` (Task 1).
- Produces: nothing new — `.arcscene` files now get a minted, written-back Guid on `ScanContent`/`AddFile` exactly like `.arcmat`.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/AssetBrowserTest.cpp`:

```cpp
TEST_CASE("a .arcscene is a native JSON asset and gets a minted id", "[editor][project]")
{
    // Native-JSON rule: a top-level "id" is read, or minted and written back.
    // Without .arcscene on that list a scene would scan as an untracked file,
    // never get a Guid, and never appear in the browser or resolve as bootScene.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_scene_asset_scan";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "scenes", ec);

    std::ofstream(dir / "scenes" / "main.arcscene")
        << R"({"version":2,"entities":[]})";   // no id -- must be minted

    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "game");
    CHECK(n == 1);

    const auto all = reg.All();
    REQUIRE(all.size() == 1);
    CHECK(all[0].first.IsValid());
    CHECK(all[0].second == "game://scenes/main.arcscene");

    // Written back: a second scan resolves to the SAME Guid.
    Arcane::AssetRegistry again;
    again.ScanContent(dir, "game");
    const auto all2 = again.All();
    REQUIRE(all2.size() == 1);
    CHECK(all2[0].first == all[0].first);

    fs::remove_all(dir, ec);
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "a .arcscene is a native JSON asset and gets a minted id"
```
Expected: FAIL — `n == 0`, because the extension is not on the native-JSON list.

- [ ] **Step 3: Write the implementation**

In `Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp`, find:

```cpp
        if (ext == ".json" || ext == ".arcmat")
```

Replace with:

```cpp
        // Native JSON assets embed their own "id" (minted + written back when
        // missing). .arcscene joins .arcmat here: a scene is authored content
        // that must be referenceable by Guid -- ProjectManifest::bootScene
        // stores one, and the asset browser lists what this registry knows.
        if (ext == ".json" || ext == ".arcmat" || ext == ".arcscene")
```

- [ ] **Step 4: Run to verify it passes**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[project]"
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp Arcane/Tests/src/AssetBrowserTest.cpp
git commit -m "feat(arcane): .arcscene scans as a native JSON asset"
```

---

### Task 3: `Project::SetBootScene`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Project/Project.hpp`, `Arcane/Arcane/src/Arcane/Project/Project.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Project/ProjectManifest.hpp:32` (comment only)
- Test: `Arcane/Tests/src/ProjectManifestTest.cpp`

**Interfaces:**
- Consumes: `Arcane::Guid`, `Arcane::ProjectManifest`.
- Produces: `bool Arcane::Project::SetBootScene(const Arcane::Guid& id)` — mutates the in-memory manifest and rewrites the `.arcproj`. `false` on IO/parse failure. A nil Guid clears the field.

`bootScene` becomes the Guid **text**, not a mount path — the asset can move on disk and the reference survives. The rewrite parses as `nlohmann::ordered_json`, not `nlohmann::json`: the default type is key-sorted, so a read-modify-write would alphabetise the whole manifest for a one-field edit. (Same reason the Hub's Rust side builds `serde_json` with `preserve_order`.)

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/ProjectManifestTest.cpp`:

```cpp
TEST_CASE("SetBootScene rewrites only that field, preserving key order", "[project]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_set_boot_scene";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);

    // Hand-written so key ORDER is known and an unknown field is present.
    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","description":"d","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":"","zzzFuture":42})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK(proj->Manifest().bootScene.empty());

    const Arcane::Guid id = Arcane::Guid::Generate();
    REQUIRE(proj->SetBootScene(id));
    CHECK(proj->Manifest().bootScene == id.ToString());

    // Re-open from disk: the write actually landed.
    auto again = Arcane::Project::Open(dir);
    REQUIRE(again.has_value());
    CHECK(again->Manifest().bootScene == id.ToString());

    // Unknown keys and their ORDER survive -- a project may carry fields a newer
    // engine added, and pointing at a scene must not reorder or drop them.
    std::ifstream in(dir / "P.arcproj");
    const nlohmann::ordered_json doc = nlohmann::ordered_json::parse(in);
    CHECK(doc["zzzFuture"] == 42);
    std::vector<std::string> keys;
    for (auto it = doc.begin(); it != doc.end(); ++it) keys.push_back(it.key());
    CHECK(keys == std::vector<std::string>{"formatVersion", "name", "description", "engine",
                                           "gameModule", "plugins", "bootScene", "zzzFuture"});

    // A nil Guid clears the field rather than writing "00000000-...".
    REQUIRE(proj->SetBootScene(Arcane::Guid{}));
    CHECK(proj->Manifest().bootScene.empty());

    fs::remove_all(dir, ec);
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `Project` has no member `SetBootScene`.

- [ ] **Step 3: Write the implementation**

In `Arcane/Arcane/src/Arcane/Project/Project.hpp`, add to the public section after `RegisterAsset`:

```cpp
        // Point this project's bootScene at `id` (nil clears it), rewriting the
        // .arcproj in place. False on read/parse/write failure, leaving both the
        // file and the in-memory manifest untouched.
        //
        // The Guid, NOT a mount path: the AssetRegistry rebuilds its Guid ->
        // mount-path map on every open, so a scene that moves on disk keeps
        // working. Written atomically (temp + rename) so an interrupted write
        // cannot leave a project with a truncated manifest.
        bool SetBootScene(const Guid& id);
```

In `Arcane/Arcane/src/Arcane/Project/Project.cpp`, add `#include <fstream>` if absent and append:

```cpp
    bool Project::SetBootScene(const Guid& id)
    {
        const std::filesystem::path file = m_manifestFile;
        if (file.empty())
        {
            ARC_ERROR("SetBootScene: this project has no manifest file on disk");
            return false;
        }

        // ordered_json, NOT json: the default type is key-sorted, so a
        // read-modify-write would hand back a manifest alphabetised top to
        // bottom for a one-field edit -- a diff nobody asked for in a file that
        // is very likely in git.
        nlohmann::ordered_json doc;
        try
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                ARC_ERROR("SetBootScene: could not read {}", file.generic_string());
                return false;
            }
            doc = nlohmann::ordered_json::parse(in);
            if (!doc.is_object())
            {
                ARC_ERROR("SetBootScene: {} is not a JSON object", file.generic_string());
                return false;
            }
        }
        catch (const nlohmann::json::exception& e)
        {
            ARC_ERROR("SetBootScene: could not parse {}: {}", file.generic_string(), e.what());
            return false;
        }

        const std::string value = id.IsValid() ? id.ToString() : std::string{};
        doc["bootScene"] = value;

        // Temp + rename: a half-written .arcproj is a project that will not open.
        const std::filesystem::path tmp = file.string() + ".tmp";
        try
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                ARC_ERROR("SetBootScene: could not open {} for writing", tmp.generic_string());
                return false;
            }
            out << doc.dump(2);
            if (!out)
            {
                ARC_ERROR("SetBootScene: could not write {}", tmp.generic_string());
                return false;
            }
        }
        catch (const nlohmann::json::exception& e)
        {
            ARC_ERROR("SetBootScene: could not serialize {}: {}", file.generic_string(), e.what());
            return false;
        }

        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec);
            ARC_ERROR("SetBootScene: could not replace {}", file.generic_string());
            return false;
        }

        m_manifest.bootScene = value;
        return true;
    }
```

If `Project` does not already retain the manifest file path, add `std::filesystem::path m_manifestFile;` to its private members and assign it in `Project::Open` where the manifest is located. Verify with `grep -n "m_manifestFile\|m_manifest" Arcane/Arcane/src/Arcane/Project/Project.hpp` before writing, and follow whatever member already holds it.

Then fix the stale comment at `ProjectManifest.hpp:32`:

```cpp
        std::string            bootScene;           // asset Guid text (see Project::SetBootScene); empty = none
```

- [ ] **Step 4: Run to verify it passes**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[project]"
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Project/ Arcane/Tests/src/ProjectManifestTest.cpp
git commit -m "feat(arcane): Project::SetBootScene -- bootScene is a Guid, written order-preserving and atomically"
```

---

### Task 4: `CommandStack::StateId`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Edit/CommandStack.hpp` (add `id` to `Transaction` at line ~103; declare `StateId`), `Arcane/Arcane/src/Arcane/Edit/CommandStack.cpp`
- Test: `Arcane/Tests/src/CommandStackTest.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] std::uint64_t Arcane::CommandStack::StateId() const noexcept` — the id of the transaction on top of the undo stack, `0` when empty.

Undoing back to a recorded id makes that id current again, so a session that undoes to its save point goes clean. A plain change-counter cannot express that.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/CommandStackTest.cpp`:

```cpp
TEST_CASE("StateId identifies the current state, not the number of edits", "[edit]")
{
    // This is what scene dirty-tracking is built on: SceneSession records
    // StateId() at save and compares. Undo back to the save point has to go
    // CLEAN again, which a monotonic edit counter cannot express.
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);

    Arcane::CommandStack stack([&runtime]() -> Astra::Registry& { return runtime.Registry(); });

    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    const Astra::ComponentDescriptor* desc =
        reg.GetComponentRegistry()->GetDescriptor(Astra::TypeId<Arcane::Transform>::Hash());
    REQUIRE(desc != nullptr);

    CHECK(stack.StateId() == 0);   // empty stack

    auto edit = [&](float x)
    {
        const Arcane::TransactionId t = stack.Begin("Move");
        stack.SnapshotComponent(e, desc);
        reg.GetComponent<Arcane::Transform>(e)->position.x = x;
        stack.Commit(t);
    };

    edit(1.0f);
    const std::uint64_t afterFirst = stack.StateId();
    CHECK(afterFirst != 0);

    edit(2.0f);
    const std::uint64_t afterSecond = stack.StateId();
    CHECK(afterSecond != afterFirst);

    stack.Undo();
    CHECK(stack.StateId() == afterFirst);   // back at the first state EXACTLY

    stack.Redo();
    CHECK(stack.StateId() == afterSecond);

    stack.Undo();
    stack.Undo();
    CHECK(stack.StateId() == 0);            // back to empty

    // A NEW edit after undoing must not re-mint a retired id -- otherwise a
    // saved marker could match a state that is not the saved one.
    edit(3.0f);
    CHECK(stack.StateId() != afterFirst);
    CHECK(stack.StateId() != afterSecond);

    stack.Clear();
    CHECK(stack.StateId() == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — no member `StateId`.

- [ ] **Step 3: Write the implementation**

In `CommandStack.hpp`, extend the private `Transaction` struct:

```cpp
        struct Transaction
        {
            std::string label;
            std::vector<std::unique_ptr<ICommand>> commands;
            // Identifies the STATE this transaction produced. Stamped from
            // m_nextId (the same monotonic generator TransactionId uses), so an
            // id is never re-minted and a retired state can never be mistaken
            // for a live one.
            std::uint64_t id = 0;
        };
```

Add the public accessor next to `CanUndo`/`CanRedo`:

```cpp
        // Identifies the CURRENT state: the id of the transaction on top of the
        // undo stack, 0 when the stack is empty.
        //
        // Exists so a caller can record "the state I saved" and later ask
        // whether anything has changed since. Undoing back to that state
        // restores its id, so undo-to-the-save-point reads as clean -- which a
        // simple change counter gets wrong. If the recorded transaction is
        // evicted by the depth cap its id becomes unreachable and the caller
        // stays dirty; that is the safe direction, and the same caveat Qt
        // documents for QUndoStack's clean state.
        [[nodiscard]] std::uint64_t StateId() const noexcept
        {
            return m_undo.empty() ? 0u : m_undo.back().id;
        }
```

In `CommandStack.cpp`, find where a completed `Transaction` is pushed onto `m_undo` inside `Commit` and stamp it before the push. The transaction is built from `m_openLabel` + `m_pending`/`m_pendingGeneric`; stamp with a fresh value from the same generator:

```cpp
        // Stamp the state this transaction produced. m_nextId is the same
        // monotonic source TransactionId::Begin draws from, so ids are unique
        // across BOTH uses and a committed state id can never collide with a
        // live transaction token.
        txn.id = m_nextId++;
        m_undo.push_back(std::move(txn));
```

Apply the same stamping in `Push` (the one-shot command path), which also appends to `m_undo`. Both sites already exist near the `while (m_undo.size() > m_maxDepth)` eviction loops at `CommandStack.cpp:66` and `:93`; stamp immediately before each corresponding push.

`Undo`/`Redo` move whole `Transaction` values between the deques, so ids travel with them and need no extra handling.

- [ ] **Step 4: Run to verify it passes**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[edit]"
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Edit/CommandStack.hpp Arcane/Arcane/src/Arcane/Edit/CommandStack.cpp Arcane/Tests/src/CommandStackTest.cpp
git commit -m "feat(arcane): CommandStack::StateId -- identify the current state so undo-to-save reads clean"
```

---

### Task 5: `SceneSession`

**Files:**
- Create: `Arcane/ArcaneEditor/src/SceneSession.hpp`, `Arcane/ArcaneEditor/src/SceneSession.cpp`
- Modify: `Arcane/premake5.lua` (add `SceneSession.cpp` to the `ArcaneTests` files list)
- Test: `Arcane/Tests/src/EditorSceneSessionTest.cpp`

**Interfaces:**
- Consumes: `Arcane::CommandStack::StateId()` (Task 4).
- Produces:
  - `enum class Arcane::Editor::SceneIntent { None, NewScene, OpenScene, OpenProject, Exit }`
  - `class Arcane::Editor::SceneSession` with:
    - `const std::filesystem::path& Path() const noexcept` (empty = never saved)
    - `const Arcane::Guid& Id() const noexcept`
    - `std::string DisplayName() const` ("Untitled" when never saved, else the stem)
    - `bool IsDirty(const Arcane::CommandStack&) const noexcept`
    - `void MarkSaved(const Arcane::CommandStack&) noexcept`
    - `void Adopt(std::filesystem::path, Arcane::Guid, const Arcane::CommandStack&)`
    - `void Reset(const Arcane::CommandStack&)`
    - `bool Request(SceneIntent, std::filesystem::path payload, const Arcane::CommandStack&)`
    - `SceneIntent Pending() const noexcept`
    - `const std::filesystem::path& PendingPath() const noexcept`
    - `void ClearPending() noexcept`

`Request` returns `true` when the caller may act immediately (nothing dirty). `false` parks the intent for the confirm modal; the host reads `Pending()`/`PendingPath()`, acts, then calls `ClearPending()`. Cancel is `ClearPending()` without acting. This mirrors `DocumentHost`'s one-pending-at-a-time close flow rather than inventing a second vocabulary.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/EditorSceneSessionTest.cpp`:

```cpp
// SceneSession: the editor's scene identity + dirty state + the unsaved-changes
// confirm machine. Entirely pure -- no ImGui, no filesystem -- so the whole
// state machine is driven headlessly here.

#include <catch2/catch_test_macros.hpp>

#include "SceneSession.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

using namespace Arcane::Editor;

namespace
{
    // A real CommandStack over a real Runtime -- StateId is what dirty rides on,
    // and a fake would not exercise the undo/redo id restoration that matters.
    struct Harness
    {
        Arcane::Runtime runtime{&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false};
        Arcane::CommandStack stack{[this]() -> Astra::Registry& { return runtime.Registry(); }};
        Astra::Entity entity{};
        const Astra::ComponentDescriptor* desc = nullptr;

        Harness()
        {
            Astra::Registry& reg = runtime.Registry();
            Arcane::RegisterSceneComponents(reg);
            entity = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(entity, Arcane::Transform{});
            desc = reg.GetComponentRegistry()->GetDescriptor(
                Astra::TypeId<Arcane::Transform>::Hash());
        }

        void Edit(float x)
        {
            const Arcane::TransactionId t = stack.Begin("Move");
            stack.SnapshotComponent(entity, desc);
            runtime.Registry().GetComponent<Arcane::Transform>(entity)->position.x = x;
            stack.Commit(t);
        }
    };
}

TEST_CASE("a fresh session is Untitled and clean", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    CHECK(s.Path().empty());
    CHECK_FALSE(s.Id().IsValid());
    CHECK(s.DisplayName() == "Untitled");
    CHECK_FALSE(s.IsDirty(h.stack));
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("editing dirties the session and saving cleans it", "[editor][scene]")
{
    Harness h;
    SceneSession s;

    h.Edit(1.0f);
    CHECK(s.IsDirty(h.stack));

    s.MarkSaved(h.stack);
    CHECK_FALSE(s.IsDirty(h.stack));

    h.Edit(2.0f);
    CHECK(s.IsDirty(h.stack));
}

TEST_CASE("undoing back to the save point goes clean again", "[editor][scene]")
{
    // The reason dirty rides StateId rather than an edit counter.
    Harness h;
    SceneSession s;

    h.Edit(1.0f);
    s.MarkSaved(h.stack);
    h.Edit(2.0f);
    CHECK(s.IsDirty(h.stack));

    h.stack.Undo();
    CHECK_FALSE(s.IsDirty(h.stack));

    h.stack.Redo();
    CHECK(s.IsDirty(h.stack));
}

TEST_CASE("Adopt retargets path and id and marks clean", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    CHECK(s.IsDirty(h.stack));

    const Arcane::Guid id = Arcane::Guid::Generate();
    s.Adopt("D:/Games/G/Content/scenes/level_one.arcscene", id, h.stack);

    CHECK(s.Id() == id);
    CHECK(s.DisplayName() == "level_one");
    CHECK_FALSE(s.IsDirty(h.stack));
}

TEST_CASE("Reset returns the session to Untitled and clean", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    s.Adopt("D:/a/b.arcscene", Arcane::Guid::Generate(), h.stack);
    h.Edit(1.0f);

    // New Scene clears the undo stack, which is what the host does around Reset.
    h.stack.Clear();
    s.Reset(h.stack);

    CHECK(s.Path().empty());
    CHECK_FALSE(s.Id().IsValid());
    CHECK(s.DisplayName() == "Untitled");
    CHECK_FALSE(s.IsDirty(h.stack));
}

TEST_CASE("a clean session proceeds immediately; a dirty one parks the intent", "[editor][scene]")
{
    Harness h;
    SceneSession s;

    CHECK(s.Request(SceneIntent::NewScene, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::None);

    h.Edit(1.0f);
    CHECK_FALSE(s.Request(SceneIntent::OpenScene, "D:/a/b.arcscene", h.stack));
    CHECK(s.Pending() == SceneIntent::OpenScene);
    CHECK(s.PendingPath() == std::filesystem::path("D:/a/b.arcscene"));

    // Cancel: the intent is dropped and the session is untouched.
    s.ClearPending();
    CHECK(s.Pending() == SceneIntent::None);
    CHECK(s.IsDirty(h.stack));
}

TEST_CASE("a second request while one is pending is ignored", "[editor][scene]")
{
    // Same rule as DocumentHost's close flow: the modal is app-modal, so a
    // second intent arriving behind it would silently replace what the user is
    // being asked about.
    Harness h;
    SceneSession s;
    h.Edit(1.0f);

    REQUIRE_FALSE(s.Request(SceneIntent::OpenScene, "D:/a/b.arcscene", h.stack));
    CHECK_FALSE(s.Request(SceneIntent::Exit, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::OpenScene);
    CHECK(s.PendingPath() == std::filesystem::path("D:/a/b.arcscene"));
}

TEST_CASE("saving while an intent is pending lets it proceed", "[editor][scene]")
{
    // The Save branch of the confirm modal: the host saves, which marks clean;
    // the parked intent is then performed and cleared.
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    REQUIRE_FALSE(s.Request(SceneIntent::NewScene, {}, h.stack));

    s.MarkSaved(h.stack);
    CHECK_FALSE(s.IsDirty(h.stack));
    CHECK(s.Pending() == SceneIntent::NewScene);   // still parked for the host to act on

    s.ClearPending();
    CHECK(s.Pending() == SceneIntent::None);
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — `SceneSession.hpp` does not exist.

- [ ] **Step 3: Write the implementation**

Create `Arcane/ArcaneEditor/src/SceneSession.hpp`:

```cpp
#pragma once

// SceneSession: which scene the editor is editing, whether it has unsaved
// changes, and the one-at-a-time unsaved-changes confirm machine.
//
// PURE state -- no ImGui, no filesystem access. The host performs every effect
// (dialogs, reading and writing files, resetting the registry); this decides
// WHETHER it may, and remembers what was asked for while the user answers.
// Same split as ConsoleBuffer and DocumentHost, and the reason the whole flow
// is unit-tested headlessly.
//
// The scene is a SESSION, not a DocumentHost document: the Viewport is already
// the scene view, and the Edit-mode registry is already the authored state
// (see PlayMode.hpp). Materials are documents because they are side artifacts.

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>

#include <filesystem>
#include <string>

namespace Arcane::Editor
{
    // What the user asked for that a dirty scene is standing in the way of.
    enum class SceneIntent
    {
        None = 0,
        NewScene,
        OpenScene,     // PendingPath() = the .arcscene to open
        OpenProject,   // PendingPath() = the .arcproj to switch to
        Exit,
    };

    class SceneSession
    {
    public:
        // ---- identity ----------------------------------------------------
        // Empty until the scene has been saved somewhere.
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }
        [[nodiscard]] const Arcane::Guid& Id() const noexcept { return m_id; }
        // "Untitled" for a never-saved scene, else the file stem.
        [[nodiscard]] std::string DisplayName() const;

        // ---- dirty -------------------------------------------------------
        // Compares the stack's CURRENT state against the one recorded at save.
        //
        // ASSUMPTION, and it is load-bearing: the CommandStack is a faithful
        // proxy for authored change. True today -- the RunLoop is paused in Edit
        // mode and every Outliner, Inspector and gizmo edit is bracketed through
        // the stack. Anything that mutates the registry OUTSIDE the stack will
        // not mark the scene dirty, and whoever adds such a path owns fixing
        // this.
        [[nodiscard]] bool IsDirty(const Arcane::CommandStack& stack) const noexcept
        {
            return stack.StateId() != m_savedStateId;
        }
        void MarkSaved(const Arcane::CommandStack& stack) noexcept
        {
            m_savedStateId = stack.StateId();
        }

        // ---- retargeting -------------------------------------------------
        // After a successful save-as or open: adopt the file and go clean.
        void Adopt(std::filesystem::path path, Arcane::Guid id,
                   const Arcane::CommandStack& stack);
        // After New Scene: back to Untitled, no id, clean. The caller clears the
        // undo stack around this (no entity handle survives a registry reset).
        void Reset(const Arcane::CommandStack& stack);

        // ---- confirm flow ------------------------------------------------
        // True  = nothing unsaved, act now.
        // False = the intent is parked; draw the confirm modal, then read
        //         Pending()/PendingPath(), act, and ClearPending().
        // A second request while one is pending is IGNORED and returns false --
        // the modal is app-modal, so replacing what the user is being asked
        // about would resolve their answer against the wrong action.
        bool Request(SceneIntent intent, std::filesystem::path payload,
                     const Arcane::CommandStack& stack);

        [[nodiscard]] SceneIntent Pending() const noexcept { return m_pending; }
        [[nodiscard]] const std::filesystem::path& PendingPath() const noexcept
        {
            return m_pendingPath;
        }
        // Cancel, or acknowledge that the intent has been performed.
        void ClearPending() noexcept;

    private:
        std::filesystem::path m_path;          // empty = never saved
        Arcane::Guid          m_id;            // nil until saved
        std::uint64_t         m_savedStateId = 0;   // 0 == an empty stack, i.e. clean

        SceneIntent           m_pending = SceneIntent::None;
        std::filesystem::path m_pendingPath;
    };
}
```

Create `Arcane/ArcaneEditor/src/SceneSession.cpp`:

```cpp
#include "SceneSession.hpp"

namespace Arcane::Editor
{
    std::string SceneSession::DisplayName() const
    {
        if (m_path.empty()) return "Untitled";
        return m_path.stem().string();
    }

    void SceneSession::Adopt(std::filesystem::path path, Arcane::Guid id,
                             const Arcane::CommandStack& stack)
    {
        m_path = std::move(path);
        m_id   = id;
        MarkSaved(stack);
    }

    void SceneSession::Reset(const Arcane::CommandStack& stack)
    {
        m_path.clear();
        m_id = Arcane::Guid{};
        MarkSaved(stack);
    }

    bool SceneSession::Request(SceneIntent intent, std::filesystem::path payload,
                               const Arcane::CommandStack& stack)
    {
        if (intent == SceneIntent::None) return false;
        if (m_pending != SceneIntent::None) return false;   // one at a time

        if (!IsDirty(stack))
            return true;

        m_pending     = intent;
        m_pendingPath = std::move(payload);
        return false;
    }

    void SceneSession::ClearPending() noexcept
    {
        m_pending = SceneIntent::None;
        m_pendingPath.clear();
    }
}
```

- [ ] **Step 4: Add the source to ArcaneTests and regenerate**

In `Arcane/premake5.lua`, inside the `project "ArcaneTests"` `files { ... }` block, after the `ComponentCatalog.cpp` entry, add:

```lua
        -- Scene authoring: SceneSession (scene identity + dirty state + the
        -- unsaved-changes confirm machine) source-compiles into the test exe so
        -- the [editor] units drive the PURE state machine directly -- there is
        -- no ImGui in it at all, same pattern as DocumentHost above.
        "%{wks.location}/ArcaneEditor/src/SceneSession.cpp",
```

Then:

```
cd Arcane && GenerateProjects.bat
```

- [ ] **Step 5: Run to verify it passes**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[editor][scene]"
```
Expected: all eight `SceneSession` cases PASS.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ArcaneEditor/src/SceneSession.hpp Arcane/ArcaneEditor/src/SceneSession.cpp Arcane/premake5.lua Arcane/Tests/src/EditorSceneSessionTest.cpp
git commit -m "feat(editor): SceneSession -- scene identity, dirty via StateId, unsaved-changes confirm machine"
```

---

### Task 6: Wire the File menu, dialogs, and guards

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (`MenuRequests`, ~line 24), `Arcane/ArcaneEditor/src/EditorPanels.cpp` (menu, lines 61-71)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (session member, pending paths, thunks), `Arcane/ArcaneEditor/src/EditorApp.cpp`

**Interfaces:**
- Consumes: `Arcane::Scene::ReadSceneFile` / `ApplySceneDocument` / `SaveSceneFile` / `CreateEmpty` / `kSceneExt` (Task 1); `Project::SetBootScene` (Task 3); `SceneSession` (Task 5); `Runtime::ResetRegistry()` (`Runtime.hpp:164`).
- Produces: a working New / Open / Save / Save As, and a scene name in the window title.

This task has no new unit test — it is ImGui and host wiring, which this codebase desk-verifies. Its correctness rests on Tasks 1 and 5, which are tested. Do not fake a test for it.

- [ ] **Step 1: Extend `MenuRequests`**

In `Arcane/ArcaneEditor/src/EditorPanels.hpp`, inside `struct MenuRequests`:

```cpp
        bool newScene = false;       // File -> New Scene
        bool openScene = false;      // File -> Open Scene...       (open-file dialog)
        bool saveScene = false;      // File -> Save Scene          (Save As when never saved)
        bool saveSceneAs = false;    // File -> Save Scene As...    (save dialog)
```

Change `DrawDockSpace`'s signature to accept what the menu now needs to render honestly — the session's dirty state and whether Play is active. Find the existing declaration and add two parameters:

```cpp
    // `sceneDirty` puts the * on Save; `playing` greys Save out entirely.
    MenuRequests BeginDockSpace(const Arcane::CommandStack& undo,
                                bool sceneDirty, bool playing);
```

Update the definition in `EditorPanels.cpp` to match, then replace the four dead scene items (lines 63-70) with:

```cpp
                if (ImGui::MenuItem("New Scene", "Ctrl+N")) requests.newScene = true;
                if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) requests.openScene = true;
                ImGui::Separator();
                if (ImGui::MenuItem("New Material...")) requests.newMaterial = true;
                if (ImGui::MenuItem("Open Material...")) requests.openMaterial = true;
                ImGui::Separator();
                // Disabled during Play: the authored scene is the pre-Play
                // snapshot, and the live registry is play-time mutation that
                // PlaySession::Stop exists to discard. Saving it would persist
                // garbage. Same call UE makes greying Save out during PIE.
                if (ImGui::MenuItem(sceneDirty ? "Save Scene *" : "Save Scene",
                                    "Ctrl+S", false, !playing))
                    requests.saveScene = true;
                if (ImGui::MenuItem("Save Scene As...", nullptr, false, !playing))
                    requests.saveSceneAs = true;
                if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Stop play mode to save the scene");
```

- [ ] **Step 2: Add session state to `EditorApp`**

In `Arcane/ArcaneEditor/src/EditorApp.hpp`, add to the private members beside the existing material-dialog stash:

```cpp
        Arcane::Editor::SceneSession m_scene;

        // Same background-thread stash pattern as m_pendingMaterialNewPath: the
        // SDL dialog backend fires its callback from a detached worker, not from
        // PumpEvents, so these are read at the top of the next frame under the
        // mutex.
        std::string m_pendingSceneOpenPath;
        std::string m_pendingSceneSavePath;
        std::mutex  m_pendingSceneMutex;

        static void SceneOpenPickedThunk(const char* path, void* user);
        static void SceneSavePickedThunk(const char* path, void* user);

        // Perform a scene intent that the confirm modal cleared. Returns false
        // when the effect itself failed (the caller surfaces m_sceneError).
        bool DoNewScene();
        bool DoOpenScene(const std::filesystem::path& file);
        bool DoSaveScene(const std::filesystem::path& file);

        std::string m_sceneError;   // last scene failure, drawn as a modal
```

Add `#include "SceneSession.hpp"` to the header's include list.

- [ ] **Step 3: Implement the effects**

In `Arcane/ArcaneEditor/src/EditorApp.cpp`, add `#include <Arcane/Serialization/SceneAsset.hpp>` and the three effect methods:

```cpp
    // Every one of these tears down editor state that references the outgoing
    // scene before touching the registry -- no entity handle survives a
    // ResetRegistry. Mirrors SwitchProject's teardown for the same reason.
    void EditorApp::ClearSceneReferences()
    {
        m_selection.Clear();
        m_outliner = {};
        if (m_undo) m_undo->Clear();
    }

    bool EditorApp::DoNewScene()
    {
        ClearSceneReferences();
        m_runtime->ResetRegistry();
        Arcane::Scene::CreateEmpty(m_runtime->Registry());
        m_scene.Reset(*m_undo);
        ARC_INFO("New scene");
        return true;
    }

    bool EditorApp::DoOpenScene(const std::filesystem::path& file)
    {
        // READ FIRST. A bad file must leave the current scene exactly as it is,
        // not empty the editor and then report a failure -- which is what
        // ResetRegistry-then-load would do.
        std::string err;
        const auto doc = Arcane::Scene::ReadSceneFile(file, &err);
        if (!doc)
        {
            ARC_ERROR("Open Scene: {}", err);
            m_sceneError = err;
            return false;
        }

        ClearSceneReferences();
        m_runtime->ResetRegistry();
        if (!Arcane::Scene::ApplySceneDocument(*doc, m_runtime->Registry()))
        {
            // Validated but unloadable (e.g. a component whose reflection reader
            // latched an unsupported field type). The registry is already empty;
            // give the user a well-formed empty scene rather than a half one.
            Arcane::Scene::CreateEmpty(m_runtime->Registry());
            m_scene.Reset(*m_undo);
            m_sceneError = "'" + file.generic_string() +
                           "' parsed but could not be loaded (see Console).";
            ARC_ERROR("Open Scene: ApplySceneDocument failed for {}", file.generic_string());
            return false;
        }

        m_scene.Adopt(file, doc->id, *m_undo);
        ARC_INFO("Opened scene {}", file.generic_string());
        return true;
    }

    bool EditorApp::DoSaveScene(const std::filesystem::path& file)
    {
        // Reuse the scene's existing id when saving over itself; mint one for a
        // new file so the AssetRegistry has something stable to register.
        const bool sameFile = !m_scene.Path().empty() && m_scene.Path() == file;
        const Arcane::Guid id = (sameFile && m_scene.Id().IsValid())
                              ? m_scene.Id() : Arcane::Guid::Generate();

        std::string err;
        if (!Arcane::Scene::SaveSceneFile(file, m_runtime->Registry(), id, &err))
        {
            ARC_ERROR("Save Scene: {}", err);
            m_sceneError = err;
            return false;
        }

        // Register the new file so it resolves by Guid and shows in the browser.
        // A scene saved outside the project's content root cannot be registered;
        // that is reported, not treated as a save failure -- the bytes are on
        // disk either way.
        if (Arcane::Project* proj = m_runtime->CurrentProjectMutable())
        {
            if (!proj->RegisterAsset(file))
                ARC_WARN("Save Scene: {} is outside the project content root, so it has no "
                         "asset id and cannot be a boot scene", file.generic_string());
        }

        m_scene.Adopt(file, id, *m_undo);
        m_assetBrowser.dirty = true;   // re-enumerate on the next draw
        ARC_INFO("Saved scene {}", file.generic_string());
        return true;
    }
```

`CurrentProjectMutable()` does not exist yet — `Runtime::CurrentProject()` returns `const Project*`. Add a non-const accessor beside it in `Runtime.hpp`/`.cpp`:

```cpp
        // Non-const sibling of CurrentProject, for the editor's save flows:
        // registering a newly written asset and setting the boot scene both
        // mutate the open project.
        Project* CurrentProjectMutable() noexcept;
```

Declare `void ClearSceneReferences();` in `EditorApp.hpp` alongside the three effect methods. If `m_assetBrowser` has no `dirty` flag, use whatever re-scan trigger `AssetBrowserState` already provides — check with `grep -n "dirty\|rescan\|Refresh" Arcane/ArcaneEditor/src/AssetBrowser.hpp` and follow it.

- [ ] **Step 4: Handle the menu requests**

In `EditorApp.cpp`, beside the existing `menuReq.openProject` handling (~line 1307), add:

```cpp
            // Scene dialogs start in the project's Content/scenes -- created on
            // demand, because a project scaffolded before scenes existed has no
            // such folder and the dialog would fall back to the OS default.
            auto sceneDir = [this]() -> std::string
            {
                const Arcane::Project* proj = m_runtime->CurrentProject();
                if (!proj) return {};
                const std::filesystem::path dir = proj->Root() / "Content" / "scenes";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                return dir.string();
            };

            if (menuReq.newScene && m_scene.Request(Arcane::Editor::SceneIntent::NewScene, {}, *m_undo))
                DoNewScene();

            if (menuReq.openScene)
            {
                const std::string dir = sceneDir();
                m_gpu->Win().ShowOpenFileDialog(&EditorApp::SceneOpenPickedThunk, this,
                                                "Arcane Scene", "arcscene",
                                                dir.empty() ? nullptr : dir.c_str());
            }
            // Save with no file yet IS Save As -- otherwise the item would look
            // enabled and do nothing on a brand-new scene.
            if (menuReq.saveScene && !m_scene.Path().empty())
                DoSaveScene(m_scene.Path());
            if (menuReq.saveSceneAs || (menuReq.saveScene && m_scene.Path().empty()))
            {
                const std::string dir = sceneDir();
                m_gpu->Win().ShowSaveFileDialog(&EditorApp::SceneSavePickedThunk, this,
                                                "Arcane Scene", "arcscene",
                                                dir.empty() ? nullptr : dir.c_str());
            }
```

Add the two thunks beside `MaterialNewPickedThunk` (~line 488):

```cpp
    void EditorApp::SceneOpenPickedThunk(const char* path, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingSceneMutex);
        self->m_pendingSceneOpenPath = path;
    }

    void EditorApp::SceneSavePickedThunk(const char* path, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingSceneMutex);
        self->m_pendingSceneSavePath = path;
    }
```

At the top of `MainLoop`, beside the existing material-path swap (~line 726):

```cpp
            std::string sceneOpen, sceneSave;
            {
                std::lock_guard<std::mutex> lk(m_pendingSceneMutex);
                sceneOpen.swap(m_pendingSceneOpenPath);
                sceneSave.swap(m_pendingSceneSavePath);
            }
            if (!sceneOpen.empty())
            {
                // Guarded: opening a scene over unsaved work parks the intent
                // for the confirm modal instead of discarding it.
                if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene, sceneOpen, *m_undo))
                    DoOpenScene(sceneOpen);
            }
            if (!sceneSave.empty())
            {
                std::filesystem::path p = sceneSave;
                // The dialog does not append the extension on every platform
                // backend, and a scene without it will not scan as an asset.
                if (p.extension() != Arcane::Scene::kSceneExt)
                    p += Arcane::Scene::kSceneExt;
                DoSaveScene(p);
            }
```

- [ ] **Step 5: Draw the confirm modal and wire the remaining guards**

Beside the existing project-open failure modal, add the unsaved-changes confirm and the scene-error modal:

```cpp
            if (m_scene.Pending() != Arcane::Editor::SceneIntent::None)
                ImGui::OpenPopup("Unsaved Scene");
            if (ImGui::BeginPopupModal("Unsaved Scene", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(("'" + m_scene.DisplayName() +
                                        "' has unsaved changes.").c_str());
                ImGui::Spacing();
                const auto perform = [this]()
                {
                    const Arcane::Editor::SceneIntent intent = m_scene.Pending();
                    const std::filesystem::path path = m_scene.PendingPath();
                    m_scene.ClearPending();
                    switch (intent)
                    {
                        case Arcane::Editor::SceneIntent::NewScene:    DoNewScene(); break;
                        case Arcane::Editor::SceneIntent::OpenScene:   DoOpenScene(path); break;
                        case Arcane::Editor::SceneIntent::OpenProject: SwitchProject(path); break;
                        case Arcane::Editor::SceneIntent::Exit:        m_running = false; break;
                        case Arcane::Editor::SceneIntent::None:        break;
                    }
                };
                if (ImGui::Button("Save"))
                {
                    // Save As when the scene has never been written: the dialog
                    // resolves next frame, and the intent stays parked until it
                    // does, so cancelling the dialog cancels nothing else.
                    if (m_scene.Path().empty())
                    {
                        const Arcane::Project* proj = m_runtime->CurrentProject();
                        const std::string dir =
                            proj ? (proj->Root() / "Content" / "scenes").string() : std::string();
                        m_gpu->Win().ShowSaveFileDialog(&EditorApp::SceneSavePickedThunk, this,
                                                        "Arcane Scene", "arcscene",
                                                        dir.empty() ? nullptr : dir.c_str());
                    }
                    else if (DoSaveScene(m_scene.Path()))
                    {
                        perform();
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard")) { perform(); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { m_scene.ClearPending(); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }

            if (!m_sceneError.empty())
                ImGui::OpenPopup("Scene Error");
            if (ImGui::BeginPopupModal("Scene Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(m_sceneError.c_str());
                ImGui::Spacing();
                if (ImGui::Button("OK")) { m_sceneError.clear(); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
```

Guard the project switch. In `SwitchProject`, immediately after the existing `m_documents.AnyDirty()` refusal at `EditorApp.cpp:617`, the scene guard belongs at the *call site* instead — routing through `SceneSession::Request` so the user gets Save/Discard/Cancel rather than a refusal. Where `m_pendingProjectPath` is consumed at the top of `MainLoop`, wrap the `SwitchProject` call:

```cpp
            if (!pendingProject.empty())
            {
                if (m_scene.Request(Arcane::Editor::SceneIntent::OpenProject,
                                    pendingProject, *m_undo))
                    SwitchProject(pendingProject);
            }
```

And guard exit: where the SDL quit event sets the loop to stop, route it the same way:

```cpp
                    case SDL_EVENT_QUIT:
                        // Unsaved scene: park the quit behind the confirm modal
                        // rather than dropping the user's work on a window close.
                        if (m_scene.Request(Arcane::Editor::SceneIntent::Exit, {}, *m_undo))
                            m_running = false;
                        break;
```

Finally, pass the new arguments at the `BeginDockSpace` call site:

```cpp
            Arcane::Editor::MenuRequests menuReq =
                Arcane::Editor::BeginDockSpace(*m_undo, m_scene.IsDirty(*m_undo), m_play.IsPlaying());
```

If `EditorApp` uses a different loop-exit member than `m_running`, or a different quit-event site, follow what is already there — verify with `grep -n "SDL_EVENT_QUIT\|m_running" Arcane/ArcaneEditor/src/EditorApp.cpp` first.

- [ ] **Step 6: Build and desk-verify**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "~[gpu]"
```
Expected: the full non-GPU suite still passes.

Then, at the desk (`bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ..\..\..\SampleProject\SampleProject.arcproj`), confirm each of:
1. New Scene on a clean session empties the Outliner to a single "Scene" root.
2. Creating an entity puts `*` on File ▸ Save Scene.
3. Save Scene on a never-saved scene opens the Save As dialog.
4. Saving writes a `.arcscene` under `Content/scenes/` and clears the `*`.
5. Ctrl+Z back to the save point clears the `*` again; redo restores it.
6. Open Scene on the saved file restores the entities.
7. Open Scene on a file edited by hand to `"version": 1` reports the version and **leaves the current scene intact**.
8. New Scene over unsaved work raises the confirm; Cancel leaves everything untouched.
9. Entering Play greys out both Save items.
10. Closing the window with unsaved changes raises the confirm rather than exiting.

- [ ] **Step 7: Commit**

```bash
git add Arcane/ArcaneEditor/src/ Arcane/Arcane/src/Arcane/Base/Runtime.hpp Arcane/Arcane/src/Arcane/Base/Runtime.cpp
git commit -m "feat(editor): New/Open/Save/Save As Scene, dirty marker, and unsaved-changes guards"
```

---

### Task 7: `HostBoot::BootScene` and project-open integration

**Files:**
- Modify: `Arcane/Loom/src/ProjectBoot.hpp`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (Init and `SwitchProject`)
- Test: `Arcane/Tests/src/HostBootTest.cpp`

**Interfaces:**
- Consumes: `Project::ResolveAsset`, `Arcane::AssetId::FromGuid`, `Scene::ReadSceneFile` / `ApplySceneDocument` / `CreateEmpty`, `Runtime::ResetRegistry`.
- Produces: `bool Arcane::HostBoot::BootScene(Arcane::Runtime&, const Arcane::Project&)` — `false` when there is no boot scene or it could not be loaded; the caller continues either way.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/HostBootTest.cpp`:

```cpp
TEST_CASE("BootSceneFile resolves the manifest's bootScene Guid to a file", "[loom][project]")
{
    // The pure half: Guid -> physical file, through the AssetRegistry the
    // project rebuilt at open. Empty when there is no boot scene, or when the
    // id names nothing this project contains.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_resolve";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content" / "scenes", ec);

    const Arcane::Guid id = Arcane::Guid::Generate();
    std::ofstream(dir / "Content" / "scenes" / "main.arcscene")
        << R"({"id":")" << id.ToString() << R"(","version":2,"entities":[]})";

    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":")" << id.ToString() << R"("})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    const fs::path file = Arcane::HostBoot::BootSceneFile(*proj);
    REQUIRE_FALSE(file.empty());
    CHECK(file.filename() == "main.arcscene");

    fs::remove_all(dir, ec);
}

TEST_CASE("BootSceneFile is empty for no boot scene and for an unknown id", "[loom][project]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_bootscene_absent";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);

    SECTION("empty bootScene")
    {
        std::ofstream(dir / "P.arcproj") <<
            R"({"formatVersion":1,"name":"P","engine":{"abi":)"
            << static_cast<int>(Arcane::kGamePluginABIVersion)
            << R"(},"gameModule":"","plugins":[],"bootScene":""})";
        auto proj = Arcane::Project::Open(dir);
        REQUIRE(proj.has_value());
        CHECK(Arcane::HostBoot::BootSceneFile(*proj).empty());
    }
    SECTION("a Guid this project does not contain")
    {
        std::ofstream(dir / "P.arcproj") <<
            R"({"formatVersion":1,"name":"P","engine":{"abi":)"
            << static_cast<int>(Arcane::kGamePluginABIVersion)
            << R"(},"gameModule":"","plugins":[],"bootScene":")"
            << Arcane::Guid::Generate().ToString() << R"("})";
        auto proj = Arcane::Project::Open(dir);
        REQUIRE(proj.has_value());
        CHECK(Arcane::HostBoot::BootSceneFile(*proj).empty());
    }

    fs::remove_all(dir, ec);
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — no `BootSceneFile`.

- [ ] **Step 3: Write the implementation**

In `Arcane/Loom/src/ProjectBoot.hpp`, add the includes `<Arcane/Serialization/SceneAsset.hpp>`, `<Arcane/Project/AssetId.hpp>` and `<Arcane/Base/Runtime.hpp>`, then:

```cpp
    // The project's boot scene as a physical file, or empty when it has none /
    // the id names nothing this project contains.
    //
    // Split out from BootScene so the RESOLUTION is unit-testable without a
    // Runtime: it is the part with the interesting failure modes.
    inline std::filesystem::path BootSceneFile(const Arcane::Project& project)
    {
        const std::string& text = project.Manifest().bootScene;
        if (text.empty()) return {};

        const std::optional<Arcane::Guid> id = Arcane::Guid::FromString(text);
        if (!id || !id->IsValid())
        {
            ARC_WARN("bootScene '{}' is not a valid asset id", text);
            return {};
        }

        const std::optional<std::filesystem::path> file =
            project.ResolveAsset(Arcane::AssetId::FromGuid(*id));
        if (!file)
        {
            ARC_WARN("bootScene {} does not resolve to a file in this project", text);
            return {};
        }
        return *file;
    }

    // Load the project's boot scene into `runtime`, replacing whatever the
    // registry holds. False when there is no boot scene or it could not be
    // loaded -- callers LOG AND CONTINUE with an empty scene rather than
    // refusing to open the project, because the editor is how a broken boot
    // scene gets fixed.
    //
    // Call AFTER the plugin loads: a scene naming a component the game module
    // registers would otherwise silently drop it.
    inline bool BootScene(Arcane::Runtime& runtime, const Arcane::Project& project)
    {
        const std::filesystem::path file = BootSceneFile(project);
        if (file.empty()) return false;

        // Read before reset, same ordering rule as the editor's Open Scene.
        std::string err;
        const auto doc = Arcane::Scene::ReadSceneFile(file, &err);
        if (!doc)
        {
            ARC_ERROR("bootScene: {}", err);
            return false;
        }

        runtime.ResetRegistry();
        if (!Arcane::Scene::ApplySceneDocument(*doc, runtime.Registry()))
        {
            ARC_ERROR("bootScene: {} parsed but could not be loaded", file.generic_string());
            Arcane::Scene::CreateEmpty(runtime.Registry());
            return false;
        }
        ARC_INFO("Loaded boot scene {}", file.generic_string());
        return true;
    }
```

- [ ] **Step 4: Call it from the editor**

In `EditorApp::Init`, after the plugin host has loaded (the `m_plugin->Load()` block, ~line 249) add:

```cpp
        // After the plugin, so component types it registers deserialize rather
        // than being dropped. A project with no boot scene keeps whatever the
        // plugin's Init built -- code-spawned scenes are legacy, but nothing
        // clears them unless a scene actually takes ownership.
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
        {
            if (Arcane::HostBoot::BootScene(*m_runtime, *proj))
            {
                const std::filesystem::path file = Arcane::HostBoot::BootSceneFile(*proj);
                std::string err;
                if (const auto doc = Arcane::Scene::ReadSceneFile(file, &err))
                    m_scene.Adopt(file, doc->id, *m_undo);
            }
        }
```

Add the identical block at the end of `SwitchProject`, after the new project's plugin has loaded, so opening a project from the menu also boots its scene.

- [ ] **Step 5: Run to verify it passes**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[loom]"
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Loom/src/ProjectBoot.hpp Arcane/ArcaneEditor/src/EditorApp.cpp Arcane/Tests/src/HostBootTest.cpp
git commit -m "feat(arcane): HostBoot::BootScene -- a project opens into its boot scene"
```

---

### Task 8: Asset Browser scene support

**Files:**
- Modify: `Arcane/ArcaneEditor/src/AssetBrowser.hpp` (`AssetKind`, `kAssetKindCount`, `AssetKindOf`, `AssetBrowserActions`), `Arcane/ArcaneEditor/src/AssetBrowser.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (consume the new actions)
- Test: `Arcane/Tests/src/AssetBrowserTest.cpp`

**Interfaces:**
- Produces: `AssetKind::Scene`; `AssetBrowserActions::openScene` (`std::filesystem::path`) and `AssetBrowserActions::setBootScene` (`Arcane::Guid`).

A scene is the one asset kind that is **not** a `DocumentHost` document — double-click loads it into the session, so it must route through the session's guard rather than through `DocumentHost::OpenPath`.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/AssetBrowserTest.cpp`:

```cpp
TEST_CASE("AssetKindOf classifies scenes", "[editor]")
{
    CHECK(AssetKindOf("game://scenes/main.arcscene") == AssetKind::Scene);
    CHECK(AssetKindOf("game://scenes/MAIN.ARCSCENE") == AssetKind::Scene);
    // Not the material kind, and not a generic data file.
    CHECK(AssetKindOf("game://scenes/main.arcscene") != AssetKind::Data);
    CHECK(AssetKindOf("game://mat/glow.arcmat") == AssetKind::Material);
}
```

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
```
Expected: FAIL to compile — no `AssetKind::Scene`.

- [ ] **Step 3: Write the implementation**

In `AssetBrowser.hpp`, extend the enum (append **before** `Other` so existing filter indices for Material/Texture/Audio/Font stay put, and bump the count):

```cpp
    enum class AssetKind : int
    {
        Material = 0,
        Texture,
        Audio,
        Font,
        Data,
        Scene,
        Other,
    };
    inline constexpr int kAssetKindCount = 7;
```

In `AssetKindOf`, add before the texture list:

```cpp
        if (ext == ".arcscene")
            return AssetKind::Scene;
```

Extend `AssetBrowserActions`:

```cpp
        // A scene is NOT a document -- double-clicking one loads it into the
        // editor session, so the host routes it through SceneSession's
        // unsaved-changes guard instead of DocumentHost::OpenPath.
        std::filesystem::path openScene;
        // "Set as Boot Scene" from the row context menu.
        Arcane::Guid setBootScene;
```

In `AssetBrowser.cpp`'s `DrawAssetBrowserPanel`, where a double-click currently routes to `documents.OpenPath`, branch on the kind first:

```cpp
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (entry.kind == AssetKind::Scene)
                        actions.openScene = file;   // host-owned: goes through the scene guard
                    else
                        documents.OpenPath(file);
                }
```

Add the context-menu item in the same row scope:

```cpp
                if (ImGui::BeginPopupContextItem())
                {
                    if (entry.kind == AssetKind::Scene &&
                        ImGui::MenuItem("Set as Boot Scene"))
                        actions.setBootScene = entry.guid;
                    ImGui::EndPopup();
                }
```

Add a `Scene` icon and filter label wherever the existing kinds list their icon and label (follow the pattern already there; `ICON_LC_CLAPPERBOARD` reads as a scene and exists in `IconsLucide.h`).

In `EditorApp.cpp`, beside the existing `browserActions.createInstanceOf` handling:

```cpp
            if (!browserActions.openScene.empty())
            {
                if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene,
                                    browserActions.openScene, *m_undo))
                    DoOpenScene(browserActions.openScene);
            }
            if (browserActions.setBootScene.IsValid())
            {
                if (Arcane::Project* proj = m_runtime->CurrentProjectMutable())
                {
                    if (proj->SetBootScene(browserActions.setBootScene))
                        ARC_INFO("Boot scene set to {}", browserActions.setBootScene.ToString());
                    else
                        m_sceneError = "Could not write the project's boot scene (see Console).";
                }
            }
```

- [ ] **Step 4: Run to verify it passes**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[editor]"
```
Expected: PASS. If any existing test asserts on `kAssetKindCount` or a filter index, update it to match the new roster.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/AssetBrowser.hpp Arcane/ArcaneEditor/src/AssetBrowser.cpp Arcane/ArcaneEditor/src/EditorApp.cpp Arcane/Tests/src/AssetBrowserTest.cpp
git commit -m "feat(editor): scenes in the Asset Browser -- open on double-click, Set as Boot Scene"
```

---

### Task 9: SampleProject ships an authored scene

**Files:**
- Create: `Arcane/SampleProject/Content/scenes/main.arcscene`
- Modify: `Arcane/SampleProject/SampleProject.arcproj`

**Interfaces:**
- Consumes: everything above. This task is the end-to-end proof that a clean checkout opens into authored content.

- [ ] **Step 1: Author the scene in the editor**

```
cd Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor
ArcaneEditor.exe --project ..\..\..\SampleProject\SampleProject.arcproj
```

File ▸ New Scene. In the Outliner, create three children under the root: name them `Ground`, `BoxA`, `BoxB`. Give each a `Transform` and a `SpriteRenderer` via Add Component, and separate them in the Viewport with the gizmo so the scene is visibly non-trivial. File ▸ Save Scene As… → `Content/scenes/main.arcscene`.

- [ ] **Step 2: Set it as the boot scene**

In the Asset Browser, right-click `main.arcscene` → **Set as Boot Scene**. Confirm `SampleProject.arcproj` now reads:

```json
  "bootScene": "<the scene's guid>"
```

and that every other key kept its original order (that is the `ordered_json` guarantee from Task 3 being exercised for real).

- [ ] **Step 3: Verify the round trip from a cold start**

Close the editor. Relaunch with the same command. Expected: the Outliner shows `Scene` with `Ground`, `BoxA`, `BoxB` beneath it, the Viewport draws them, and the Console logs `Loaded boot scene ...`. No `*` on File ▸ Save Scene.

- [ ] **Step 4: Verify the failure path is survivable**

Temporarily edit `SampleProject.arcproj` to set `"bootScene": "00000000-0000-0000-0000-000000000009"`. Relaunch. Expected: the project still opens, the Console carries `bootScene ... does not resolve to a file in this project`, and the editor is usable with an empty scene. Restore the real Guid afterwards.

- [ ] **Step 5: Run the full gate under two seeds**

```
cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe "~[gpu]" --rng-seed 6
ArcaneTests.exe "~[gpu]" --rng-seed 17
```
Expected: identical pass counts under both seeds.

- [ ] **Step 6: Commit**

```bash
git add Arcane/SampleProject/
git commit -m "feat(arcane): SampleProject ships an authored boot scene"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §3.1 `SceneAsset` file layer, read-then-apply ordering | 1 |
| §3.1 `AssetRegistry` `.arcscene` | 2 |
| §3.1 `Project::SetBootScene` | 3 |
| §3.1 `HostBoot::BootScene` | 7 |
| §3.2 `SceneSession` pure state + confirm machine | 5 |
| §4 file format, `id` + `version`, `Content/scenes/`, outside-content-root reporting | 1, 6 |
| §4 `bootScene` as Guid, stale comment fixed | 3 |
| §5 `CommandStack::StateId`, undo-to-clean, cap caveat | 4 |
| §5 dirty proxy assumption documented at the seam | 5 |
| §5 Save disabled during Play | 6 |
| §6 boot scene on open, failure continues | 7 |
| §6 Asset Browser Scene kind, double-click, Set as Boot Scene | 8 |
| §6 SampleProject ships a scene | 9 |
| §7 guards on New/Open/Open Project/exit, teardown reuse | 6 |
| §8 test roster | 1, 2, 3, 4, 5, 7, 8 |

Two spec items are deliberately realised differently and are called out where they land: `SceneSession` computes dirty from a `CommandStack` passed per call rather than holding a reference (no lifetime coupling, and the registry swap makes stored references hazardous), and the Open Project guard lives at the call site rather than inside `SwitchProject`, so the user gets Save/Discard/Cancel instead of the refusal that `AnyDirty` gives material documents.

**Placeholders:** none. Task 6 has no unit test by design and says so rather than inventing one; its verification is a numbered ten-item desk-verify.

**Type consistency:** `kSceneExt`, `SceneDocument`, `ReadSceneFile`, `ApplySceneDocument`, `SaveSceneFile`, `CreateEmpty`, `StateId`, `SetBootScene`, `BootSceneFile`, `BootScene`, `SceneIntent`, `SceneSession::{Path,Id,DisplayName,IsDirty,MarkSaved,Adopt,Reset,Request,Pending,PendingPath,ClearPending}`, `AssetKind::Scene`, `AssetBrowserActions::{openScene,setBootScene}`, `CurrentProjectMutable` are each defined once and used with the same spelling and signature everywhere after.

Three places tell the implementer to verify against the codebase before writing rather than guessing, because the plan could not settle them from the outside: the member holding `Project`'s manifest path, `AssetBrowserState`'s re-scan trigger, and `EditorApp`'s loop-exit member and quit-event site.
