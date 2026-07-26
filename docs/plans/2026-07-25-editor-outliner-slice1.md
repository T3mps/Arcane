# Editor Outliner Slice 1: EntityInfo/Hidden + structural undo — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The engine half of the Outliner arc — the `EntityInfo` + `Hidden` components and the registry-memento structural-undo foundation, all headless-tested; no editor UI in this slice.

**Architecture:** Two appended scene components (identity + hide-marker) ride the existing reflection/serialization seams; structural edits (create/delete/reparent/add/remove/hide/rename) are pure mutator functions in `Arcane/Edit/EntityOps`, and undo is ONE whole-registry memento command (`RegistryStateCommand`) over injected snapshot/restore seams — binary restore resurrects exact entity ids, which dissolves the spec's delete-undo id risk (resolved at planning: the seam is `Registry::Save()/Load()`, already exposed as `Runtime::SnapshotRegistry/RestoreRegistry`, and the CommandStack resolver design already survives the registry swap — see EditorApp.cpp:293).

**Tech Stack:** C++23, Astra ECS (vendored), Catch2 (ArcaneTests), premake5/msbuild.

**Spec:** `docs/superpowers/specs/2026-07-25-editor-outliner-design.md` (§1, §2, slice 1).

## Global Constraints

- /MD runtime everywhere in the Arcane workspace; UTF-8 without BOM; ASCII comments; no `/fp:fast`.
- Tests run FROM THE EXE DIR: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`.
- The dev-loop gate is `ArcaneTests.exe "~[gpu]"` (windowed/[gpu] suites are desk-only on this machine).
- New files ⇒ re-run `Arcane\GenerateProjects.bat` before building (premake globs).
- MSBuild: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo` from `Arcane\`.
- Commit messages with special characters: write to a scratch file, `git commit -F <file>` (PS 5.1 quoting).
- ABI rule: any ARCANE_API vtable / EngineContext / header-only component-or-system change gets a PluginABI.hpp ledger entry (bump only when an existing layout/slot changes — these tasks append types and add a view filter, expected NO bump).

---

### Task 1: EntityInfo + Hidden components, submission skip, rosters

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/Components.hpp` (structs after `PostProcess`, reflect blocks after the `PostProcess` block)
- Modify: `Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp:19-26` (`RegisterSceneComponents`)
- Modify: `Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp:29-40` (traits + view)
- Modify: `Arcane/Sandbox/src/Sandbox.cpp` (ReRegister roster, after the `PostProcess` line)
- Modify: `Arcane/PlaygroundGame/src/PlaygroundGame.cpp` (ReRegister roster, same)
- Modify: `Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp` (extend the 2026-07-25 NO-bump ledger NOTE)
- Test: `Arcane/Tests/src/EntityIdentityTest.cpp` (new)

**Interfaces:**
- Consumes: `Arcane::Guid` (`<Arcane/Guid.hpp>`), the existing reflect macros, `Astra::Not<>` (`<Astra/Registry/Query.hpp>`, pulled in via Registry.hpp).
- Produces: `Arcane::EntityInfo { Guid id; std::string name; }` and `Arcane::Hidden {}` — Task 2's ops and every later slice use exactly these names.

- [ ] **Step 1: Add the components** to `Components.hpp` (after the `PostProcess` struct, inside `namespace Arcane`):

```cpp
    // The editor-facing identity (Outliner arc): a STABLE Guid + display name.
    // Policy: the EDITOR adds this (entity create + first rename); runtime
    // spawns are never forced to carry strings. An entity without one displays
    // as "Entity <id>". The Guid is generated when the component is added and
    // is the durable cross-save identity -- entity ids are not.
    struct EntityInfo
    {
        Guid        id{};
        std::string name;
        // Non-trivially-copyable: the binary path (Play snapshots, scene
        // SaveBinary) serializes through this member instead of the POD
        // memcpy overload (Astra's HasSerializeMethod seam).
        template<typename Archive> void Serialize(Archive& ar) { ar(id); ar(name); }
    };

    // Marker ("tag component"): render submission skips entities carrying it
    // (the Outliner eye). Serialized like any component, so hidden stays
    // hidden in game; the eye applies it to an entity AND its descendants.
    struct Hidden {};
```

And the reflect blocks (after the `PostProcess` reflect block):

```cpp
    ASTRA_REFLECT_TYPE(EntityInfo)
        ASTRA_REFLECT_FIELD(EntityInfo, id)
        ASTRA_REFLECT_FIELD(EntityInfo, name)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(Hidden)
    ASTRA_END_REFLECT_TYPE()
```

`<string>` joins the includes at the top of Components.hpp.

- [ ] **Step 2: Register everywhere.** `SceneModule.hpp` `RegisterSceneComponents` gains, after the `PostProcess` line:

```cpp
        creg.RegisterComponent<EntityInfo>();
        creg.RegisterComponent<Hidden>();
```

`Sandbox.cpp` and `PlaygroundGame.cpp` rosters gain, after their `PostProcess` lines:

```cpp
        creg->ReRegisterComponent<Arcane::EntityInfo>();
        creg->ReRegisterComponent<Arcane::Hidden>();
```

- [ ] **Step 3: Submission skip.** In `RenderSystems.hpp`, `RenderSubmissionSystem`:

```cpp
    struct RenderSubmissionSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer, PreviousTransform, Hidden>>
```

and the view (line ~39):

```cpp
            auto view = reg.CreateView<WorldTransform, SpriteRenderer, Astra::Not<Hidden>>();
```

(`Astra::Not` comes with Registry.hpp; no new include.)

- [ ] **Step 4: Ledger.** Extend the `NOTE (2026-07-25, ...)` entry in `PluginABI.hpp` (same note, one added sentence — do not add a new version):

```
    //     Outliner slice 1 rides the same argument: EntityInfo + Hidden are
    //     two more appended name-keyed component types, and the Not<Hidden>
    //     filter in the plugin-compiled RenderSubmissionSystem is behavioral
    //     (a stale plugin just doesn't honor hiding). Still NO bump.
```

- [ ] **Step 5: Write `EntityIdentityTest.cpp`** — tag `[outliner]`, no device:

```cpp
// Outliner slice 1: EntityInfo/Hidden ride every persistence seam. JSON
// (scene files), BINARY (Play snapshots -- the path that exercises
// EntityInfo::Serialize, the first non-trivially-copyable component), and
// the RenderSubmissionSystem Not<Hidden> skip (recording mock batcher,
// SpriteRotationTest's harness).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <string>

namespace
{
    std::unique_ptr<Astra::Registry> FreshReg(std::shared_ptr<Astra::ComponentRegistry>& outCreg)
    {
        outCreg = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(outCreg);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }

    // The recording-mock shape from SpriteRotationTest.cpp, counting only.
    struct CountingBatcher final : Arcane::Batcher2D
    {
        int rects = 0;
        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float) override { ++rects; }
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override { ++rects; }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2, float, glm::vec4) override {}
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        void RemoveTexture(nvrhi::ITexture*) override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };
}

TEST_CASE("EntityInfo + Hidden round-trip scene JSON", "[outliner][json]")
{
    const Arcane::Guid stableId = Arcane::Guid::Generate();
    nlohmann::json doc;
    {
        std::shared_ptr<Astra::ComponentRegistry> creg;
        auto reg = FreshReg(creg);
        Astra::Entity e = reg->CreateEntity();
        reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
        reg->AddComponent<Arcane::EntityInfo>(e, Arcane::EntityInfo{ stableId, "Player Spawn" });
        reg->AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
        doc = Arcane::Scene::SaveJson(*reg);
    }
    std::shared_ptr<Astra::ComponentRegistry> creg;
    auto reg = FreshReg(creg);
    REQUIRE(Arcane::Scene::LoadJson(*reg, doc));

    int found = 0;
    reg->CreateView<Arcane::EntityInfo>().ForEach(
        [&](Astra::Entity e2, Arcane::EntityInfo& info)
    {
        ++found;
        CHECK(info.id == stableId);
        CHECK(info.name == "Player Spawn");
        CHECK(reg->GetComponent<Arcane::Hidden>(e2) != nullptr);
    });
    CHECK(found == 1);
}

TEST_CASE("EntityInfo survives the binary snapshot path (Play round-trip)", "[outliner]")
{
    std::shared_ptr<Astra::ComponentRegistry> creg;
    auto reg = FreshReg(creg);
    Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::EntityInfo>(
        e, Arcane::EntityInfo{ Arcane::Guid{ 1, 2 }, "A name long enough to defeat SSO ................" });
    reg->AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});

    auto bytes = reg->Save();
    REQUIRE(bytes.IsOk());
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*bytes), creg);
    REQUIRE(loaded.IsOk());

    Arcane::EntityInfo* info = (*loaded)->GetComponent<Arcane::EntityInfo>(e);   // SAME entity id
    REQUIRE(info != nullptr);
    CHECK(info->id == (Arcane::Guid{ 1, 2 }));
    CHECK(info->name == "A name long enough to defeat SSO ................");
    CHECK((*loaded)->GetComponent<Arcane::Hidden>(e) != nullptr);
}

TEST_CASE("RenderSubmissionSystem skips Hidden entities", "[outliner][render]")
{
    std::shared_ptr<Astra::ComponentRegistry> creg;
    auto reg = FreshReg(creg);
    CountingBatcher batcher;
    reg->SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &batcher, glm::vec2(0.0f, 0.0f), 1.0f });

    auto sprite = [&](bool hidden)
    {
        Astra::Entity e = reg->CreateEntity();
        reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
        reg->AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        reg->AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
        if (hidden) reg->AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
        return e;
    };
    sprite(false);
    sprite(true);
    sprite(false);

    Arcane::RenderSubmissionSystem{}(*reg);
    CHECK(batcher.rects == 2);   // the Hidden one never reached the batcher
}
```

(The mock's override set and the `RenderContext2D{ batcher, cameraOffset, zoom }` construction are copied from `SpriteRotationTest.cpp:67-118` — if either drifted, that file is the truth.)

- [ ] **Step 6: Generate, build, test.**

```powershell
cd D:\dev\starworks\Gacha\Arcane; .\GenerateProjects.bat
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
cd bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "[outliner]"
```

Expected: all `[outliner]` cases pass. If the binary round-trip fails inside `WriteVersionedComponent`, the versioned writer needs the same HasSerializeMethod route as the plain one — investigate `BinaryWriter::WriteVersionedComponent` before patching anything (this is the one known unknown; the test exists to surface it).

- [ ] **Step 7: Commit.**

```powershell
cd D:\dev\starworks\Gacha
git add Arcane/Arcane/src/Arcane/Scene/Components.hpp Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp Arcane/Sandbox/src/Sandbox.cpp Arcane/PlaygroundGame/src/PlaygroundGame.cpp Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp Arcane/Tests/src/EntityIdentityTest.cpp
git commit -m "feat(arcane): EntityInfo + Hidden components (outliner arc, slice 1a)"
```

---

### Task 2: EntityOps — pure structural mutators

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp`
- Create: `Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp`
- Test: `Arcane/Tests/src/EntityOpsTest.cpp` (new)

**Interfaces:**
- Consumes: Task 1's `EntityInfo`/`Hidden`; `Astra::Registry` — `CreateEntity()`, `DestroyEntity(e)`, `SetParent(child, parent)`, `GetParent(e)`, `GetChildren(e)`, `AddComponent<T>(e, T)`, `GetComponent<T>(e)`, `AddComponentByID(e, id, buf, size)`, `RemoveComponentByID(e, id)`; `Astra::ComponentDescriptor` — `id`, `size`, `alignment`, `DefaultConstruct(void*)`, `Destruct(void*)`.
- Produces (namespace `Arcane::Edit`, all `ARCANE_API`, exactly these signatures — Task 3's tests and slices 2–4 call them):
  - `std::string AutoEntityName(Astra::Registry&)`
  - `std::string DisplayName(Astra::Registry&, Astra::Entity)`
  - `Astra::Entity CreateEntity(Astra::Registry&, Astra::Entity parent)`
  - `std::size_t DeleteEntities(Astra::Registry&, std::span<const Astra::Entity>)`
  - `std::size_t Reparent(Astra::Registry&, std::span<const Astra::Entity>, Astra::Entity parent)`
  - `std::size_t SetHiddenRecursive(Astra::Registry&, Astra::Entity, bool hidden)`
  - `bool RenameEntity(Astra::Registry&, Astra::Entity, std::string name)`
  - `std::size_t AddComponent(Astra::Registry&, std::span<const Astra::Entity>, const Astra::ComponentDescriptor&)`
  - `std::size_t RemoveComponent(Astra::Registry&, std::span<const Astra::Entity>, const Astra::ComponentDescriptor&)`

- [ ] **Step 1: Write `EntityOps.hpp`:**

```cpp
#pragma once

// Arcane/Edit: pure structural mutators for editor scene edits (Outliner
// arc). Each function mutates the live registry directly and returns what
// the caller reports; UNDO IS NOT HERE -- the editor wraps every call in a
// RegistryStateCommand (whole-registry memento), which is what keeps these
// simple, reusable, and headless-testable. Engine-side so ArcaneTests and
// every host reuse one implementation; zero editor/UI types.

#include <Arcane/Base/Api.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane::Edit
{
    // First of "Entity", "Entity_2", "Entity_3", ... not already used by an
    // EntityInfo in `reg`.
    ARCANE_API std::string AutoEntityName(Astra::Registry& reg);

    // EntityInfo.name when present and non-empty, else "Entity <id>".
    ARCANE_API std::string DisplayName(Astra::Registry& reg, Astra::Entity e);

    // New entity carrying Transform{} + EntityInfo{Generate(), AutoEntityName},
    // parented under `parent` when valid. Returns the new entity.
    ARCANE_API Astra::Entity CreateEntity(Astra::Registry& reg,
                                          Astra::Entity parent);

    // Delete every entity in `set` (duplicates tolerated). Children of a
    // deleted entity first splice up to its nearest NOT-being-deleted
    // ancestor (or to the root when none). Returns entities destroyed.
    ARCANE_API std::size_t DeleteEntities(Astra::Registry& reg,
                                          std::span<const Astra::Entity> set);

    // Reparent every entity in `set` under `parent` (invalid = unparent to
    // root). REFUSES the whole operation (returns 0) when `parent` is inside
    // any moved entity's subtree or is itself in `set` (cycle). Skips
    // entities already under `parent`; returns how many moved.
    ARCANE_API std::size_t Reparent(Astra::Registry& reg,
                                    std::span<const Astra::Entity> set,
                                    Astra::Entity parent);

    // Add (hidden=true) or remove the Hidden marker on `e` AND every
    // descendant. Returns how many entities changed state.
    ARCANE_API std::size_t SetHiddenRecursive(Astra::Registry& reg,
                                              Astra::Entity e, bool hidden);

    // Set the display name; adds EntityInfo{Generate(), name} when absent.
    // False only for a dead entity.
    ARCANE_API bool RenameEntity(Astra::Registry& reg, Astra::Entity e,
                                 std::string name);

    // Default-construct `desc`'s component on every entity in `set` that
    // lacks it / remove it from every entity that carries it. Return =
    // entities touched.
    ARCANE_API std::size_t AddComponent(Astra::Registry& reg,
                                        std::span<const Astra::Entity> set,
                                        const Astra::ComponentDescriptor& desc);
    ARCANE_API std::size_t RemoveComponent(Astra::Registry& reg,
                                           std::span<const Astra::Entity> set,
                                           const Astra::ComponentDescriptor& desc);
}
```

- [ ] **Step 2: Write `EntityOps.cpp`:**

```cpp
#include <Arcane/Edit/EntityOps.hpp>

#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <new>
#include <unordered_set>
#include <vector>

namespace Arcane::Edit
{
    namespace
    {
        bool IsAncestorOrSelf(Astra::Registry& reg, Astra::Entity maybeAncestor,
                              Astra::Entity e)
        {
            for (Astra::Entity cur = e; cur.IsValid(); cur = reg.GetParent(cur))
                if (cur == maybeAncestor)
                    return true;
            return false;
        }

        void CollectSubtree(Astra::Registry& reg, Astra::Entity root,
                            std::vector<Astra::Entity>& out)
        {
            out.push_back(root);
            for (Astra::Entity c : reg.GetChildren(root))
                CollectSubtree(reg, c, out);
        }
    }

    std::string AutoEntityName(Astra::Registry& reg)
    {
        std::unordered_set<std::string> taken;
        reg.CreateView<EntityInfo>().ForEach(
            [&](Astra::Entity, EntityInfo& info) { taken.insert(info.name); });
        if (!taken.contains("Entity"))
            return "Entity";
        for (int i = 2;; ++i)
        {
            std::string candidate = "Entity_" + std::to_string(i);
            if (!taken.contains(candidate))
                return candidate;
        }
    }

    std::string DisplayName(Astra::Registry& reg, Astra::Entity e)
    {
        if (EntityInfo* info = reg.GetComponent<EntityInfo>(e))
            if (!info->name.empty())
                return info->name;
        return "Entity " + std::to_string(e.GetID());
    }

    Astra::Entity CreateEntity(Astra::Registry& reg, Astra::Entity parent)
    {
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Transform>(e, Transform{});
        reg.AddComponent<EntityInfo>(e, EntityInfo{ Guid::Generate(),
                                                    AutoEntityName(reg) });
        if (parent.IsValid())
            reg.SetParent(e, parent);
        return e;
    }

    std::size_t DeleteEntities(Astra::Registry& reg,
                               std::span<const Astra::Entity> set)
    {
        std::unordered_set<Astra::Entity> doomed(set.begin(), set.end());

        // Splice each doomed entity's children to its nearest surviving
        // ancestor BEFORE destroying anything, so subtree structure among
        // survivors is preserved regardless of set nesting.
        for (Astra::Entity e : doomed)
        {
            Astra::Entity heir = reg.GetParent(e);
            while (heir.IsValid() && doomed.contains(heir))
                heir = reg.GetParent(heir);
            for (Astra::Entity c : reg.GetChildren(e))
            {
                if (doomed.contains(c))
                    continue;
                if (heir.IsValid())
                    reg.SetParent(c, heir);
                else
                    reg.RemoveParent(c);
            }
        }

        std::size_t destroyed = 0;
        for (Astra::Entity e : doomed)
        {
            reg.DestroyEntity(e);
            ++destroyed;
        }
        return destroyed;
    }

    std::size_t Reparent(Astra::Registry& reg,
                         std::span<const Astra::Entity> set,
                         Astra::Entity parent)
    {
        if (parent.IsValid())
            for (Astra::Entity e : set)
                if (IsAncestorOrSelf(reg, e, parent))
                    return 0;   // cycle: refuse the whole operation

        std::size_t moved = 0;
        for (Astra::Entity e : set)
        {
            if (reg.GetParent(e) == parent || e == parent)
                continue;
            if (parent.IsValid())
                reg.SetParent(e, parent);
            else
                reg.RemoveParent(e);
            ++moved;
        }
        return moved;
    }

    std::size_t SetHiddenRecursive(Astra::Registry& reg, Astra::Entity e,
                                   bool hidden)
    {
        std::vector<Astra::Entity> subtree;
        CollectSubtree(reg, e, subtree);
        std::size_t changed = 0;
        for (Astra::Entity s : subtree)
        {
            const bool has = reg.GetComponent<Hidden>(s) != nullptr;
            if (hidden && !has)
            {
                reg.AddComponent<Hidden>(s, Hidden{});
                ++changed;
            }
            else if (!hidden && has)
            {
                reg.RemoveComponent<Hidden>(s);
                ++changed;
            }
        }
        return changed;
    }

    bool RenameEntity(Astra::Registry& reg, Astra::Entity e, std::string name)
    {
        if (!reg.IsValid(e))
            return false;
        if (EntityInfo* info = reg.GetComponent<EntityInfo>(e))
        {
            info->name = std::move(name);
            return true;
        }
        reg.AddComponent<EntityInfo>(e, EntityInfo{ Guid::Generate(),
                                                    std::move(name) });
        return true;
    }

    std::size_t AddComponent(Astra::Registry& reg,
                             std::span<const Astra::Entity> set,
                             const Astra::ComponentDescriptor& desc)
    {
        // Mirror of SceneSerializer's add-by-descriptor factory: default-
        // construct into an aligned buffer, add by id, destruct the buffer.
        const std::size_t bytes = desc.size ? desc.size : 1;
        const std::align_val_t align{ desc.alignment ? desc.alignment : 1 };
        std::size_t touched = 0;
        for (Astra::Entity e : set)
        {
            if (reg.HasComponentByHash(e, desc.hash))
                continue;
            void* buf = ::operator new(bytes, align);
            desc.DefaultConstruct(buf);
            reg.AddComponentByID(e, desc.id, buf, desc.size);
            desc.Destruct(buf);
            ::operator delete(buf, align);
            ++touched;
        }
        return touched;
    }

    std::size_t RemoveComponent(Astra::Registry& reg,
                                std::span<const Astra::Entity> set,
                                const Astra::ComponentDescriptor& desc)
    {
        std::size_t touched = 0;
        for (Astra::Entity e : set)
            if (reg.RemoveComponentByID(e, desc.id))
                ++touched;
        return touched;
    }
}
```

Verify while implementing (all in `ThirdParty/Astra/include/Astra/Registry/Registry.hpp`): the unparent call's exact name (`RemoveParent(child)` — check near `SetParent`, line ~1236 uses it internally), `Registry::IsValid(Entity)`'s exact name (Registry.hpp — if it's `IsEntityValid` or on the EntityManager, adapt `RenameEntity` and keep the guard), the descriptor's type-hash field name (`hash` vs `typeHash` — `GetComponentDescriptorByHash(meta->typeHash)` in SceneSerializer implies the descriptor stores it; use the field `HasComponentByHash` wants), and `RemoveComponent<T>` (templated remove exists alongside `RemoveComponentByID`; if not, route the Hidden removal through `RemoveComponentByID` with the Hidden descriptor's id). These are name-level adaptations only — the shapes above are the contract.

- [ ] **Step 3: Write `EntityOpsTest.cpp`** — tag `[outliner]`:

```cpp
// Outliner slice 1: EntityOps structural mutators, headless. Undo semantics
// are RegistryStateCommandTest's job -- these prove the raw mutations.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <array>
#include <memory>

using namespace Arcane;

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        World() { RegisterSceneComponents(reg); }
    };
}

TEST_CASE("CreateEntity: Transform + EntityInfo + auto-name + parenting", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, a);

    CHECK(w.reg.GetComponent<Transform>(a) != nullptr);
    EntityInfo* ia = w.reg.GetComponent<EntityInfo>(a);
    EntityInfo* ib = w.reg.GetComponent<EntityInfo>(b);
    REQUIRE(ia != nullptr);
    REQUIRE(ib != nullptr);
    CHECK(ia->id.IsValid());
    CHECK(ia->name == "Entity");
    CHECK(ib->name == "Entity_2");          // collision-free auto-name
    CHECK(w.reg.GetParent(b) == a);
    CHECK(Edit::DisplayName(w.reg, a) == "Entity");
}

TEST_CASE("DisplayName falls back to the id for untagged entities", "[outliner]")
{
    World w;
    Astra::Entity raw = w.reg.CreateEntity();
    CHECK(Edit::DisplayName(w.reg, raw) ==
          "Entity " + std::to_string(raw.GetID()));
}

TEST_CASE("DeleteEntities splices children to the nearest survivor", "[outliner]")
{
    World w;
    Astra::Entity top = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity mid = Edit::CreateEntity(w.reg, top);
    Astra::Entity leaf = Edit::CreateEntity(w.reg, mid);

    // Delete the middle: the leaf must climb to top, not dangle or die.
    const std::array<Astra::Entity, 1> doomed{ mid };
    CHECK(Edit::DeleteEntities(w.reg, doomed) == 1);
    CHECK(w.reg.GetParent(leaf) == top);

    // Nested set (parent + child both doomed): survivor climbs past both.
    Astra::Entity mid2  = Edit::CreateEntity(w.reg, top);
    Astra::Entity mid3  = Edit::CreateEntity(w.reg, mid2);
    Astra::Entity leaf2 = Edit::CreateEntity(w.reg, mid3);
    const std::array<Astra::Entity, 2> doomed2{ mid2, mid3 };
    CHECK(Edit::DeleteEntities(w.reg, doomed2) == 2);
    CHECK(w.reg.GetParent(leaf2) == top);
}

TEST_CASE("Reparent refuses cycles wholesale, skips no-ops", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, a);
    Astra::Entity c = Edit::CreateEntity(w.reg, b);

    // a -> under its own grandchild = cycle: whole op refused.
    const std::array<Astra::Entity, 1> setA{ a };
    CHECK(Edit::Reparent(w.reg, setA, c) == 0);
    CHECK(!w.reg.GetParent(a).IsValid());

    // b already under a: no-op skip. c -> a: moves.
    const std::array<Astra::Entity, 2> setBC{ b, c };
    CHECK(Edit::Reparent(w.reg, setBC, a) == 1);
    CHECK(w.reg.GetParent(c) == a);

    // Unparent (invalid parent) pulls both to root.
    CHECK(Edit::Reparent(w.reg, setBC, Astra::Entity::Invalid()) == 2);
    CHECK(!w.reg.GetParent(b).IsValid());
}

TEST_CASE("SetHiddenRecursive covers the subtree, idempotently", "[outliner]")
{
    World w;
    Astra::Entity top = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity kid = Edit::CreateEntity(w.reg, top);
    Astra::Entity grandkid = Edit::CreateEntity(w.reg, kid);

    CHECK(Edit::SetHiddenRecursive(w.reg, top, true) == 3);
    CHECK(w.reg.GetComponent<Hidden>(grandkid) != nullptr);
    CHECK(Edit::SetHiddenRecursive(w.reg, top, true) == 0);    // idempotent
    CHECK(Edit::SetHiddenRecursive(w.reg, kid, false) == 2);   // partial unhide
    CHECK(w.reg.GetComponent<Hidden>(top) != nullptr);
    CHECK(w.reg.GetComponent<Hidden>(grandkid) == nullptr);
}

TEST_CASE("RenameEntity adds EntityInfo when missing", "[outliner]")
{
    World w;
    Astra::Entity raw = w.reg.CreateEntity();
    CHECK(Edit::RenameEntity(w.reg, raw, "Boss Arena"));
    EntityInfo* info = w.reg.GetComponent<EntityInfo>(raw);
    REQUIRE(info != nullptr);
    CHECK(info->id.IsValid());               // fresh stable id minted
    CHECK(info->name == "Boss Arena");
    CHECK(Edit::RenameEntity(w.reg, raw, "Boss Arena 2"));
    CHECK(w.reg.GetComponent<EntityInfo>(raw)->name == "Boss Arena 2");
}

TEST_CASE("Add/RemoveComponent by descriptor over a set", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.AddComponent<SpriteRenderer>(a, SpriteRenderer{});   // a already has it

    const Astra::ComponentDescriptor* desc = nullptr;
    for (const Astra::Registry::ComponentInfo& ci : w.reg.InspectEntity(a))
        if (ci.meta && ci.meta->typeName == "Arcane::SpriteRenderer")
            desc = ci.descriptor;
    REQUIRE(desc != nullptr);

    const std::array<Astra::Entity, 2> set{ a, b };
    CHECK(Edit::AddComponent(w.reg, set, *desc) == 1);     // only b lacked it
    CHECK(w.reg.GetComponent<SpriteRenderer>(b) != nullptr);
    CHECK(Edit::RemoveComponent(w.reg, set, *desc) == 2);
    CHECK(w.reg.GetComponent<SpriteRenderer>(a) == nullptr);
}
```

- [ ] **Step 4: Generate, build, run `[outliner]`** (same commands as Task 1 Step 6). Expected: all pass.

- [ ] **Step 5: Commit.**

```powershell
cd D:\dev\starworks\Gacha
git add Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp Arcane/Tests/src/EntityOpsTest.cpp
git commit -m "feat(arcane): EntityOps structural mutators (outliner arc, slice 1b)"
```

---

### Task 3: RegistryStateCommand — the structural memento

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Edit/RegistryStateCommand.hpp`
- Create: `Arcane/Arcane/src/Arcane/Edit/RegistryStateCommand.cpp`
- Modify: `docs/superpowers/specs/2026-07-25-editor-outliner-design.md` §2 (record the resolved risk)
- Test: `Arcane/Tests/src/RegistryStateCommandTest.cpp` (new)

**Interfaces:**
- Consumes: `Arcane::ICommand` (Undo/Redo/Label), `Arcane::CommandStack::Push`, `Arcane::FunctionRef` (`<Arcane/Util/FunctionRef.hpp>`), Task 2's ops (tests only).
- Produces: `Arcane::RegistryStateCommand` (ctor below) and `Arcane::ApplyRegistryMutation(stack, label, snapshot, restore, mutate) -> bool` — slice 2's panel and slice 4's Inspector UI call `ApplyRegistryMutation` for every structural edit.

- [ ] **Step 1: Write `RegistryStateCommand.hpp`:**

```cpp
#pragma once

// Arcane/Edit: whole-registry memento for STRUCTURAL edits (create/delete/
// reparent/add/remove component/hide/rename). Field edits keep the
// fine-grained ComponentEditCommand path; structure uses this because binary
// registry restore resurrects EXACT entity ids -- so delete-undo, create-redo,
// and every later stack entry that references an entity by id stay valid
// (the spec's id-resurrection risk, resolved at planning).
//
// Snapshot/restore are injected seams: the editor binds
// Runtime::SnapshotRegistry / Runtime::RestoreRegistry (restore REPLACES the
// registry object -- the CommandStack's resolve-every-call design already
// survives that, see CommandStack's ctor contract); tests bind
// Registry::Save / Registry::Load over a local slot.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Arcane
{
    class CommandStack;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD
#endif
    class ARCANE_API RegistryStateCommand final : public ICommand
    {
    public:
        using SnapshotFn = std::function<std::vector<std::byte>()>;       // empty = failed
        using RestoreFn  = std::function<bool(std::span<const std::byte>)>;

        // `before` is the registry state BEFORE the (already applied) edit.
        RegistryStateCommand(std::string label, SnapshotFn snapshot,
                             RestoreFn restore, std::vector<std::byte> before);

        // First Undo captures the CURRENT state as the redo target, then
        // restores `before`. A failed capture warns and still restores
        // (undo works; redo becomes a warned no-op).
        void Undo() override;
        void Redo() override;
        const char* Label() const override;

    private:
        std::string            m_label;
        SnapshotFn             m_snapshot;
        RestoreFn              m_restore;
        std::vector<std::byte> m_before;
        std::vector<std::byte> m_after;   // captured on first Undo
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // The one structural-edit entry point: snapshot -> mutate() -> push.
    // Refuses (false, nothing runs) when the before-snapshot fails -- a
    // structural edit without undo coverage must not happen silently.
    // Skips the push (edit stands, no undo step) when mutate() reports no
    // change, so no-op edits never pollute the history. Returns mutate()'s
    // result.
    ARCANE_API bool ApplyRegistryMutation(CommandStack& stack, std::string label,
                                          const RegistryStateCommand::SnapshotFn& snapshot,
                                          const RegistryStateCommand::RestoreFn& restore,
                                          FunctionRef<bool()> mutate);
}
```

- [ ] **Step 2: Write `RegistryStateCommand.cpp`:**

```cpp
#include <Arcane/Edit/RegistryStateCommand.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/CommandStack.hpp>

#include <memory>
#include <utility>

namespace Arcane
{
    RegistryStateCommand::RegistryStateCommand(std::string label,
                                               SnapshotFn snapshot,
                                               RestoreFn restore,
                                               std::vector<std::byte> before)
        : m_label(std::move(label))
        , m_snapshot(std::move(snapshot))
        , m_restore(std::move(restore))
        , m_before(std::move(before))
    {
    }

    void RegistryStateCommand::Undo()
    {
        if (m_after.empty())
        {
            m_after = m_snapshot();
            if (m_after.empty())
                ARC_WARN("'{}': redo-state capture failed -- undo proceeds, "
                         "redo will be unavailable", m_label);
        }
        if (!m_restore(m_before))
            ARC_WARN("'{}': registry restore failed on undo", m_label);
    }

    void RegistryStateCommand::Redo()
    {
        if (m_after.empty())
        {
            ARC_WARN("'{}': no redo state captured -- redo skipped", m_label);
            return;
        }
        if (!m_restore(m_after))
            ARC_WARN("'{}': registry restore failed on redo", m_label);
    }

    const char* RegistryStateCommand::Label() const { return m_label.c_str(); }

    bool ApplyRegistryMutation(CommandStack& stack, std::string label,
                               const RegistryStateCommand::SnapshotFn& snapshot,
                               const RegistryStateCommand::RestoreFn& restore,
                               FunctionRef<bool()> mutate)
    {
        std::vector<std::byte> before = snapshot();
        if (before.empty())
        {
            ARC_WARN("'{}': before-snapshot failed -- structural edit refused "
                     "(it would be un-undoable)", label);
            return false;
        }
        if (!mutate())
            return false;   // no-op edit: no undo step
        stack.Push(std::make_unique<RegistryStateCommand>(
            std::move(label), snapshot, restore, std::move(before)));
        return true;
    }
}
```

- [ ] **Step 3: Write `RegistryStateCommandTest.cpp`** — tag `[outliner]`. The harness owns the registry in a swappable slot, exactly the shape the editor's Runtime seam has:

```cpp
// Outliner slice 1: structural undo through the whole-registry memento.
// THE regression this design exists for: undoing a delete resurrects the
// EXACT entity id (binary restore round-trips the EntityManager), so later
// undo-stack entries referencing the entity stay valid.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <array>
#include <memory>

using namespace Arcane;

namespace
{
    // A registry in a swappable slot: restore REPLACES the object, mirroring
    // Runtime::RestoreRegistry. The resolver hands out the CURRENT one.
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };

        World() { RegisterSceneComponents(*reg); }

        RegistryStateCommand::SnapshotFn Snapshot()
        {
            return [this]() -> std::vector<std::byte>
            {
                auto r = reg->Save();
                return r.IsOk() ? std::move(*r) : std::vector<std::byte>{};
            };
        }
        RegistryStateCommand::RestoreFn Restore()
        {
            return [this](std::span<const std::byte> bytes)
            {
                auto r = Astra::Registry::Load(bytes, creg);
                if (!r.IsOk())
                    return false;
                reg = std::move(*r);
                return true;
            };
        }
    };
}

TEST_CASE("delete-undo resurrects the exact entity id", "[outliner]")
{
    World w;
    Astra::Entity top  = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    Astra::Entity mid  = Edit::CreateEntity(*w.reg, top);
    Astra::Entity leaf = Edit::CreateEntity(*w.reg, mid);
    const Guid stableId = w.reg->GetComponent<EntityInfo>(mid)->id;

    const std::array<Astra::Entity, 1> doomed{ mid };
    REQUIRE(ApplyRegistryMutation(w.stack, "Delete Entity", w.Snapshot(), w.Restore(),
        [&] { return Edit::DeleteEntities(*w.reg, doomed) > 0; }));
    CHECK(w.reg->GetComponent<EntityInfo>(mid) == nullptr);   // gone
    CHECK(w.reg->GetParent(leaf) == top);                      // spliced

    w.stack.Undo();
    // SAME id/version works against the restored registry -- the whole point.
    EntityInfo* info = w.reg->GetComponent<EntityInfo>(mid);
    REQUIRE(info != nullptr);
    CHECK(info->id == stableId);
    CHECK(w.reg->GetParent(mid) == top);
    CHECK(w.reg->GetParent(leaf) == mid);

    w.stack.Redo();
    CHECK(w.reg->GetComponent<EntityInfo>(mid) == nullptr);
    CHECK(w.reg->GetParent(leaf) == top);

    w.stack.Undo();   // and back once more -- the memento swap is stable
    CHECK(w.reg->GetComponent<EntityInfo>(mid) != nullptr);
}

TEST_CASE("create-undo destroys; redo restores the SAME entity", "[outliner]")
{
    World w;
    Astra::Entity created = Astra::Entity::Invalid();
    REQUIRE(ApplyRegistryMutation(w.stack, "New Entity", w.Snapshot(), w.Restore(),
        [&]
        {
            created = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
            return created.IsValid();
        }));
    REQUIRE(created.IsValid());

    w.stack.Undo();
    CHECK(w.reg->GetComponent<EntityInfo>(created) == nullptr);
    w.stack.Redo();
    // The after-state restore brings back the entity under its ORIGINAL id --
    // a plain re-run of CreateEntity could not guarantee that.
    CHECK(w.reg->GetComponent<EntityInfo>(created) != nullptr);
}

TEST_CASE("no-op mutations produce no undo step", "[outliner]")
{
    World w;
    Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> set{ a };
    // Already at root: unparent is a no-op -> refused push.
    CHECK_FALSE(ApplyRegistryMutation(w.stack, "Reparent", w.Snapshot(), w.Restore(),
        [&] { return Edit::Reparent(*w.reg, set, Astra::Entity::Invalid()) > 0; }));
    CHECK_FALSE(w.stack.CanUndo());
}

TEST_CASE("failed before-snapshot refuses the whole edit", "[outliner]")
{
    World w;
    bool mutated = false;
    CHECK_FALSE(ApplyRegistryMutation(
        w.stack, "Doomed",
        []() { return std::vector<std::byte>{}; },   // snapshot always fails
        w.Restore(),
        [&] { mutated = true; return true; }));
    CHECK_FALSE(mutated);          // the mutator never ran
    CHECK_FALSE(w.stack.CanUndo());
}

TEST_CASE("every structural op round-trips through the memento", "[outliner]")
{
    // Hide, rename, and add-component all ride the SAME memento path --
    // this proves each op's effect survives its own undo/redo cycle.
    World w;
    Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> set{ a };

    const Astra::ComponentDescriptor* spriteDesc = nullptr;
    w.reg->AddComponent<SpriteRenderer>(a, SpriteRenderer{});
    for (const Astra::Registry::ComponentInfo& ci : w.reg->InspectEntity(a))
        if (ci.meta && ci.meta->typeName == "Arcane::SpriteRenderer")
            spriteDesc = ci.descriptor;
    REQUIRE(spriteDesc != nullptr);
    w.reg->RemoveComponent<SpriteRenderer>(a);

    REQUIRE(ApplyRegistryMutation(w.stack, "Hide", w.Snapshot(), w.Restore(),
        [&] { return Edit::SetHiddenRecursive(*w.reg, a, true) > 0; }));
    REQUIRE(ApplyRegistryMutation(w.stack, "Rename", w.Snapshot(), w.Restore(),
        [&] { return Edit::RenameEntity(*w.reg, a, "Renamed"); }));
    REQUIRE(ApplyRegistryMutation(w.stack, "Add Sprite", w.Snapshot(), w.Restore(),
        [&] { return Edit::AddComponent(*w.reg, set, *spriteDesc) > 0; }));

    // Unwind all three, newest first.
    w.stack.Undo();
    CHECK(w.reg->GetComponent<SpriteRenderer>(a) == nullptr);
    w.stack.Undo();
    CHECK(w.reg->GetComponent<EntityInfo>(a)->name == "Entity");   // pre-rename
    w.stack.Undo();
    CHECK(w.reg->GetComponent<Hidden>(a) == nullptr);

    // And forward again.
    w.stack.Redo();
    w.stack.Redo();
    w.stack.Redo();
    CHECK(w.reg->GetComponent<Hidden>(a) != nullptr);
    CHECK(w.reg->GetComponent<EntityInfo>(a)->name == "Renamed");
    CHECK(w.reg->GetComponent<SpriteRenderer>(a) != nullptr);
}
```

- [ ] **Step 4: Amend the spec** — in `docs/superpowers/specs/2026-07-25-editor-outliner-design.md` §2, replace the **Named risk** paragraph with:

```markdown
**Risk resolved at planning:** exact-id resurrection comes free from binary
registry restore (`Registry::Save/Load`, surfaced as
`Runtime::SnapshotRegistry/RestoreRegistry`). The six bespoke commands
collapsed into pure mutators (`Arcane/Edit/EntityOps`) + ONE whole-registry
memento (`RegistryStateCommand` via `ApplyRegistryMutation`); the CommandStack
resolver already survives the registry swap. Snapshot size per structural op
is the whole scene (editor-scale; acceptable; revisit only if profiling says
so).
```

- [ ] **Step 5: Generate, build, run `[outliner]`** (commands as Task 1 Step 6), then the full gate:

```powershell
cd D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[outliner]"
.\ArcaneTests.exe "~[gpu]"
```

Expected: `[outliner]` all pass; full `~[gpu]` suite green (29279+ assertions).

- [ ] **Step 6: Commit.**

```powershell
cd D:\dev\starworks\Gacha
git add Arcane/Arcane/src/Arcane/Edit/RegistryStateCommand.hpp Arcane/Arcane/src/Arcane/Edit/RegistryStateCommand.cpp Arcane/Tests/src/RegistryStateCommandTest.cpp docs/superpowers/specs/2026-07-25-editor-outliner-design.md
git commit -m "feat(arcane): RegistryStateCommand structural undo memento (outliner arc, slice 1c)"
```

---

## Out of scope for this plan

Slices 2–4 (panel, multi-edit, add/remove UI) get their own plans as each
predecessor lands — S2's plan will consume exactly the `Produces` interfaces
above. Nothing in this slice touches EditorApp.
