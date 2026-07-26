# Editor Outliner Slice 4: Add/Remove Component UI — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Inspector a searchable `+ Add Component` catalog and a per-component
`Remove Component` header menu, both fanning out across the whole selection as one undo
step, and wire the Outliner row menu's deferred "Add Component..." item to the same popup.

**Architecture:** The pure half (enumerate the ComponentRegistry, apply the one
system-managed hide-list, count how many selected entities lack each type, sort + filter)
becomes a new headless-tested `ArcaneEditor/src/ComponentCatalog.{hpp,cpp}` — the same
split as `EntityList.{hpp,cpp}` / `BuildOutlinerRows`. The ImGui shell in
`EditorPanels.cpp` draws it and routes both mutations through the existing
`Edit::AddComponent` / `Edit::RemoveComponent` (slice 1) wrapped in `ApplyStructural` →
`ApplyRegistryMutation` (whole-registry memento). The Inspector must be handed the
snapshot/restore binding it currently lacks; that struct is renamed `OutlinerBinding` →
`SceneEditBinding` because two panels now share it.

**Tech Stack:** C++23, Astra ECS (`ComponentRegistry::ForEachComponent`,
`Registry::InspectEntity`), Dear ImGui (docking), Catch2, premake5 + MSBuild (VS 2026).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-25-editor-outliner-design.md` **section 5** (and
  the section 3 amendment: no type column, ever).
- Branch: `shader-editor-slice1-material-core`, base HEAD `edf894b3`. Do not merge.
- Hide-list is EXACTLY three types: `Arcane::WorldTransform`, `Arcane::PreviousTransform`,
  `Arcane::PhysicsBodyRef`. Everything else — **including `Arcane::Transform`** — is
  addable and removable (the user asked for Transform by name).
- One user action = **one** undo step. Structural edits go through
  `ApplyRegistryMutation`; never push a raw command.
- UTF-8 without BOM, **ASCII comments only** (no em dashes, no smart quotes) — matches
  every file in this tree.
- Run the gate **from the exe dir**: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`.
  From anywhere else PluginHost/Msdfgen false-fail on relative fixture paths.
- ArcaneTests runs in **random time-seeded order**. One green run is a sample, not a
  proof. Re-run with `--rng-seed 6` and `--rng-seed 17` before calling a task done.
- **Never** write a bare `Arcane::Runtime rt;` in a test — it steals Arcane.dll's
  TypeContext slot and `Edit::` ops then silently report 0 changes. Always
  `Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());`.
- `EditorPanels.cpp` and `EditorApp.cpp` are **NOT** compiled into ArcaneTests. A green
  gate proves nothing about them; prove they compiled by checking that
  `Arcane\bin-int\Debug-windows-x86_64-md\ArcaneEditor\<file>.obj` postdates the source.
- `[gpu]` suites are desk-only on this box. Never attempt them; report DEFERRED.
- MSBuild invocation (from `Arcane\`):
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo`
- Commit messages containing special characters: write a scratch file and
  `git commit -F <file>` (PS 5.1 quoting mangles them otherwise).

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/ArcaneEditor/src/ComponentCatalog.hpp` (create) | `IsSystemManagedComponent` (THE hide-list) + `ComponentCatalogEntry` + `BuildComponentCatalog` declarations |
| `Arcane/ArcaneEditor/src/ComponentCatalog.cpp` (create) | Pure implementation; zero ImGui |
| `Arcane/Tests/src/EditorComponentCatalogTest.cpp` (create) | `[editor][outliner]` headless coverage of the above + add/remove-through-undo |
| `Arcane/premake5.lua` (modify) | Source-compile `ComponentCatalog.cpp` into ArcaneTests |
| `Arcane/ArcaneEditor/src/EditorPanels.hpp` (modify) | `OutlinerBinding` -> `SceneEditBinding`; Inspector takes the binding; `OutlinerState::addComponentPending` |
| `Arcane/ArcaneEditor/src/EditorPanels.cpp` (modify) | Inspector rework (unified hide-list, tag-component headers, intersection fix, Remove menu, Add button) + shared Add popup + Outliner row menu item |
| `Arcane/ArcaneEditor/src/EditorApp.hpp` (modify) | `m_outlinerBinding` -> `m_editBinding`, retyped |
| `Arcane/ArcaneEditor/src/EditorApp.cpp` (modify) | Same rename at 4 sites; pass the binding to `DrawInspectorPanel` |

---

### Task 1: ComponentCatalog (pure core + headless tests)

**Files:**
- Create: `Arcane/ArcaneEditor/src/ComponentCatalog.hpp`
- Create: `Arcane/ArcaneEditor/src/ComponentCatalog.cpp`
- Create: `Arcane/Tests/src/EditorComponentCatalogTest.cpp`
- Modify: `Arcane/premake5.lua` (ArcaneTests `files {}` block, after the
  `ShaderEditorDocument.cpp` entry around line 578)

**Interfaces:**
- Consumes: `Astra::Registry::GetComponentRegistry()`, `Astra::ComponentRegistry::ForEachComponent(fn(ComponentID, const ComponentDescriptor&))`, `Astra::Registry::HasComponentByHash(Entity, uint64_t)`, `Astra::Registry::IsValid(Entity)`, `Astra::ComponentDescriptor::{meta,hash}`, `Astra::TypeMeta::typeName`.
- Produces (Tasks 2-5 rely on these exact signatures):
  - `bool Arcane::Editor::IsSystemManagedComponent(std::string_view typeName)`
  - `struct Arcane::Editor::ComponentCatalogEntry { const Astra::ComponentDescriptor* desc; std::string typeName; std::size_t missingCount; }`
  - `std::vector<ComponentCatalogEntry> Arcane::Editor::BuildComponentCatalog(Astra::Registry&, std::span<const Astra::Entity>, std::string_view filter)`

- [ ] **Step 1: Write the header**

Create `Arcane/ArcaneEditor/src/ComponentCatalog.hpp`:

```cpp
#pragma once

// Add/Remove Component core (Outliner slice 4): the pure, headless-tested half
// of the Inspector's component catalog. The ImGui popup that draws it lives in
// EditorPanels.cpp -- same split as EntityList.hpp / BuildOutlinerRows.

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane::Editor
{
    // System-managed components: derived or runtime-owned state a user must
    // never hand-add or hand-remove.
    //   Arcane::WorldTransform    -- recomputed by TransformPropagationSystem
    //                                every frame; an edit would be stomped.
    //   Arcane::PreviousTransform -- the physics-capture interpolation pose.
    //   Arcane::PhysicsBodyRef    -- a live BodyHandle PhysicsSystem owns and
    //                                re-establishes; hand-adding one installs
    //                                a dangling handle.
    // This is THE hide-list. The Inspector's per-component sections, the Add
    // Component catalog, and Remove Component all consult this one predicate,
    // so the three can never drift apart.
    //
    // Everything else is fair game, INCLUDING Arcane::Transform: an entity
    // without one is simply non-spatial (the gizmo and the render systems
    // already skip it), and the removal is undoable like any other.
    [[nodiscard]] bool IsSystemManagedComponent(std::string_view typeName);

    struct ComponentCatalogEntry
    {
        const Astra::ComponentDescriptor* desc = nullptr;
        std::string typeName;           // reflected name, e.g. "Arcane::SpriteRenderer"
        std::size_t missingCount = 0;   // LIVE selected entities lacking it
    };

    // Every reflected, non-system-managed component type registered in `reg`,
    // sorted by type name, filtered by a case-insensitive substring over that
    // name (an empty filter matches everything).
    //
    // `missingCount` counts the LIVE entities in `selection` that do NOT carry
    // the component -- exactly the set Edit::AddComponent would touch. 0 means
    // "every selected entity already has it", so the caller shows the row
    // disabled instead of offering a no-op add. A dead (stale) selection entry
    // is skipped, matching EntityOps' stale-selection tolerance. Duplicate
    // entries in `selection` would double-count; SelectionContext never holds
    // duplicates.
    //
    // UNREFLECTED components are omitted on purpose: the Inspector can only
    // render reflected types, so adding one would be an invisible edit the
    // header menu could never remove.
    [[nodiscard]] std::vector<ComponentCatalogEntry> BuildComponentCatalog(
        Astra::Registry& reg,
        std::span<const Astra::Entity> selection,
        std::string_view filter);
}
```

- [ ] **Step 2: Write the implementation**

Create `Arcane/ArcaneEditor/src/ComponentCatalog.cpp`:

```cpp
#include "ComponentCatalog.hpp"

#include <Astra/Component/Component.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>
#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        // Case-insensitive substring. Local twin of AssetBrowser.hpp's
        // MatchesFilter helper and EntityList.cpp's ContainsCI; each is an
        // anonymous-namespace local in its own TU, kept that way so no panel
        // header has to export a string utility.
        bool ContainsCI(std::string_view hay, std::string_view needle)
        {
            if (needle.empty())
                return true;
            if (needle.size() > hay.size())
                return false;
            auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
            {
                std::size_t j = 0;
                while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        }
    }

    bool IsSystemManagedComponent(std::string_view typeName)
    {
        return typeName == "Arcane::WorldTransform"
            || typeName == "Arcane::PreviousTransform"
            || typeName == "Arcane::PhysicsBodyRef";
    }

    std::vector<ComponentCatalogEntry> BuildComponentCatalog(
        Astra::Registry& reg,
        std::span<const Astra::Entity> selection,
        std::string_view filter)
    {
        std::vector<ComponentCatalogEntry> out;

        const Astra::ComponentRegistry* creg = reg.GetComponentRegistry();
        if (!creg)
            return out;

        creg->ForEachComponent([&](Astra::ComponentID, const Astra::ComponentDescriptor& desc)
        {
            if (!desc.meta)
                return;   // unreflected: nothing the Inspector could ever show
            // TypeMeta::typeName is a std::string_view into a substring of a
            // larger compile-time literal (__FUNCSIG__ / __PRETTY_FUNCTION__)
            // and is NOT guaranteed NUL-terminated -- copy it before anything
            // treats it as a C string (the ImGui popup does).
            std::string typeName(desc.meta->typeName);
            if (IsSystemManagedComponent(typeName))
                return;
            if (!ContainsCI(typeName, filter))
                return;

            ComponentCatalogEntry e;
            e.desc = &desc;   // points into ComponentRegistry's fixed array: stable
            for (Astra::Entity ent : selection)
            {
                // HasComponentByHash, NOT GetComponentByHash: an empty (tag)
                // component has no storage array, so the getter returns null
                // even when the entity really carries it.
                if (reg.IsValid(ent) && !reg.HasComponentByHash(ent, desc.hash))
                    ++e.missingCount;
            }
            e.typeName = std::move(typeName);
            out.push_back(std::move(e));
        });

        // ForEachComponent walks ascending ComponentID = registration order,
        // which is meaningless to a user. Name order is the browsable one.
        std::sort(out.begin(), out.end(),
                  [](const ComponentCatalogEntry& a, const ComponentCatalogEntry& b)
                  { return a.typeName < b.typeName; });
        return out;
    }
}
```

- [ ] **Step 3: Wire the new TU into ArcaneTests**

In `Arcane/premake5.lua`, inside the `project "ArcaneTests"` `files {}` block, append
directly AFTER the existing `"%{wks.location}/ArcaneEditor/src/ShaderEditorDocument.cpp",`
line (around line 578) and BEFORE the closing `}`:

```lua
        -- Outliner slice 4: ComponentCatalog (registry enumeration + the one
        -- system-managed hide-list + selection-aware missing counts) source-
        -- compiles into the test exe so the [editor] units drive it directly --
        -- no ImGui dependency, same pattern as EntityList/InspectorFields above.
        "%{wks.location}/ArcaneEditor/src/ComponentCatalog.cpp",
```

- [ ] **Step 4: Write the failing tests**

Create `Arcane/Tests/src/EditorComponentCatalogTest.cpp`:

```cpp
// Outliner slice 4: the pure component-catalog core behind the Inspector's
// Add Component popup. The ImGui shell (EditorPanels.cpp) is NOT compiled into
// this exe, so these cover the filtering/counting rules only; the popup itself
// is desk-verified.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Component/Component.hpp>
#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ComponentCatalog.hpp"
#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;
using Arcane::Editor::BuildComponentCatalog;
using Arcane::Editor::ComponentCatalogEntry;
using Arcane::Editor::IsSystemManagedComponent;

namespace
{
    // A registry in a swappable slot (mirrors RegistryStateCommandTest's World):
    // a structural undo REPLACES the object, so the resolver hands out the
    // CURRENT one.
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };

        World()
        {
            // EntityOps.cpp is compiled into Arcane.dll, so its AddComponent<T>
            // resolves ids through Arcane.dll's own per-module TypeContext slot.
            // Pin that slot to the shared test context; NEVER construct a bare
            // Arcane::Runtime here (it would install an unshared context and the
            // Edit:: ops would silently report 0 changes).
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            RegisterSceneComponents(*reg);
            // Registered so the PhysicsBodyRef hide-list assertion is a real
            // check rather than a vacuous one.
            RegisterPhysicsComponents(*reg);
        }

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

    const ComponentCatalogEntry* Find(const std::vector<ComponentCatalogEntry>& v,
                                      std::string_view name)
    {
        for (const ComponentCatalogEntry& e : v)
            if (e.typeName == name)
                return &e;
        return nullptr;
    }
}

TEST_CASE("IsSystemManagedComponent covers exactly the three derived types", "[editor][outliner]")
{
    CHECK(IsSystemManagedComponent("Arcane::WorldTransform"));
    CHECK(IsSystemManagedComponent("Arcane::PreviousTransform"));
    CHECK(IsSystemManagedComponent("Arcane::PhysicsBodyRef"));

    // Transform is deliberately REMOVABLE (spec section 5).
    CHECK_FALSE(IsSystemManagedComponent("Arcane::Transform"));
    CHECK_FALSE(IsSystemManagedComponent("Arcane::SpriteRenderer"));
    CHECK_FALSE(IsSystemManagedComponent("Arcane::EntityInfo"));
    CHECK_FALSE(IsSystemManagedComponent("Arcane::Hidden"));
    CHECK_FALSE(IsSystemManagedComponent(""));
}

TEST_CASE("BuildComponentCatalog hides system-managed types and sorts by name", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    const std::vector<ComponentCatalogEntry> all = BuildComponentCatalog(*w.reg, sel, "");
    REQUIRE_FALSE(all.empty());

    CHECK(Find(all, "Arcane::WorldTransform") == nullptr);
    CHECK(Find(all, "Arcane::PreviousTransform") == nullptr);
    CHECK(Find(all, "Arcane::PhysicsBodyRef") == nullptr);
    CHECK(Find(all, "Arcane::Transform") != nullptr);
    CHECK(Find(all, "Arcane::SpriteRenderer") != nullptr);

    CHECK(std::is_sorted(all.begin(), all.end(),
                         [](const ComponentCatalogEntry& x, const ComponentCatalogEntry& y)
                         { return x.typeName < y.typeName; }));

    for (const ComponentCatalogEntry& e : all)
        CHECK(e.desc != nullptr);
}

TEST_CASE("BuildComponentCatalog missingCount is the set Edit::AddComponent would touch",
          "[editor][outliner]")
{
    World w;
    // `a` carries Transform (CreateEntity adds it); `b` is a bare entity.
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = w.reg->CreateEntity();
    const std::array<Astra::Entity, 2> sel{ a, b };

    const std::vector<ComponentCatalogEntry> cat = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* transform = Find(cat, "Arcane::Transform");
    const ComponentCatalogEntry* sprite    = Find(cat, "Arcane::SpriteRenderer");
    REQUIRE(transform != nullptr);
    REQUIRE(sprite != nullptr);

    CHECK(transform->missingCount == 1);   // only b lacks it
    CHECK(sprite->missingCount == 2);      // neither has it

    // And the count is exactly what the mutator reports.
    CHECK(Edit::AddComponent(*w.reg, sel, *transform->desc) == 1);
    CHECK(Edit::AddComponent(*w.reg, sel, *sprite->desc) == 2);

    const std::vector<ComponentCatalogEntry> after = BuildComponentCatalog(*w.reg, sel, "");
    CHECK(Find(after, "Arcane::Transform")->missingCount == 0);
    CHECK(Find(after, "Arcane::SpriteRenderer")->missingCount == 0);
}

TEST_CASE("BuildComponentCatalog counts TAG components via HasComponentByHash",
          "[editor][outliner]")
{
    // Regression guard: Hidden is an empty component, so GetComponentByHash
    // returns null even when the entity carries it. Counting with the getter
    // would report it missing forever.
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    const ComponentCatalogEntry* before =
        Find(BuildComponentCatalog(*w.reg, sel, ""), "Arcane::Hidden");
    REQUIRE(before != nullptr);
    CHECK(before->missingCount == 1);

    REQUIRE(Edit::SetHiddenRecursive(*w.reg, a, true) == 1);

    const std::vector<ComponentCatalogEntry> after = BuildComponentCatalog(*w.reg, sel, "");
    const ComponentCatalogEntry* hidden = Find(after, "Arcane::Hidden");
    REQUIRE(hidden != nullptr);
    CHECK(hidden->missingCount == 0);
}

TEST_CASE("BuildComponentCatalog filter is a case-insensitive substring", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    CHECK(Find(BuildComponentCatalog(*w.reg, sel, "sprite"), "Arcane::SpriteRenderer") != nullptr);
    CHECK(Find(BuildComponentCatalog(*w.reg, sel, "SPRITE"), "Arcane::SpriteRenderer") != nullptr);
    CHECK(Find(BuildComponentCatalog(*w.reg, sel, "sprite"), "Arcane::Transform") == nullptr);
    CHECK(BuildComponentCatalog(*w.reg, sel, "zzzz-no-such-component").empty());

    // The filter runs over the FULL reflected name, so the namespace matches too.
    CHECK_FALSE(BuildComponentCatalog(*w.reg, sel, "arcane::").empty());
}

TEST_CASE("BuildComponentCatalog skips dead selection entries", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const Astra::Entity doomed = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> kill{ doomed };
    REQUIRE(Edit::DeleteEntities(*w.reg, kill) == 1);

    const std::array<Astra::Entity, 2> sel{ a, doomed };
    const ComponentCatalogEntry* sprite =
        Find(BuildComponentCatalog(*w.reg, sel, ""), "Arcane::SpriteRenderer");
    REQUIRE(sprite != nullptr);
    CHECK(sprite->missingCount == 1);   // the dead handle contributes nothing
}

TEST_CASE("catalog add/remove round-trip through ApplyRegistryMutation", "[editor][outliner]")
{
    // The exact call shape the Inspector uses: pick a descriptor out of the
    // catalog, mutate the whole selection, undo, redo.
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const Astra::Entity b = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 2> sel{ a, b };

    const ComponentCatalogEntry* sprite =
        Find(BuildComponentCatalog(*w.reg, sel, ""), "Arcane::SpriteRenderer");
    REQUIRE(sprite != nullptr);
    const std::uint64_t spriteHash = sprite->desc->hash;

    REQUIRE(ApplyRegistryMutation(w.stack, "Add Component", w.Snapshot(), w.Restore(),
                                  [&] { return Edit::AddComponent(*w.reg, sel, *sprite->desc) > 0; }));
    CHECK(w.reg->HasComponentByHash(a, spriteHash));
    CHECK(w.reg->HasComponentByHash(b, spriteHash));

    w.stack.Undo();
    CHECK_FALSE(w.reg->HasComponentByHash(a, spriteHash));
    CHECK_FALSE(w.reg->HasComponentByHash(b, spriteHash));

    w.stack.Redo();
    CHECK(w.reg->HasComponentByHash(a, spriteHash));
    CHECK(w.reg->HasComponentByHash(b, spriteHash));

    // Removal is one step too. Re-fetch the descriptor: Restore REPLACED the
    // registry object, though the ComponentRegistry (and so the descriptor
    // addresses) is shared and survives.
    const ComponentCatalogEntry* sprite2 =
        Find(BuildComponentCatalog(*w.reg, sel, ""), "Arcane::SpriteRenderer");
    REQUIRE(sprite2 != nullptr);
    REQUIRE(ApplyRegistryMutation(w.stack, "Remove Component", w.Snapshot(), w.Restore(),
                                  [&] { return Edit::RemoveComponent(*w.reg, sel, *sprite2->desc) > 0; }));
    CHECK_FALSE(w.reg->HasComponentByHash(a, spriteHash));

    w.stack.Undo();
    CHECK(w.reg->HasComponentByHash(a, spriteHash));
    CHECK(w.reg->HasComponentByHash(b, spriteHash));
}

TEST_CASE("no-op add pushes no undo step", "[editor][outliner]")
{
    World w;
    const Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());
    const std::array<Astra::Entity, 1> sel{ a };

    const ComponentCatalogEntry* transform =
        Find(BuildComponentCatalog(*w.reg, sel, ""), "Arcane::Transform");
    REQUIRE(transform != nullptr);
    CHECK(transform->missingCount == 0);   // CreateEntity already added it

    CHECK_FALSE(ApplyRegistryMutation(w.stack, "Add Component", w.Snapshot(), w.Restore(),
                                      [&] { return Edit::AddComponent(*w.reg, sel, *transform->desc) > 0; }));
    CHECK_FALSE(w.stack.CanUndo());
}
```

- [ ] **Step 5: Regenerate projects and build**

Run from `D:\dev\starworks\Gacha\Arcane`:

```
GenerateProjects.bat
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
```

Expected: build succeeds. (`GenerateProjects.bat` is required for two reasons — the new
`ComponentCatalog.cpp` must enter both the globbed ArcaneEditor project and the explicit
ArcaneTests list edited in Step 3.)

- [ ] **Step 6: Run the new tests**

From `D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests`:

```
.\ArcaneTests.exe "[editor][outliner]" --rng-seed 6
```

Expected: all `EditorComponentCatalogTest` cases PASS, 0 failures.

- [ ] **Step 7: Run the full gate twice under fixed seeds**

From the same directory:

```
.\ArcaneTests.exe ~[gpu] --rng-seed 6
.\ArcaneTests.exe ~[gpu] --rng-seed 17
```

Expected: `All tests passed` both times, assertion count >= 29529 and case count >= 505
(the slice-3 baseline plus the new cases).

- [ ] **Step 8: Commit**

```bash
git add Arcane/ArcaneEditor/src/ComponentCatalog.hpp Arcane/ArcaneEditor/src/ComponentCatalog.cpp Arcane/Tests/src/EditorComponentCatalogTest.cpp Arcane/premake5.lua
git commit -m "feat(arcane-editor): component catalog core for Add/Remove Component (slice 4 task 1)"
```

---

### Task 2: Thread the edit binding into the Inspector; unify the hide-list

No new UI. This task makes the Inspector capable of structural edits and fixes two latent
defects it already has (tag components invisible, intersection test blind to tag
components).

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (struct rename + both signatures)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`ApplyStructural`, `DeleteSelection`, `DrawOutlinerPanel`, `DrawInspectorPanel`)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp:102`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp:305,310,1396,1398,1399-1400`

**Interfaces:**
- Consumes: Task 1's `IsSystemManagedComponent`.
- Produces: `struct Arcane::Editor::SceneEditBinding { RegistryStateCommand::SnapshotFn snapshot; RegistryStateCommand::RestoreFn restore; bool editMode; }` and
  `void DrawInspectorPanel(Astra::Registry&, const SelectionContext&, Arcane::CommandStack&, const SceneEditBinding&, const Arcane::Project*)`. Tasks 3-5 use both.

- [ ] **Step 1: Rename the binding struct in the header**

In `Arcane/ArcaneEditor/src/EditorPanels.hpp`, replace lines 89-100 (the comment block
plus `struct OutlinerBinding`) with:

```cpp
    // The Outliner (replaces the flat Hierarchy panel). Pure row data comes
    // from BuildOutlinerRows (EntityList.hpp, headless-tested); this shell
    // draws it and routes EVERY structural edit through ApplyRegistryMutation
    // over binding.snapshot/restore (Runtime::SnapshotRegistry/RestoreRegistry).
    // binding.editMode == false (Play running) disables structural edits --
    // the slice-2 resolution of RegistryStateCommand.hpp's native-state note.
    //
    // Named SceneEditBinding rather than OutlinerBinding since slice 4: the
    // Inspector's Add/Remove Component are structural edits too and share it.
    struct SceneEditBinding
    {
        Arcane::RegistryStateCommand::SnapshotFn snapshot;
        Arcane::RegistryStateCommand::RestoreFn  restore;
        bool editMode = true;    // false during Play: structural edits disabled
    };
```

- [ ] **Step 2: Add the deferred-popup latch to OutlinerState and update both signatures**

Still in `EditorPanels.hpp`: inside `struct OutlinerState`, after
`double lastClickTime = 0.0;` (line 110), add:

```cpp
        // Latched by the row menu's "Add Component..." and consumed at panel
        // scope one step later: ImGui cannot open a popup from inside another
        // popup's scope.
        bool addComponentPending = false;
```

Change `DrawOutlinerPanel`'s parameter type (line 113) from `const OutlinerBinding& binding`
to `const SceneEditBinding& binding`.

Replace `DrawInspectorPanel`'s declaration (lines 129-131) with:

```cpp
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding,
                            const Arcane::Project* project);
```

And in the doc comment immediately above it (lines 116-128), replace the two occurrences of
`` `editMode` `` with `` `binding.editMode` `` and append this paragraph before the
`project` sentence:

```cpp
    // `binding` also carries the registry snapshot/restore seam, which is what
    // makes Add/Remove Component (structural, whole-registry memento) possible
    // from this panel -- the field-edit path above still uses the fine-grained
    // ComponentEditCommand gestures.
```

- [ ] **Step 3: Retype the three uses in EditorPanels.cpp**

In `Arcane/ArcaneEditor/src/EditorPanels.cpp`, change `const OutlinerBinding& b` to
`const SceneEditBinding& b` at line 368 (`ApplyStructural`), and
`const OutlinerBinding& binding` to `const SceneEditBinding& binding` at line 385
(`DeleteSelection`) and line 397 (`DrawOutlinerPanel`).

- [ ] **Step 4: Rewrite DrawInspectorPanel**

Replace the whole of `DrawInspectorPanel` (lines 943-1013, from
`void DrawInspectorPanel(` through the closing `}` before the namespace's final `}`) with:

```cpp
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding,
                            const Arcane::Project* project)
    {
        ImGui::Begin("Inspector");
        if (!sel.HasSelection())
        {
            ImGui::TextDisabled("No selection");
            ImGui::End();
            return;
        }

        const Astra::Entity primary = sel.Primary();
        const std::string primaryName = Arcane::Edit::DisplayName(registry, primary);
        if (sel.Count() > 1)
            ImGui::Text("%s (+%zu)", primaryName.c_str(), sel.Count() - 1);
        else
            ImGui::TextUnformatted(primaryName.c_str());
        ImGui::Separator();

        // Removal is DEFERRED past the loop: Edit::RemoveComponent moves the
        // entity to a different archetype, which dangles every ci.data pointer
        // in the vector being iterated. Descriptor pointers themselves are
        // stable (they live in ComponentRegistry's fixed array).
        const Astra::ComponentDescriptor* pendingRemove = nullptr;

        for (const Astra::Registry::ComponentInfo& ci : registry.InspectEntity(primary))
        {
            // An unreflected component has no name to show and no fields to
            // visit: visitFields is populated FROM TypeMeta at registration, so
            // meta != null implies visitFields != null.
            if (!ci.descriptor || !ci.meta)
                continue;
            // TypeMeta::typeName is a std::string_view into a substring of a larger
            // compile-time literal (__FUNCSIG__/__PRETTY_FUNCTION__) -- NOT
            // guaranteed NUL-terminated, so it is copied into a std::string before
            // handing a `const char*` to ImGui.
            const std::string typeName(ci.meta->typeName);
            // Derived/runtime-owned state is never authored. ONE hide-list, in
            // ComponentCatalog.hpp: the same predicate gates these sections, the
            // Add Component catalog, and Remove Component, so the three cannot
            // drift apart.
            if (IsSystemManagedComponent(typeName))
                continue;

            // Component-type INTERSECTION: editing a component only some of the
            // selection carries would silently edit a subset, so hide it entirely.
            // HasComponentByHash, NOT GetComponentByHash: an empty (tag) component
            // has no storage array, so the getter returns null even when present,
            // which used to make every tag component look unshared.
            bool sharedByAll = true;
            for (Astra::Entity e : sel.Entities())
            {
                if (e != primary && !registry.HasComponentByHash(e, ci.descriptor->hash))
                {
                    sharedByAll = false;
                    break;
                }
            }
            if (!sharedByAll)
                continue;

            const bool open = ImGui::CollapsingHeader(typeName.c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            // Header context menu. A null str_id makes the popup inherit the
            // HEADER's item id, so each component gets its own popup -- a shared
            // literal id would make every header open the same popup and the
            // first-drawn component would swallow the click.
            if (ImGui::BeginPopupContextItem())
            {
                if (!binding.editMode)
                    ImGui::BeginDisabled();
                if (ImGui::MenuItem("Remove Component"))
                    pendingRemove = ci.descriptor;
                if (!binding.editMode)
                    ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            if (!open)
                continue;

            // Tag components (Astra's is_empty optimization) have no storage
            // array, so ci.data is null even though the entity carries them.
            // They still get a header: that is what makes them visible at all,
            // and what makes them removable through the menu above.
            if (!ci.data)
            {
                ImGui::TextDisabled("(tag component -- no fields)");
                continue;
            }

            ImGuiFieldVisitor visitor;
            // Null while Play is running: BeginGestureIfActivated/EndGesture both
            // early-return on a null stack, so gesture bracketing is fully inert
            // (no Begin, no Commit/Cancel) against the live simulating registry.
            visitor.stack      = binding.editMode ? &undo : nullptr;
            visitor.entity     = primary;
            visitor.descriptor = ci.descriptor;
            visitor.typeName   = typeName;
            visitor.project    = project;
            visitor.registry   = &registry;
            visitor.selection  = &sel.Entities();
            ci.descriptor->visitFields(ci.data, visitor);
        }

        if (pendingRemove)
        {
            // Copy the selection: ApplyStructural's mutate runs immediately and
            // the span must outlive it.
            const std::vector<Astra::Entity> targets = sel.Entities();
            const Astra::ComponentDescriptor& desc = *pendingRemove;
            ApplyStructural(undo, binding, "Remove Component",
                [&] { return Arcane::Edit::RemoveComponent(registry, targets, desc) > 0; });
        }

        ImGui::End();
    }
```

Add `#include "ComponentCatalog.hpp"` to the include block at the top of
`EditorPanels.cpp`, immediately after `#include "AssetBrowser.hpp"` (line 2), keeping the
existing alphabetical order:

```cpp
#include "ComponentCatalog.hpp"
```

- [ ] **Step 5: Update EditorApp**

In `Arcane/ArcaneEditor/src/EditorApp.hpp` line 102, replace:

```cpp
        Arcane::Editor::OutlinerBinding m_outlinerBinding;
```

with:

```cpp
        Arcane::Editor::SceneEditBinding m_editBinding;
```

In `Arcane/ArcaneEditor/src/EditorApp.cpp`, rename the identifier at lines 305, 310, 1396
and 1398 (`m_outlinerBinding` -> `m_editBinding`), and replace lines 1399-1400 with:

```cpp
            Arcane::Editor::DrawInspectorPanel(m_runtime->Registry(), m_selection, *m_undo,
                                               m_editBinding, m_runtime->CurrentProject());
```

Note that line 1396 (`m_editBinding.editMode = !m_play.IsPlaying();`) already runs BEFORE
both panel calls, so the Inspector sees the same edit-mode value it used to receive as its
own `bool` argument. Do not add a second assignment.

- [ ] **Step 6: Build**

From `D:\dev\starworks\Gacha\Arcane`:

```
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
```

Expected: build succeeds with 0 errors. If `ArcaneEditor.exe` fails to link because the
running editor holds the file lock, close the editor and rebuild -- do not work around it.

- [ ] **Step 7: Prove the ImGui TUs actually recompiled**

`EditorPanels.cpp` and `EditorApp.cpp` are not in ArcaneTests, so the gate says nothing
about them. Verify from `D:\dev\starworks\Gacha\Arcane`:

```powershell
Get-ChildItem bin-int\Debug-windows-x86_64-md\ArcaneEditor\EditorPanels.obj, bin-int\Debug-windows-x86_64-md\ArcaneEditor\EditorApp.obj | Select-Object Name, LastWriteTime
Get-ChildItem ArcaneEditor\src\EditorPanels.cpp, ArcaneEditor\src\EditorApp.cpp | Select-Object Name, LastWriteTime
```

Expected: both `.obj` timestamps are LATER than their corresponding `.cpp` timestamps.

- [ ] **Step 8: Run the full gate**

From `D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests`:

```
.\ArcaneTests.exe ~[gpu] --rng-seed 6
```

Expected: `All tests passed`.

- [ ] **Step 9: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp Arcane/ArcaneEditor/src/EditorApp.hpp Arcane/ArcaneEditor/src/EditorApp.cpp
git commit -m "feat(arcane-editor): Inspector takes the scene-edit binding + Remove Component (slice 4 task 2)"
```

---

### Task 3: The shared searchable Add Component popup + the Inspector button

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (anonymous namespace at line ~362; `DrawInspectorPanel`)

**Interfaces:**
- Consumes: Task 1's `BuildComponentCatalog` / `ComponentCatalogEntry`; Task 2's
  `SceneEditBinding` and `ApplyStructural`.
- Produces (Task 5 reuses both, verbatim):
  - `constexpr const char* kAddComponentPopup` (file-local)
  - `void DrawAddComponentPopup(Astra::Registry&, const std::vector<Astra::Entity>& selection, Arcane::CommandStack&, const SceneEditBinding&)` (file-local)

- [ ] **Step 1: Add the shared popup helper**

In `Arcane/ArcaneEditor/src/EditorPanels.cpp`, inside the existing anonymous namespace
(the block opening at line 362), insert AFTER the `DeleteSelection` function and BEFORE
the closing `}` of that namespace:

```cpp
        // Popup id shared by the Inspector's "+ Add Component" button and the
        // Outliner row menu's "Add Component...". Both open it at their own
        // panel-window scope, so the id resolves identically at both sites.
        constexpr const char* kAddComponentPopup = "##addcomponent";

        // The searchable Add Component popup: draws the catalog, applies the
        // pick as ONE undo step over the whole selection. The caller opens it
        // with ImGui::OpenPopup(kAddComponentPopup) and then calls this every
        // frame at the same id-stack level.
        //
        // One popup is open at a time, so a function-local search buffer serves
        // both call sites (same rationale as the asset-ref pick popup below).
        void DrawAddComponentPopup(Astra::Registry& registry,
                                   const std::vector<Astra::Entity>& selection,
                                   Arcane::CommandStack& undo,
                                   const SceneEditBinding& binding)
        {
            static char s_search[64] = {};
            const Astra::ComponentDescriptor* chosen = nullptr;

            if (ImGui::BeginPopup(kAddComponentPopup))
            {
                if (ImGui::IsWindowAppearing())
                {
                    s_search[0] = '\0';
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputTextWithHint("##compsearch", "Search...",
                                         s_search, sizeof(s_search));
                ImGui::Separator();

                const std::vector<ComponentCatalogEntry> entries =
                    BuildComponentCatalog(registry, selection, s_search);
                if (entries.empty())
                {
                    ImGui::TextDisabled("no matching components");
                }
                else
                {
                    ImGui::BeginChild("##complist", ImVec2(260.0f, 260.0f));
                    for (const ComponentCatalogEntry& e : entries)
                    {
                        // missingCount == 0 means every selected entity already
                        // carries it, so the add would be a no-op. Shown
                        // disabled rather than hidden: "you already have this"
                        // reads better than a row that silently vanishes.
                        const bool addable = e.missingCount > 0;
                        if (!addable)
                            ImGui::BeginDisabled();
                        if (ImGui::Selectable(e.typeName.c_str()) && addable)
                            chosen = e.desc;
                        if (!addable)
                            ImGui::EndDisabled();
                    }
                    ImGui::EndChild();
                }

                if (chosen)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            // Applied OUTSIDE the popup scope: the mutation invalidates the
            // catalog vector the loop above is still holding.
            if (chosen)
            {
                const Astra::ComponentDescriptor& desc = *chosen;
                ApplyStructural(undo, binding, "Add Component",
                    [&] { return Arcane::Edit::AddComponent(registry, selection, desc) > 0; });
            }
        }
```

- [ ] **Step 2: Add the Inspector button**

In `DrawInspectorPanel` (rewritten in Task 2), replace the tail:

```cpp
        ImGui::End();
    }
```

with:

```cpp
        // Bottom of the panel, full width -- the UE/Unity placement.
        ImGui::Separator();
        if (!binding.editMode)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_LC_PLUS " Add Component", ImVec2(-FLT_MIN, 0.0f)))
            ImGui::OpenPopup(kAddComponentPopup);
        if (!binding.editMode)
            ImGui::EndDisabled();
        // Drawn unconditionally at window scope: BeginPopup is a no-op until
        // the button above (or a previous frame's click) opened it.
        DrawAddComponentPopup(registry, sel.Entities(), undo, binding);

        ImGui::End();
    }
```

Note: this must land AFTER the `if (pendingRemove) { ... }` block, so the button is the
last thing drawn in the panel. `ICON_LC_PLUS` is already available via the existing
`#include "IconsLucide.h"` at the top of the file, and `FLT_MIN` via `<cfloat>`.

- [ ] **Step 3: Build**

From `D:\dev\starworks\Gacha\Arcane`:

```
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
```

Expected: build succeeds with 0 errors.

- [ ] **Step 4: Prove EditorPanels.cpp recompiled**

From `D:\dev\starworks\Gacha\Arcane`:

```powershell
Get-ChildItem bin-int\Debug-windows-x86_64-md\ArcaneEditor\EditorPanels.obj, ArcaneEditor\src\EditorPanels.cpp | Select-Object Name, LastWriteTime
```

Expected: the `.obj` timestamp is LATER than the `.cpp` timestamp.

- [ ] **Step 5: Run the full gate**

From `D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests`:

```
.\ArcaneTests.exe ~[gpu] --rng-seed 17
```

Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(arcane-editor): searchable Add Component popup in the Inspector (slice 4 task 3)"
```

---

### Task 4: Outliner row context menu -> "Add Component..."

Closes the item slice 2 deferred to slice 4.

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`DrawOutlinerPanel`: row context menu around line 609; panel tail around line 705)

**Interfaces:**
- Consumes: Task 3's `kAddComponentPopup` + `DrawAddComponentPopup`; Task 2's
  `OutlinerState::addComponentPending`.
- Produces: nothing new.

- [ ] **Step 1: Add the menu item**

In `DrawOutlinerPanel`'s row context menu, replace:

```cpp
                        if (ImGui::MenuItem("Rename", "F2"))
                            BeginRename(state, row.entity, row.label);
```

with:

```cpp
                        if (ImGui::MenuItem("Rename", "F2"))
                            BeginRename(state, row.entity, row.label);
                        // ImGui cannot open a popup from inside another popup's
                        // scope, so the request is latched and consumed at panel
                        // scope below (the standard deferred-OpenPopup pattern).
                        if (ImGui::MenuItem("Add Component..."))
                            state.addComponentPending = true;
```

- [ ] **Step 2: Consume the latch at panel scope**

In `DrawOutlinerPanel`, replace:

```cpp
            ImGui::EndPopup();
        }

        std::size_t total = 0;
```

(the end of the empty-space `##outliner_ctx` popup, around line 704) with:

```cpp
            ImGui::EndPopup();
        }

        // Latched by the row menu one step earlier -- see the comment there.
        // The right-clicked row is already in the selection (the row's
        // right-click handler selects it when it was outside), so the popup
        // operates on exactly what the user aimed at.
        if (state.addComponentPending)
        {
            state.addComponentPending = false;
            ImGui::OpenPopup(kAddComponentPopup);
        }
        DrawAddComponentPopup(registry, sel.Entities(), undo, binding);

        std::size_t total = 0;
```

- [ ] **Step 3: Build**

From `D:\dev\starworks\Gacha\Arcane`:

```
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
```

Expected: build succeeds with 0 errors.

- [ ] **Step 4: Prove EditorPanels.cpp recompiled**

From `D:\dev\starworks\Gacha\Arcane`:

```powershell
Get-ChildItem bin-int\Debug-windows-x86_64-md\ArcaneEditor\EditorPanels.obj, ArcaneEditor\src\EditorPanels.cpp | Select-Object Name, LastWriteTime
```

Expected: the `.obj` timestamp is LATER than the `.cpp` timestamp.

- [ ] **Step 5: Run the full gate under both fixed seeds**

From `D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests`:

```
.\ArcaneTests.exe ~[gpu] --rng-seed 6
.\ArcaneTests.exe ~[gpu] --rng-seed 17
```

Expected: `All tests passed` both times.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(arcane-editor): Outliner row menu opens the Add Component popup (slice 4 task 4)"
```

---

### Task 5: Spec + memory + desk-verify checklist

**Files:**
- Modify: `docs/superpowers/specs/2026-07-25-editor-outliner-design.md` (section 5)
- Modify: `.superpowers/sdd/progress.md`

**Interfaces:**
- Consumes: the delivered behavior of Tasks 1-4.
- Produces: the record slice 5 (or the merge) reads.

- [ ] **Step 1: Amend the spec with what slice 4 actually decided**

In `docs/superpowers/specs/2026-07-25-editor-outliner-design.md`, append to section 5
(after the existing two bullets, before `## 6. Slices`):

```markdown
**Slice-4 amendments (2026-07-25, as built):**
- The hide-list is ONE predicate, `Arcane::Editor::IsSystemManagedComponent`
  (`ArcaneEditor/src/ComponentCatalog.hpp`). The Inspector's section filter, the
  Add catalog, and Remove Component all consult it, so they cannot drift.
- Unreflected components are omitted from the catalog: the Inspector can only
  render reflected types, so adding one would be an invisible edit the header
  menu could never undo through the UI.
- Tag (empty) components now DO get an Inspector header, showing
  "(tag component -- no fields)". Without one they were invisible and therefore
  unremovable. The multi-select intersection test switched from
  `GetComponentByHash` to `HasComponentByHash` for the same reason -- the getter
  returns null for a tag component the entity really carries.
- A catalog row whose `missingCount` is 0 (every selected entity already has it)
  renders disabled rather than hidden.
- Removal is deferred past the `InspectEntity` loop: the archetype move dangles
  every `ci.data` pointer in the vector being iterated.
- `OutlinerBinding` was renamed `SceneEditBinding` -- two panels share it now.
```

- [ ] **Step 2: Ledger the slice in the SDD progress file**

Append to `.superpowers/sdd/progress.md` a `SLICE 4` section recording, per task: the
commit sha, the gate result (assertions/cases and the seeds used), and the honest
verification status of the ImGui files (obj-timestamp only, no automated coverage).

- [ ] **Step 3: Write the desk-verify checklist into the same ledger entry**

Record these as the outstanding human checks (the panel has no automated coverage by
design):

1. Select one entity, `+ Add Component`, type `spr`, pick `Arcane::SpriteRenderer` ->
   the section appears; Ctrl+Z removes it; Ctrl+Y re-adds it.
2. Multi-select two entities where only one has SpriteRenderer -> the catalog row is
   ENABLED (missingCount 1); adding gives both the component as ONE undo step.
3. Multi-select two entities that both already have Transform -> the `Arcane::Transform`
   row is DISABLED and clicking it does nothing (no undo entry appears).
4. `Arcane::WorldTransform`, `Arcane::PreviousTransform` and `Arcane::PhysicsBodyRef`
   never appear in the catalog and have no Inspector section.
5. Right-click a component header -> Remove Component; confirm Transform IS removable and
   the entity's gizmo simply disappears (no crash); Ctrl+Z restores it.
6. Toggle the eye on an entity (adds `Hidden`) -> an `Arcane::Hidden` header appears
   reading "(tag component -- no fields)" and its Remove Component works.
7. Press Play, then confirm `+ Add Component` is greyed out and the header context menu's
   Remove Component is greyed out.
8. Outliner: right-click a row -> Add Component... -> the same searchable popup opens and
   targets the selection.
9. Right-click a row that is OUTSIDE the current selection -> it becomes the selection
   first, and Add Component... targets only it.
10. With nothing selected, the Inspector still shows "No selection" and no button.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-07-25-editor-outliner-design.md .superpowers/sdd/progress.md
git commit -m "docs(arcane-editor): slice 4 spec amendments + SDD ledger + desk-verify checklist"
```

---

## Verification Summary

| What | How | Where proven |
|---|---|---|
| Hide-list correctness | Catch2 `[editor][outliner]` | Task 1 |
| Catalog filtering / sorting / dead-entry tolerance | Catch2 | Task 1 |
| `missingCount` == what `Edit::AddComponent` touches | Catch2 | Task 1 |
| Tag components counted via `HasComponentByHash` | Catch2 regression case | Task 1 |
| Add/Remove round-trip through undo/redo | Catch2 over `ApplyRegistryMutation` | Task 1 |
| No-op add pushes no undo step | Catch2 | Task 1 |
| `EditorPanels.cpp` / `EditorApp.cpp` compile | `.obj` postdates `.cpp` | Tasks 2, 3, 4 |
| Popup behavior, menus, disabled states, Play gating | **DESK-VERIFY ONLY** | Task 5 checklist |
| `[gpu]` suites | **DEFERRED** (driver crashes on this box) | n/a |
