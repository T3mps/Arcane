# Editor Outliner Slice 2: SelectionContext set+primary + Outliner panel

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the flat Hierarchy panel with the UE-shaped Outliner (tree rows, eye toggle, search, rename, create/delete/reparent) driven by slice 1's EntityOps + RegistryStateCommand, and turn SelectionContext into an ordered multi-select set + primary (every consumer stays single-entity via `Primary()` until slice 3).

**Architecture:** Pure-core + ImGui-shell split, exactly like AssetBrowser: `BuildOutlinerRows` (headless, tested) produces flat depth-annotated rows; `DrawOutlinerPanel` (EditorPanels.cpp, untested shell) draws them and routes every structural edit through `ApplyRegistryMutation` bound to `Runtime::SnapshotRegistry/RestoreRegistry`. Structural edits are **Edit-mode-only** (disabled during Play) — this is the slice-2 resolution of the RegistryStateCommand.hpp OPEN note: plugin-hosted native state is (re)built by PlaySession at Play start from the current registry, so a raw registry restore in Edit mode has nothing to reconcile.

**Tech Stack:** C++23, Dear ImGui (tables + TreeNodeEx), Astra ECS (relationship graph), Catch2. Spec: `docs/superpowers/specs/2026-07-25-editor-outliner-design.md` §3 + slice list §6.2.

## Global Constraints

- /MD runtime everywhere in the Arcane workspace; UTF-8 without BOM; ASCII comments; no `/fp:fast`.
- Tests run FROM THE EXE DIR: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`. Running from anywhere else false-fails PluginHost/Msdfgen.
- The dev-loop gate is `ArcaneTests.exe "~[gpu]"` (windowed/[gpu] suites are desk-only on this machine).
- New files => re-run `Arcane\GenerateProjects.bat` before building (premake globs). Only Task 3 adds a file.
- MSBuild: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo` from `Arcane\`.
- Commit messages with special characters: write to a scratch file, `git commit -F <file>` (PS 5.1 quoting).
- ABI rule: nothing in this slice changes an existing layout/slot. Task 1 edits function BODIES and docs in Arcane.dll (`RenameEntity`, `RegistryStateCommand::Undo/Redo` internals — `m_redoLost` is a new private member of a non-virtual-layout... **careful**: `RegistryStateCommand` IS exported with a vtable (ICommand) and gains a data member. It is created and consumed only inside Arcane.dll (`ApplyRegistryMutation` is the only creator) and never crosses the plugin ABI — no PluginABI.hpp entry needed, but say so in the commit message. Everything else is editor-exe-side.
- Desk-verify (interactive panel behavior) is the USER's step, not CI's. Every task still lands headless-green.

---

### Task 1: Engine ride-alongs from the slice-1 final review (Edit/ polish)

Four small items the slice-1 final review deferred to this slice, all in `Arcane/Edit/`.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp` (RenameEntity ~:165-177, CreateEntity doc site ~:56)
- Modify: `Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp` (RenameEntity + CreateEntity doc comments)
- Modify: `Arcane/Arcane/src/Arcane/Edit/RegistryStateCommand.cpp` (Undo/Redo ~:22-44)
- Modify: `Arcane/Arcane/src/Arcane/Edit/RegistryStateCommand.hpp` (m_redoLost member; mutate() contract sentence; OPEN note -> DECIDED)
- Test: `Arcane/Tests/src/EntityOpsTest.cpp`, `Arcane/Tests/src/RegistryStateCommandTest.cpp` (extend, no new files)

**Interfaces:**
- Consumes: everything already on the branch (slice 1 as merged).
- Produces: `Edit::RenameEntity` returns **false** on a same-name no-op rename (Tasks 4/5 rely on this so inline rename never pollutes the undo history). No other signature changes.

- [ ] **Step 1: Write the failing tests.** Append to `Arcane/Tests/src/EntityOpsTest.cpp`:

```cpp
TEST_CASE("Same-name rename is a no-op and reports false", "[outliner]")
{
    // Inline rename commits on defocus even when the user changed nothing;
    // reporting false lets ApplyRegistryMutation skip the memento push so
    // the undo history never records a step that reverts nothing.
    World w;
    Astra::Entity e = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    REQUIRE(Edit::RenameEntity(w.reg, e, "Hero"));
    CHECK_FALSE(Edit::RenameEntity(w.reg, e, "Hero"));   // unchanged -> false
    CHECK(Edit::RenameEntity(w.reg, e, "Hero2"));        // real change -> true
    CHECK(w.reg.GetComponent<EntityInfo>(e)->name == "Hero2");
}

TEST_CASE("CreateEntity under a dead parent creates at root", "[outliner]")
{
    // Documented fallback (slice-1 final review, Minor #2): a stale parent
    // handle silently no-ops in Registry::SetParent, so the entity lands
    // unparented. Pin the behavior the header now documents.
    World w;
    Astra::Entity parent = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    w.reg.DestroyEntity(parent);
    Astra::Entity e = Edit::CreateEntity(w.reg, parent);
    CHECK(w.reg.IsValid(e));
    CHECK_FALSE(w.reg.GetParent(e).IsValid());
}
```

Append to `Arcane/Tests/src/RegistryStateCommandTest.cpp`:

```cpp
TEST_CASE("failed redo-capture latches: redo stays a warned no-op", "[outliner]")
{
    // Slice-1 final review Minor #3: without a latch, a second Undo after a
    // failed after-capture re-attempts the capture and silently snapshots
    // the already-restored BEFORE state as the redo target. The latch keeps
    // redo an honest no-op forever. Observable pin: state stays correct
    // through undo/redo/undo/redo with a snapshot fn that fails after the
    // initial (ApplyRegistryMutation-time) call.
    World w;
    Astra::Entity a = Edit::CreateEntity(*w.reg, Astra::Entity::Invalid());

    int calls = 0;
    auto flakySnapshot = [&]() -> std::vector<std::byte>
    {
        ++calls;
        if (calls > 1)
            return {};                    // every capture after the first fails
        auto r = w.reg->Save();
        return r.IsOk() ? std::move(*r) : std::vector<std::byte>{};
    };

    REQUIRE(ApplyRegistryMutation(w.stack, "Hide", flakySnapshot, w.Restore(),
        [&] { return Edit::SetHiddenRecursive(*w.reg, a, true) > 0; }));
    CHECK(w.reg->GetComponent<Hidden>(a) != nullptr);

    w.stack.Undo();                                       // capture fails, restore works
    CHECK(w.reg->GetComponent<Hidden>(a) == nullptr);
    w.stack.Redo();                                       // warned no-op
    CHECK(w.reg->GetComponent<Hidden>(a) == nullptr);
    w.stack.Undo();                                       // must NOT re-capture
    CHECK(w.reg->GetComponent<Hidden>(a) == nullptr);
    w.stack.Redo();                                       // still a no-op
    CHECK(w.reg->GetComponent<Hidden>(a) == nullptr);
}
```

Note: `CommandStack::Redo()` moves the command back to the undo deque regardless of what `ICommand::Redo()` does internally — that is existing behavior and fine here; the pin is that the REGISTRY state never silently changes.

- [ ] **Step 2: Build, run, verify the new tests fail** (same-name test fails on `CHECK_FALSE`; latch test may pass by accident on state alone — verify it at least runs; its value is behavioral honesty + the code change below). From the exe dir: `.\ArcaneTests.exe "[outliner]"`.

- [ ] **Step 3: Implement.**

`EntityOps.cpp` — RenameEntity (currently :165-177) becomes:

```cpp
    bool RenameEntity(Astra::Registry& reg, Astra::Entity e, std::string name)
    {
        if (!reg.IsValid(e))
            return false;
        if (EntityInfo* info = reg.GetComponent<EntityInfo>(e))
        {
            if (info->name == name)
                return false;   // no-op rename: no change, no memento
            info->name = std::move(name);
            return true;
        }
        reg.AddComponent<EntityInfo>(e, EntityInfo{ Guid::Generate(),
                                                    std::move(name) });
        return true;
    }
```

`EntityOps.hpp` — RenameEntity doc gains: `Returns false for a dead entity OR when the name is already exactly `name` (no-op).` CreateEntity doc gains: `A dead (stale) parent handle silently falls back to root creation -- Registry::SetParent no-ops on dead parents and a create is still a real edit worth keeping.`

`RegistryStateCommand.hpp` — add member below `m_after` (private section): `bool m_redoLost = false;   // first failed after-capture latches: redo stays a warned no-op`. In the `ApplyRegistryMutation` doc block add: `mutate() must be all-or-nothing: it must NOT partially mutate the registry and then return false -- a false return means "nothing changed" and skips the undo push entirely.` Replace the `OPEN (slice 2 decides)` paragraph with:

```cpp
// DECIDED (slice 2): the editor's structural-edit binding is EDIT-MODE-ONLY
// (the Outliner disables structural ops while Play is running), so a raw
// registry restore never has plugin-hosted native state (physics worlds,
// cached handles) to reconcile -- PlaySession rebuilds that state at Play
// start from the current registry. If a future plugin ever holds edit-time
// native state, add a post-restore reconcile hook then.
```

`RegistryStateCommand.cpp` — Undo/Redo (currently :22-44) become:

```cpp
    void RegistryStateCommand::Undo()
    {
        if (m_after.empty() && !m_redoLost)
        {
            m_after = m_snapshot();
            if (m_after.empty())
            {
                // Latch: retrying on a later Undo would capture the restored
                // BEFORE state and make redo "succeed" silently while
                // restoring the state the registry is already in.
                m_redoLost = true;
                ARC_WARN("'{}': redo-state capture failed -- undo proceeds, "
                         "redo will be unavailable", m_label);
            }
        }
        if (!m_restore(m_before))
            ARC_WARN("'{}': registry restore failed on undo", m_label);
    }

    void RegistryStateCommand::Redo()
    {
        if (m_redoLost || m_after.empty())
        {
            ARC_WARN("'{}': no redo state captured -- redo skipped", m_label);
            return;
        }
        if (!m_restore(m_after))
            ARC_WARN("'{}': registry restore failed on redo", m_label);
    }
```

- [ ] **Step 4: Build + run.** `.\ArcaneTests.exe "[outliner]"` from the exe dir — expect all green (currently 18 cases + 3 new).

- [ ] **Step 5: Commit.** `fix(arcane): Edit ride-alongs from slice-1 review (same-name rename no-op, redo-lost latch, docs)` — note in the body: RegistryStateCommand gains a private member; the type never crosses the plugin ABI (created/consumed inside Arcane.dll only), no PluginABI ledger entry needed.

---

### Task 2: SelectionContext becomes ordered set + primary; every consumer switches to Primary()

Mechanical slice: multi-select STORAGE lands, behavior stays single-select everywhere (the panel starts using Toggle/AddRange in Task 4; the viewport stays plain-click-replace until slice 3).

**Files:**
- Rewrite: `Arcane/ArcaneEditor/src/SelectionContext.hpp`
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (:366-368 Hierarchy row; :566-572, :595 Inspector)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (:887, :894, :899 gizmo drag; :1077-1080 gizmo draw; :1134-1141 outline; :1289-1295 pick — pick needs no code change, verify only)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (:92 stale comment)
- Test: `Arcane/Tests/src/EditorEntityListTest.cpp` (replace the SelectionContext cases)

**Interfaces:**
- Produces (Tasks 4/5 and slice 3 rely on these exact names):
  `bool HasSelection()`, `std::size_t Count()`, `Astra::Entity Primary()`,
  `const std::vector<Astra::Entity>& Entities()`, `bool Contains(Astra::Entity)`,
  `void Select(Astra::Entity)` (replace-all), `void Toggle(Astra::Entity)`,
  `void AddRange(std::span<const Astra::Entity>, Astra::Entity primary)`,
  `void Clear()`, `template<typename F> void Prune(F&& alive)`.
- The old public field `selected` is GONE — anything still reading it is a compile error, which is the point.

- [ ] **Step 1: Write the failing tests.** In `Arcane/Tests/src/EditorEntityListTest.cpp`, replace the existing SelectionContext TEST_CASE(s) (currently around :30 and :45 — keep the CollectEntities cases untouched, Task 4 handles those) with:

```cpp
TEST_CASE("SelectionContext: Select replaces, Toggle adds/removes, primary follows", "[editor]")
{
    Arcane::Editor::SelectionContext sel;
    auto reg = MakeSceneRegistry();
    Astra::Entity a = reg->CreateEntity();
    Astra::Entity b = reg->CreateEntity();
    Astra::Entity c = reg->CreateEntity();

    CHECK_FALSE(sel.HasSelection());
    CHECK_FALSE(sel.Primary().IsValid());

    sel.Select(a);
    CHECK(sel.Count() == 1);
    CHECK(sel.Primary() == a);
    CHECK(sel.Contains(a));

    sel.Toggle(b);                       // ctrl-click add
    CHECK(sel.Count() == 2);
    CHECK(sel.Primary() == b);
    CHECK(sel.Contains(a));

    sel.Toggle(b);                       // ctrl-click remove -> primary falls back
    CHECK(sel.Count() == 1);
    CHECK(sel.Primary() == a);

    sel.Select(c);                       // plain click replaces everything
    CHECK(sel.Count() == 1);
    CHECK(sel.Primary() == c);
    CHECK_FALSE(sel.Contains(a));

    sel.Clear();
    CHECK_FALSE(sel.HasSelection());
}

TEST_CASE("SelectionContext: AddRange appends without duplicates; Prune sweeps dead", "[editor]")
{
    Arcane::Editor::SelectionContext sel;
    auto reg = MakeSceneRegistry();
    Astra::Entity a = reg->CreateEntity();
    Astra::Entity b = reg->CreateEntity();
    Astra::Entity c = reg->CreateEntity();

    sel.Select(a);
    const std::array<Astra::Entity, 3> range{ a, b, c };   // shift-range includes the anchor
    sel.AddRange(range, c);
    CHECK(sel.Count() == 3);                               // a not duplicated
    CHECK(sel.Primary() == c);

    reg->DestroyEntity(c);
    sel.Prune([&](Astra::Entity e) { return reg->IsValid(e); });
    CHECK(sel.Count() == 2);
    CHECK(sel.Primary() == b);                             // fell back to most recent live
    CHECK_FALSE(sel.Contains(c));
}
```

(`MakeSceneRegistry` already exists in this file at :21-27. Add `#include <array>` and `#include <span>` if missing.)

- [ ] **Step 2: Build ArcaneTests — expect COMPILE FAILURE** (`Primary` not a member). That is the failing state for a storage rewrite.

- [ ] **Step 3: Rewrite `SelectionContext.hpp`** (whole file):

```cpp
#pragma once

// The selected-entity source of truth, shared by outliner, inspector, and
// viewport pick. Ordered multi-select: Entities() keeps selection order
// (front = oldest), Primary() is the last-clicked member -- the entity every
// single-entity consumer (gizmo anchor, inspector, outline) operates on.
// Slice 2 keeps all consumers single-entity via Primary(); slice 3 (full
// multi-edit) fans them out over Entities().
//
// No registry access here: dead entries are swept by Prune() (EditorApp,
// once per frame) and tolerated everywhere else -- EntityOps set ops skip
// dead entities by contract.

#include <Astra/Entity/Entity.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace Arcane::Editor
{
    struct SelectionContext
    {
        using EntityT = Astra::Entity;

        [[nodiscard]] bool HasSelection() const noexcept { return !m_entities.empty(); }
        [[nodiscard]] std::size_t Count() const noexcept { return m_entities.size(); }
        [[nodiscard]] Astra::Entity Primary() const noexcept { return m_primary; }
        [[nodiscard]] const std::vector<Astra::Entity>& Entities() const noexcept { return m_entities; }
        [[nodiscard]] bool Contains(Astra::Entity e) const noexcept
        {
            return std::find(m_entities.begin(), m_entities.end(), e) != m_entities.end();
        }

        // Plain click: selection becomes exactly { e }.
        void Select(Astra::Entity e)
        {
            m_entities.assign(1, e);
            m_primary = e;
        }

        // Ctrl-click: add (becomes primary) or remove (primary falls back to
        // the most recently selected remaining entry).
        void Toggle(Astra::Entity e)
        {
            auto it = std::find(m_entities.begin(), m_entities.end(), e);
            if (it == m_entities.end())
            {
                m_entities.push_back(e);
                m_primary = e;
            }
            else
            {
                m_entities.erase(it);
                m_primary = m_entities.empty() ? Astra::Entity::Invalid()
                                               : m_entities.back();
            }
        }

        // Shift-range: append `range` in visible-row order, skipping entries
        // already selected; `primary` becomes the primary (the clicked row).
        void AddRange(std::span<const Astra::Entity> range, Astra::Entity primary)
        {
            for (Astra::Entity e : range)
                if (!Contains(e))
                    m_entities.push_back(e);
            if (primary.IsValid())
                m_primary = primary;
        }

        void Clear() noexcept
        {
            m_entities.clear();
            m_primary = Astra::Entity::Invalid();
        }

        // Sweep entries the registry no longer recognizes (after a structural
        // undo/redo swapped the registry object). Primary falls back like
        // Toggle-removal. `alive` is injected so this header stays free of
        // registry includes: sel.Prune([&](Astra::Entity e){ return reg.IsValid(e); });
        template<typename IsAliveFn>
        void Prune(IsAliveFn&& alive)
        {
            std::erase_if(m_entities,
                          [&](Astra::Entity e) { return !alive(e); });
            if (!m_entities.empty() && !Contains(m_primary))
                m_primary = m_entities.back();
            else if (m_entities.empty())
                m_primary = Astra::Entity::Invalid();
        }

    private:
        std::vector<Astra::Entity> m_entities;
        Astra::Entity m_primary = Astra::Entity::Invalid();
    };
}
```

- [ ] **Step 4: Switch every consumer.** Exact edits:

`EditorPanels.cpp:366-368` (Hierarchy row — still alive until Task 4):
```cpp
        const bool isSel = sel.Contains(e);
        if (ImGui::Selectable(label, isSel))
            sel.Select(e);
```

`EditorPanels.cpp` Inspector (:566-572, :595): `sel.HasSelection()` guard is unchanged; `registry.InspectEntity(sel.selected)` -> `registry.InspectEntity(sel.Primary())`; `visitor.entity = sel.selected;` -> `visitor.entity = sel.Primary();`

`EditorApp.cpp` gizmo drag (:887, :894, :899): `m_selection.selected` -> `m_selection.Primary()` (three sites; the local `const Astra::Entity sel = m_selection.selected;` becomes `= m_selection.Primary();`).

`EditorApp.cpp` gizmo draw (:1077-1080): `GetComponent<Arcane::Transform>(m_selection.selected)` -> `(m_selection.Primary())`.

`EditorApp.cpp` outline (:1134-1141): `m_pick->PassIdOf(m_selection.selected)` -> `m_pick->PassIdOf(m_selection.Primary())`.

`EditorApp.cpp` pick (:1289-1295): already uses `Select`/`Clear` — no change; verify it compiles.

`EditorApp.hpp:92` comment: "The one selected-entity source of truth" -> "Ordered multi-select source of truth (set + primary); slice-2 consumers operate on Primary()".

- [ ] **Step 5: Build + run.** Full editor suite from the exe dir: `.\ArcaneTests.exe "[editor]"` — all green. Also build the whole solution (ArcaneEditor must compile — it is not covered by tests).

- [ ] **Step 6: Commit.** `feat(arcane-editor): SelectionContext ordered set + primary (outliner arc, slice 2a)`

---

### Task 3: BuildOutlinerRows + RowRange (pure core) + tests

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EntityList.hpp` (keep `CollectEntities` for now; add the Outliner core)
- Modify: `Arcane/ArcaneEditor/src/EntityList.cpp`
- Create: `Arcane/Tests/src/EditorOutlinerTest.cpp` (NEW FILE — run `Arcane\GenerateProjects.bat` after creating it)

**Interfaces:**
- Consumes: `Edit::DisplayName(Astra::Registry&, Astra::Entity)` (Arcane.dll — the test fixture therefore needs the TypeContext pin idiom), `reg.HasComponent<T>`, `reg.GetChildren/GetChildCount/HasParent/GetEntityManager`.
- Produces (Task 4 relies on exactly these):

```cpp
struct OutlinerRow
{
    Astra::Entity entity;
    int           depth = 0;
    std::string   label;          // Edit::DisplayName
    std::string   type;           // priority table below
    bool          hidden = false; // carries Arcane::Hidden
    bool          dimmed = false; // kept only as a matching descendant's ancestor
    bool          hasChildren = false;
    std::size_t   childCount = 0;
};
struct OutlinerSort
{
    enum class Column { None, Label, Type };
    Column column = Column::None;   // None = authored tree order
    bool   ascending = true;
};
std::vector<OutlinerRow> BuildOutlinerRows(Astra::Registry& reg,
                                           std::string_view filter,
                                           const OutlinerSort& sort,
                                           const std::unordered_set<std::uint64_t>& collapsed);
std::vector<Astra::Entity> RowRange(std::span<const OutlinerRow> rows,
                                    Astra::Entity a, Astra::Entity b);
```

Semantics (copy into the header doc):
- Roots = entities without a parent, in EntityManager order; children in `GetChildren` order; depth-first emission.
- `sort` reorders SIBLING groups (and roots) by label or type, case-insensitive; tree structure is never broken.
- `filter`: case-insensitive substring over labels. A match keeps itself AND all its ancestors; kept non-matching ancestors get `dimmed = true`. When `filter` is non-empty the `collapsed` set is ignored (search auto-expands).
- `collapsed` (entity `GetValue()`s): a collapsed row is emitted, its descendants are not.
- `RowRange`: inclusive span of `rows` between the rows of `a` and `b` (either order); empty if either entity has no row.

- [ ] **Step 1: Write the failing tests.** Create `Arcane/Tests/src/EditorOutlinerTest.cpp`:

```cpp
// Outliner slice 2: the headless row builder behind the Outliner panel.
// Cross-DLL note: BuildOutlinerRows calls Edit::DisplayName (Arcane.dll),
// so the fixture pins Arcane.dll's TypeContext slot to the shared test
// context -- same idiom as EntityOpsTest.

#include <catch2/catch_test_macros.hpp>

#include "EntityList.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

using namespace Arcane;
using Arcane::Editor::BuildOutlinerRows;
using Arcane::Editor::OutlinerRow;
using Arcane::Editor::OutlinerSort;
using Arcane::Editor::RowRange;

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        World()
        {
            static const bool s_ctxPinned = []
            {
                Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
                return true;
            }();
            (void)s_ctxPinned;
            RegisterSceneComponents(reg);
            RegisterPhysicsComponents(reg);
        }
        Astra::Entity Make(const char* name, Astra::Entity parent = Astra::Entity::Invalid())
        {
            Astra::Entity e = Edit::CreateEntity(reg, parent);
            Edit::RenameEntity(reg, e, name);
            return e;
        }
    };

    const std::unordered_set<std::uint64_t> kNoneCollapsed;
    const OutlinerSort kNoSort;
}

TEST_CASE("Rows come out in depth-first tree order with depths", "[editor][outliner]")
{
    World w;
    Astra::Entity a = w.Make("A");
    Astra::Entity b = w.Make("B", a);
    Astra::Entity c = w.Make("C", a);
    Astra::Entity d = w.Make("D", c);

    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, kNoneCollapsed);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].entity == a); CHECK(rows[0].depth == 0);
    CHECK(rows[1].entity == b); CHECK(rows[1].depth == 1);
    CHECK(rows[2].entity == c); CHECK(rows[2].depth == 1);
    CHECK(rows[3].entity == d); CHECK(rows[3].depth == 2);
    CHECK(rows[0].hasChildren); CHECK(rows[0].childCount == 2);
    CHECK_FALSE(rows[1].hasChildren);
}

TEST_CASE("Labels use DisplayName; hidden flag rides Arcane::Hidden", "[editor][outliner]")
{
    World w;
    Astra::Entity named = w.Make("Player");
    Astra::Entity anon = w.reg.CreateEntity();            // no EntityInfo at all
    w.reg.AddComponent<Hidden>(named, Hidden{});

    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, kNoneCollapsed);
    REQUIRE(rows.size() == 2);
    auto find = [&](Astra::Entity e) -> const OutlinerRow&
    {
        for (const auto& r : rows) if (r.entity == e) return r;
        FAIL("row missing"); return rows[0];
    };
    CHECK(find(named).label == "Player");
    CHECK(find(named).hidden);
    CHECK(find(anon).label == Edit::DisplayName(w.reg, anon));   // "Entity <id>" fallback
    CHECK_FALSE(find(anon).hidden);
}

TEST_CASE("Type column follows the priority table", "[editor][outliner]")
{
    World w;
    Astra::Entity plain = w.Make("P");
    Astra::Entity sprite = w.Make("S");
    w.reg.AddComponent<SpriteRenderer>(sprite, SpriteRenderer{});
    Astra::Entity body = w.Make("B");
    w.reg.AddComponent<SpriteRenderer>(body, SpriteRenderer{});
    w.reg.AddComponent<RigidBody2D>(body, RigidBody2D{});
    Astra::Entity post = w.Make("PP");
    w.reg.AddComponent<PostProcess>(post, PostProcess{});
    w.reg.AddComponent<RigidBody2D>(post, RigidBody2D{});

    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, kNoneCollapsed);
    auto typeOf = [&](Astra::Entity e) -> std::string
    {
        for (const auto& r : rows) if (r.entity == e) return r.type;
        return "<missing>";
    };
    CHECK(typeOf(plain) == "Entity");
    CHECK(typeOf(sprite) == "Sprite");
    CHECK(typeOf(body) == "Rigid Body");        // RigidBody2D outranks Sprite
    CHECK(typeOf(post) == "Post Process");      // PostProcess outranks everything
}

TEST_CASE("Filter keeps matches and their ancestors; ancestors dim", "[editor][outliner]")
{
    World w;
    Astra::Entity root = w.Make("Root");
    Astra::Entity mid = w.Make("Middle", root);
    Astra::Entity hit = w.Make("Treasure", mid);
    w.Make("Noise", root);
    w.Make("Static");

    auto rows = BuildOutlinerRows(w.reg, "TREAS", kNoSort, kNoneCollapsed);   // case-insensitive
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].entity == root); CHECK(rows[0].dimmed);
    CHECK(rows[1].entity == mid);  CHECK(rows[1].dimmed);
    CHECK(rows[2].entity == hit);  CHECK_FALSE(rows[2].dimmed);
}

TEST_CASE("Collapsed rows keep themselves, drop descendants; filter overrides", "[editor][outliner]")
{
    World w;
    Astra::Entity a = w.Make("A");
    Astra::Entity b = w.Make("B", a);
    w.Make("C", b);

    std::unordered_set<std::uint64_t> collapsed{ static_cast<std::uint64_t>(a.GetValue()) };
    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, collapsed);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].entity == a);
    CHECK(rows[0].hasChildren);                 // arrow still drawable

    rows = BuildOutlinerRows(w.reg, "C", kNoSort, collapsed);   // search auto-expands
    REQUIRE(rows.size() == 3);
}

TEST_CASE("Sort reorders sibling groups without breaking the tree", "[editor][outliner]")
{
    World w;
    Astra::Entity zeta = w.Make("Zeta");
    Astra::Entity alpha = w.Make("alpha");      // case-insensitive: sorts before Zeta
    w.Make("z-child", alpha);
    w.Make("a-child", alpha);

    OutlinerSort byLabel{ OutlinerSort::Column::Label, true };
    auto rows = BuildOutlinerRows(w.reg, "", byLabel, kNoneCollapsed);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].entity == alpha);
    CHECK(rows[1].label == "a-child"); CHECK(rows[1].depth == 1);
    CHECK(rows[2].label == "z-child"); CHECK(rows[2].depth == 1);
    CHECK(rows[3].entity == zeta);

    OutlinerSort desc{ OutlinerSort::Column::Label, false };
    rows = BuildOutlinerRows(w.reg, "", desc, kNoneCollapsed);
    CHECK(rows[0].entity == zeta);
}

TEST_CASE("RowRange spans visible rows inclusively, either direction", "[editor][outliner]")
{
    World w;
    Astra::Entity a = w.Make("A");
    Astra::Entity b = w.Make("B");
    Astra::Entity c = w.Make("C");
    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, kNoneCollapsed);

    auto fwd = RowRange(rows, a, c);
    REQUIRE(fwd.size() == 3);
    CHECK(fwd.front() == a); CHECK(fwd.back() == c);

    auto rev = RowRange(rows, c, a);
    REQUIRE(rev.size() == 3);
    CHECK(rev.front() == a); CHECK(rev.back() == c);

    CHECK(RowRange(rows, a, Astra::Entity::Invalid()).empty());
    CHECK(RowRange(rows, b, b).size() == 1);
}
```

- [ ] **Step 2: Run `Arcane\GenerateProjects.bat`, build — expect COMPILE FAILURE** (`BuildOutlinerRows` undeclared).

- [ ] **Step 3: Implement.** `EntityList.hpp` becomes (keep the existing `CollectEntities` declaration and its comment for now — Task 4 deletes it):

```cpp
#pragma once

// Outliner core (pure, headless-tested; the ImGui shell lives in
// EditorPanels.cpp) -- flat depth-annotated rows over the relationship
// graph, plus the legacy flat CollectEntities (deleted when the old
// Hierarchy panel goes).
//
// Row semantics:
// - Roots = entities without a parent, in EntityManager order; children in
//   GetChildren order; depth-first emission.
// - `sort` reorders SIBLING groups (and roots) case-insensitively by label
//   or type; the tree structure is never broken.
// - `filter`: case-insensitive substring over labels. A match keeps itself
//   AND all its ancestors; kept non-matching ancestors get dimmed = true.
//   A non-empty filter ignores `collapsed` (search auto-expands).
// - `collapsed` (entity GetValue()s): the row is emitted, descendants not.

#include <Astra/Entity/Entity.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    std::vector<Astra::Entity> CollectEntities(Astra::Registry& registry);

    struct OutlinerRow
    {
        Astra::Entity entity;
        int           depth = 0;
        std::string   label;
        std::string   type;
        bool          hidden = false;
        bool          dimmed = false;
        bool          hasChildren = false;
        std::size_t   childCount = 0;
    };

    struct OutlinerSort
    {
        enum class Column { None, Label, Type };
        Column column = Column::None;
        bool   ascending = true;
    };

    std::vector<OutlinerRow> BuildOutlinerRows(Astra::Registry& reg,
                                               std::string_view filter,
                                               const OutlinerSort& sort,
                                               const std::unordered_set<std::uint64_t>& collapsed);

    // Inclusive visible-row span between a and b (either order); empty when
    // either has no row. Backs shift-range selection.
    std::vector<Astra::Entity> RowRange(std::span<const OutlinerRow> rows,
                                        Astra::Entity a, Astra::Entity b);
}
```

`EntityList.cpp` (keep `CollectEntities` as-is, append):

```cpp
#include "EntityList.hpp"

#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cctype>

namespace
{
    bool ContainsCI(std::string_view hay, std::string_view needle)
    {
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

    bool LessCI(std::string_view a, std::string_view b)
    {
        auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            const char ca = lower(a[i]), cb = lower(b[i]);
            if (ca != cb)
                return ca < cb;
        }
        return a.size() < b.size();
    }

    // Priority table, first hit wins. Extend here when a new component type
    // deserves its own type-column string.
    std::string TypeLabel(Astra::Registry& reg, Astra::Entity e)
    {
        if (reg.HasComponent<Arcane::PostProcess>(e))    return "Post Process";
        if (reg.HasComponent<Arcane::RigidBody2D>(e))    return "Rigid Body";
        if (reg.HasComponent<Arcane::SpriteRenderer>(e)) return "Sprite";
        return "Entity";
    }
}

namespace Arcane::Editor
{
    std::vector<OutlinerRow> BuildOutlinerRows(Astra::Registry& reg,
                                               std::string_view filter,
                                               const OutlinerSort& sort,
                                               const std::unordered_set<std::uint64_t>& collapsed)
    {
        std::vector<OutlinerRow> rows;

        auto makeRow = [&](Astra::Entity e, int depth)
        {
            OutlinerRow r;
            r.entity = e;
            r.depth = depth;
            r.label = Edit::DisplayName(reg, e);
            r.type = TypeLabel(reg, e);
            r.hidden = reg.HasComponent<Hidden>(e);
            r.childCount = reg.GetChildCount(e);
            r.hasChildren = r.childCount > 0;
            return r;
        };

        auto sortKey = [&](Astra::Entity e) -> std::string
        {
            return sort.column == OutlinerSort::Column::Type ? TypeLabel(reg, e)
                                                             : Edit::DisplayName(reg, e);
        };
        auto sortSiblings = [&](std::vector<Astra::Entity>& kids)
        {
            if (sort.column == OutlinerSort::Column::None)
                return;
            std::stable_sort(kids.begin(), kids.end(),
                [&](Astra::Entity a, Astra::Entity b)
                {
                    const std::string ka = sortKey(a), kb = sortKey(b);
                    return sort.ascending ? LessCI(ka, kb) : LessCI(kb, ka);
                });
        };

        auto emit = [&](this auto&& self, Astra::Entity e, int depth) -> void
        {
            rows.push_back(makeRow(e, depth));
            // Search auto-expands: a non-empty filter ignores collapse.
            if (filter.empty() && collapsed.contains(static_cast<std::uint64_t>(e.GetValue())))
                return;
            std::vector<Astra::Entity> kids = reg.GetChildren(e);
            sortSiblings(kids);
            for (Astra::Entity c : kids)
                self(c, depth + 1);
        };

        std::vector<Astra::Entity> roots;
        for (Astra::Entity e : reg.GetEntityManager())
            if (!reg.HasParent(e))
                roots.push_back(e);
        sortSiblings(roots);
        for (Astra::Entity r : roots)
            emit(r, 0);

        if (!filter.empty())
        {
            // Keep matches and their ancestor chains; dim kept non-matches.
            std::vector<char> match(rows.size(), 0), keep(rows.size(), 0);
            for (std::size_t i = 0; i < rows.size(); ++i)
                match[i] = ContainsCI(rows[i].label, filter) ? 1 : 0;
            std::vector<std::size_t> chain;   // indices of the current ancestor path
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                while (!chain.empty() && rows[chain.back()].depth >= rows[i].depth)
                    chain.pop_back();
                if (match[i])
                {
                    keep[i] = 1;
                    for (std::size_t a : chain)
                        keep[a] = 1;
                }
                chain.push_back(i);
            }
            std::vector<OutlinerRow> out;
            out.reserve(rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                if (!keep[i])
                    continue;
                rows[i].dimmed = !match[i];
                out.push_back(std::move(rows[i]));
            }
            rows = std::move(out);
        }
        return rows;
    }

    std::vector<Astra::Entity> RowRange(std::span<const OutlinerRow> rows,
                                        Astra::Entity a, Astra::Entity b)
    {
        std::size_t ia = rows.size(), ib = rows.size();
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].entity == a) ia = i;
            if (rows[i].entity == b) ib = i;
        }
        if (ia == rows.size() || ib == rows.size())
            return {};
        const auto [lo, hi] = std::minmax(ia, ib);
        std::vector<Astra::Entity> out;
        out.reserve(hi - lo + 1);
        for (std::size_t i = lo; i <= hi; ++i)
            out.push_back(rows[i].entity);
        return out;
    }
}
```

(Note the existing `EntityList.cpp` already includes some of these headers for `CollectEntities` — merge, don't duplicate. `emit` uses C++23 deducing-this recursion; the workspace is /std:c++23.)

- [ ] **Step 4: Build + run.** `.\ArcaneTests.exe "[editor][outliner]"` then `.\ArcaneTests.exe "[editor]"` from the exe dir — all green.

- [ ] **Step 5: Commit.** `feat(arcane-editor): BuildOutlinerRows headless row builder (outliner arc, slice 2b)`

---

### Task 4: Outliner panel shell part A — table, search, selection, rename, eye, footer + EditorApp wiring

The old Hierarchy panel dies here; the Outliner window replaces it. Structural edits (rename, hide) go live through ApplyRegistryMutation.

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (remove `DrawHierarchyPanel` decl :87; add OutlinerBinding/OutlinerState/DrawOutlinerPanel)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (remove `DrawHierarchyPanel` :358-371; dock string :107 "Hierarchy" -> "Outliner"; add the panel)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (members: `OutlinerState m_outliner;` + `OutlinerBinding m_outlinerBinding;`)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (binding init after :300; per-frame Prune + editMode; call-site swap :1298; project-switch reset near :619)
- Modify: `Arcane/ArcaneEditor/src/EntityList.hpp/.cpp` (DELETE `CollectEntities` — the Outliner replaced its only consumer)
- Test: `Arcane/Tests/src/EditorEntityListTest.cpp` (delete the CollectEntities cases; SelectionContext cases stay)

**Interfaces:**
- Consumes: Task 2's SelectionContext, Task 3's rows/RowRange, `ApplyRegistryMutation`, `Edit::RenameEntity/SetHiddenRecursive/DeleteEntities/DisplayName`, `Runtime::SnapshotRegistry/RestoreRegistry`, ICON_LC_EYE (IconsLucide.h:676) / ICON_LC_EYE_OFF (:678) / ICON_LC_SEARCH (:1511).
- Produces (Task 5 extends the same panel):

```cpp
struct OutlinerBinding
{
    Arcane::RegistryStateCommand::SnapshotFn snapshot;
    Arcane::RegistryStateCommand::RestoreFn  restore;
    bool editMode = true;    // false during Play: structural edits disabled
};
struct OutlinerState
{
    char search[128] = {};
    std::unordered_set<std::uint64_t> collapsed;
    OutlinerSort sort;
    Astra::Entity renameTarget = Astra::Entity::Invalid();
    char renameBuf[256] = {};
    bool renameFocusPending = false;
    Astra::Entity lastClicked = Astra::Entity::Invalid();
    double lastClickTime = 0.0;
};
void DrawOutlinerPanel(Astra::Registry& registry, SelectionContext& sel,
                       Arcane::CommandStack& undo, const OutlinerBinding& binding,
                       OutlinerState& state);
```

- [ ] **Step 1: Header changes.** `EditorPanels.hpp`: delete the `DrawHierarchyPanel` declaration (:87). Add near it (needs `#include <Arcane/Edit/RegistryStateCommand.hpp>`, `#include "EntityList.hpp"`, `#include <unordered_set>` at the top if absent) the three declarations above, with this comment block:

```cpp
    // The Outliner (replaces the flat Hierarchy panel). Pure row data comes
    // from BuildOutlinerRows (EntityList.hpp, headless-tested); this shell
    // draws it and routes EVERY structural edit through ApplyRegistryMutation
    // over binding.snapshot/restore (Runtime::SnapshotRegistry/RestoreRegistry).
    // binding.editMode == false (Play running) disables structural edits --
    // the slice-2 resolution of RegistryStateCommand.hpp's native-state note.
```

- [ ] **Step 2: The panel.** In `EditorPanels.cpp`, replace `DrawHierarchyPanel` (:358-371) with:

```cpp
    namespace
    {
        bool ApplyStructural(Arcane::CommandStack& undo, const OutlinerBinding& b,
                             std::string label, Arcane::FunctionRef<bool()> mutate)
        {
            if (!b.editMode)
                return false;
            return Arcane::ApplyRegistryMutation(undo, std::move(label),
                                                 b.snapshot, b.restore, mutate);
        }

        void BeginRename(OutlinerState& st, Astra::Entity e, const std::string& current)
        {
            st.renameTarget = e;
            std::snprintf(st.renameBuf, sizeof(st.renameBuf), "%s", current.c_str());
            st.renameFocusPending = true;
        }

        void DeleteSelection(Astra::Registry& registry, SelectionContext& sel,
                             Arcane::CommandStack& undo, const OutlinerBinding& binding)
        {
            const std::vector<Astra::Entity> doomed = sel.Entities();   // copy: sel mutates after
            if (doomed.empty())
                return;
            if (ApplyStructural(undo, binding, "Delete",
                    [&] { return Arcane::Edit::DeleteEntities(registry, doomed) > 0; }))
                sel.Clear();
        }
    }

    void DrawOutlinerPanel(Astra::Registry& registry, SelectionContext& sel,
                           Arcane::CommandStack& undo, const OutlinerBinding& binding,
                           OutlinerState& state)
    {
        ImGui::Begin("Outliner");

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##outliner_search", ICON_LC_SEARCH " Filter",
                                 state.search, sizeof(state.search));

        const std::vector<OutlinerRow> rows =
            BuildOutlinerRows(registry, state.search, state.sort, state.collapsed);

        const bool renaming = state.renameTarget.IsValid();
        const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        if (binding.editMode && windowFocused && !renaming)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && sel.Count() == 1)
                BeginRename(state, sel.Primary(),
                            Arcane::Edit::DisplayName(registry, sel.Primary()));
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && sel.HasSelection())
                DeleteSelection(registry, sel, undo, binding);
        }

        const float footerH = ImGui::GetFrameHeightWithSpacing();
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
        if (ImGui::BeginTable("##outliner_rows", 3, tflags, ImVec2(0.0f, -footerH)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(ICON_LC_EYE,
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("Post Process").x * 1.4f);
            ImGui::TableHeadersRow();

            // Header sort -> state.sort; rows were built with LAST frame's
            // sort (one-frame lag, rebuilt every frame anyway).
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
            {
                state.sort = OutlinerSort{};
                if (specs->SpecsCount > 0)
                {
                    const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
                    state.sort.column = (s.ColumnIndex == 2) ? OutlinerSort::Column::Type
                                                             : OutlinerSort::Column::Label;
                    state.sort.ascending = s.SortDirection != ImGuiSortDirection_Descending;
                }
            }

            for (const OutlinerRow& row : rows)
            {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(row.entity.GetValue()));

                // -- column 0: the eye --------------------------------------
                ImGui::TableSetColumnIndex(0);
                {
                    const char* icon = row.hidden ? ICON_LC_EYE_OFF : ICON_LC_EYE;
                    if (row.hidden)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (binding.editMode)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        if (ImGui::SmallButton(icon))
                        {
                            const Astra::Entity e = row.entity;
                            const bool hide = !row.hidden;
                            ApplyStructural(undo, binding, hide ? "Hide" : "Show",
                                [&] { return Arcane::Edit::SetHiddenRecursive(registry, e, hide) > 0; });
                        }
                        ImGui::PopStyleColor();
                    }
                    else
                        ImGui::TextUnformatted(icon);
                    if (row.hidden)
                        ImGui::PopStyleColor();
                }

                // -- column 1: tree arrow + label (or inline rename) --------
                ImGui::TableSetColumnIndex(1);
                if (state.renameTarget == row.entity)
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (state.renameFocusPending)
                    {
                        ImGui::SetKeyboardFocusHere();
                        state.renameFocusPending = false;
                    }
                    bool commit = ImGui::InputText("##rename", state.renameBuf,
                        sizeof(state.renameBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsItemDeactivated())
                    {
                        commit = commit || !ImGui::IsKeyPressed(ImGuiKey_Escape);
                        if (commit)
                        {
                            const Astra::Entity e = row.entity;
                            ApplyStructural(undo, binding, "Rename",
                                [&] { return Arcane::Edit::RenameEntity(registry, e, state.renameBuf); });
                        }
                        state.renameTarget = Astra::Entity::Invalid();
                    }
                }
                else
                {
                    const float indent = row.depth * ImGui::GetStyle().IndentSpacing;
                    if (indent > 0.0f)
                        ImGui::Indent(indent);

                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                                             | ImGuiTreeNodeFlags_OpenOnArrow
                                             | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    if (!row.hasChildren)
                        flags |= ImGuiTreeNodeFlags_Leaf;
                    if (sel.Contains(row.entity))
                        flags |= ImGuiTreeNodeFlags_Selected;

                    const std::uint64_t value = static_cast<std::uint64_t>(row.entity.GetValue());
                    const bool open = !state.collapsed.contains(value);
                    ImGui::SetNextItemOpen(open, ImGuiCond_Always);
                    if (row.dimmed)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    const bool nowOpen = ImGui::TreeNodeEx(row.label.c_str(), flags);
                    if (row.dimmed)
                        ImGui::PopStyleColor();
                    if (row.hasChildren && nowOpen != open)
                    {
                        if (nowOpen) state.collapsed.erase(value);
                        else         state.collapsed.insert(value);
                    }

                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)
                        && !ImGui::IsItemToggledOpen())
                    {
                        const double now = ImGui::GetTime();
                        const bool ctrl = ImGui::GetIO().KeyCtrl;
                        const bool shift = ImGui::GetIO().KeyShift;
                        if (ctrl)
                            sel.Toggle(row.entity);
                        else if (shift && sel.HasSelection())
                            sel.AddRange(RowRange(rows, sel.Primary(), row.entity), row.entity);
                        else
                        {
                            // Slow second click on the sole-selected row = rename.
                            const bool slowSecond = binding.editMode
                                && sel.Count() == 1 && sel.Primary() == row.entity
                                && state.lastClicked == row.entity
                                && (now - state.lastClickTime) > ImGui::GetIO().MouseDoubleClickTime
                                && (now - state.lastClickTime) < 1.2;
                            if (slowSecond)
                                BeginRename(state, row.entity, row.label);
                            else
                                sel.Select(row.entity);
                        }
                        state.lastClicked = row.entity;
                        state.lastClickTime = now;
                    }

                    if (indent > 0.0f)
                        ImGui::Unindent(indent);
                }

                // -- column 2: type -----------------------------------------
                ImGui::TableSetColumnIndex(2);
                if (row.dimmed)
                    ImGui::TextDisabled("%s", row.type.c_str());
                else
                    ImGui::TextUnformatted(row.type.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        std::size_t total = 0;
        for (Astra::Entity e : registry.GetEntityManager())
        {
            (void)e;
            ++total;
        }
        ImGui::Text("%zu entities (%zu selected)", total, sel.Count());

        ImGui::End();
    }
```

Also update the dock-layout string at `EditorPanels.cpp:107`: `ImGui::DockBuilderDockWindow("Hierarchy", leftId);` -> `ImGui::DockBuilderDockWindow("Outliner", leftId);`. Add the needed includes at the top of EditorPanels.cpp: `<Arcane/Edit/EntityOps.hpp>` (RegistryStateCommand.hpp arrives via the header).

- [ ] **Step 3: EditorApp wiring.** `EditorApp.hpp`: below the `m_selection` member add:

```cpp
        Arcane::Editor::OutlinerState   m_outliner;
        Arcane::Editor::OutlinerBinding m_outlinerBinding;
```

`EditorApp.cpp`, right after the `m_undo.emplace(...)` at :300:

```cpp
    // Structural-edit binding: whole-registry snapshot/restore through the
    // SAME Runtime the resolver reads, so the memento survives registry swaps.
    m_outlinerBinding.snapshot = [rt = &*m_runtime]() -> std::vector<std::byte>
    {
        auto r = rt->SnapshotRegistry();
        return r.IsOk() ? std::move(*r) : std::vector<std::byte>{};
    };
    m_outlinerBinding.restore = [rt = &*m_runtime](std::span<const std::byte> bytes)
    {
        return rt->RestoreRegistry(bytes);
    };
```

Call-site swap at :1298 (and Prune + editMode immediately before it):

```cpp
            m_selection.Prune([reg = &m_runtime->Registry()](Astra::Entity e)
                              { return reg->IsValid(e); });
            m_outlinerBinding.editMode = !m_play.IsPlaying();
            Arcane::Editor::DrawOutlinerPanel(m_runtime->Registry(), m_selection,
                                              *m_undo, m_outlinerBinding, m_outliner);
```

Project-switch teardown near :619 (next to `m_selection.Clear();`): add `m_outliner = {};`.

- [ ] **Step 4: Delete `CollectEntities`** from EntityList.hpp/.cpp and delete its TEST_CASE(s) from `EditorEntityListTest.cpp` (grep first: the Outliner replaced its only production consumer; `EditorPlayModeTest.cpp` iterates `GetEntityManager()` directly, not through it — verify with a workspace grep for `CollectEntities` before deleting, and if any other consumer surfaced since this plan was written, STOP and report instead of deleting).

- [ ] **Step 5: Build + full gate.** Build the solution; from the exe dir run `.\ArcaneTests.exe "~[gpu]"` — all green (structural-edit paths are shell-only; the gate proves nothing regressed).

- [ ] **Step 6: Commit.** `feat(arcane-editor): Outliner panel — tree table, search, rename, eye, multi-select (outliner arc, slice 2c)` — body notes: window renamed Hierarchy -> Outliner (existing imgui.ini users: the panel appears undocked until re-docked or layout reset — desk-verify item), CollectEntities deleted with its tests.

---

### Task 5: Outliner panel shell part B — context menus, drag-reparent, root drop

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (extend `DrawOutlinerPanel` only)

**Interfaces:**
- Consumes: Task 4's panel + helpers, `Edit::CreateEntity/Reparent`.
- Produces: none new (interaction-complete panel; §5 Add Component menu item intentionally DEFERRED to slice 4 where the searchable popup exists).

- [ ] **Step 1: Row context menu.** Inside the row loop's `else` (tree-node) branch, after the click handling and before `Unindent`:

```cpp
                    // Right-click selects (if outside the selection) then menus.
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)
                        && !sel.Contains(row.entity))
                        sel.Select(row.entity);
                    if (ImGui::BeginPopupContextItem("##row_ctx"))
                    {
                        if (!binding.editMode)
                            ImGui::BeginDisabled();
                        if (ImGui::MenuItem("New Child Entity"))
                        {
                            Astra::Entity created = Astra::Entity::Invalid();
                            const Astra::Entity parent = row.entity;
                            if (ApplyStructural(undo, binding, "Create Entity",
                                    [&] { created = Arcane::Edit::CreateEntity(registry, parent);
                                          return created.IsValid(); }))
                            {
                                state.collapsed.erase(
                                    static_cast<std::uint64_t>(parent.GetValue()));
                                sel.Select(created);
                            }
                        }
                        if (ImGui::MenuItem("Rename", "F2"))
                            BeginRename(state, row.entity, row.label);
                        if (ImGui::MenuItem("Delete", "Del"))
                        {
                            if (!sel.Contains(row.entity))
                                sel.Select(row.entity);
                            DeleteSelection(registry, sel, undo, binding);
                        }
                        if (!binding.editMode)
                            ImGui::EndDisabled();
                        ImGui::EndPopup();
                    }
```

- [ ] **Step 2: Drag source + row drop target.** Same spot (after the context menu block):

```cpp
                    if (binding.editMode && ImGui::BeginDragDropSource())
                    {
                        ImGui::SetDragDropPayload("ARC_OUTLINER_ENTITY",
                                                  &row.entity, sizeof(Astra::Entity));
                        ImGui::TextUnformatted(row.label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (binding.editMode && ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* p =
                                ImGui::AcceptDragDropPayload("ARC_OUTLINER_ENTITY"))
                        {
                            Astra::Entity dragged;
                            std::memcpy(&dragged, p->Data, sizeof(dragged));
                            const std::vector<Astra::Entity> moving =
                                sel.Contains(dragged) ? sel.Entities()
                                                      : std::vector<Astra::Entity>{ dragged };
                            const Astra::Entity target = row.entity;
                            ApplyStructural(undo, binding, "Reparent",
                                [&] { return Arcane::Edit::Reparent(registry, moving, target) > 0; });
                        }
                        ImGui::EndDragDropTarget();
                    }
```

(`#include <cstring>` if not already present. Cycle attempts are refused wholesale by `Edit::Reparent` — dropping a parent onto its own child is a silent no-op with no undo entry, exactly the slice-1 contract.)

- [ ] **Step 3: Empty-space context menu + root drop strip.** After `ImGui::EndTable()`, before the footer:

```cpp
        // Drop below the table = unparent to root. Only visible mid-drag.
        if (binding.editMode && ImGui::GetDragDropPayload() != nullptr)
        {
            ImGui::Selectable("(drop here to unparent)", false,
                              ImGuiSelectableFlags_Disabled);
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("ARC_OUTLINER_ENTITY"))
                {
                    Astra::Entity dragged;
                    std::memcpy(&dragged, p->Data, sizeof(dragged));
                    const std::vector<Astra::Entity> moving =
                        sel.Contains(dragged) ? sel.Entities()
                                              : std::vector<Astra::Entity>{ dragged };
                    ApplyStructural(undo, binding, "Unparent",
                        [&] { return Arcane::Edit::Reparent(registry, moving,
                                                            Astra::Entity::Invalid()) > 0; });
                }
                ImGui::EndDragDropTarget();
            }
        }

        if (binding.editMode && ImGui::BeginPopupContextWindow("##outliner_ctx",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::MenuItem("New Entity"))
            {
                Astra::Entity created = Astra::Entity::Invalid();
                if (ApplyStructural(undo, binding, "Create Entity",
                        [&] { created = Arcane::Edit::CreateEntity(registry,
                                            Astra::Entity::Invalid());
                              return created.IsValid(); }))
                    sel.Select(created);
            }
            ImGui::EndPopup();
        }
```

Note the `ImGuiSelectableFlags_Disabled` drop strip still accepts drops because the drag-drop target is registered on the item regardless of its selectable behavior; if that proves false at desk-verify, swap `Selectable` for `ImGui::Button` with full width — either satisfies "onto empty = unparent".

- [ ] **Step 4: Build + full gate.** Solution build; `.\ArcaneTests.exe "~[gpu]"` from the exe dir — all green.

- [ ] **Step 5: Commit.** `feat(arcane-editor): Outliner context menus + drag-reparent + root drop (outliner arc, slice 2d)`

---

## Out of scope (verified against the spec)

- Viewport Ctrl+click toggle, gizmo group transforms, Inspector fan-out, multi-outline CB array — slice 3 (§4).
- Add/Remove Component UI + the outliner "Add Component..." context item — slice 4 (§5; the menu item is deferred WITH its popup).
- Mixed-value dashes, folders, >64 outlines — non-goals (spec).
- Interactive desk-verify of the panel (docking, drag feel, rename focus, eye latency) — the USER's step after merge-readiness; every task above lands headless-green first.

## Self-review notes

- Spec coverage: §3 row builder (T3), table/eye/rename/search/footer (T4), context menus + drag + delete key (T4/T5), SelectionContext set+primary with mechanical Primary() switch (T2), everything routed through §2 commands (T4/T5 via ApplyStructural), headless row-builder tests (T3). Same-name rename fix + redo-latch + docs (T1). OPEN note resolved as Edit-mode-only (T1 doc + T4 editMode gate).
- Type consistency: `OutlinerSort::Column::{None,Label,Type}`, `OutlinerBinding.editMode`, `SelectionContext::{Primary,Entities,Contains,Select,Toggle,AddRange,Clear,Prune,Count,HasSelection}` — used identically in T2/T3/T4/T5.
- Known accepted risks (reviewers: these are choices, not oversights): one-frame sort lag (rows rebuilt every frame); footer counts ALL live entities while the filter is active (spec's "N entities" is the scene count, not the visible count); `ImGuiTableFlags_SortTristate` returns SpecsCount==0 for the unsorted state which maps to Column::None.
