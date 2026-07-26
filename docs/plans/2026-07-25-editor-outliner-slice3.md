# Editor Outliner Slice 3: full multi-edit

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the multi-selection slice 2 stored actually *do* something: the gizmo group-transforms it, the Inspector fans field edits across it, the outline highlights all of it, and viewport Ctrl+click toggles membership.

**Architecture:** The transform math lands as pure value functions in the existing editor-free `Arcane::Gizmo` core (headless `[gizmo]` tests); "which entities may be transformed" lands as `Edit::SelectionRoots` next to the other registry set-ops (headless `[outliner]` tests). `EditorApp` then wires them. **No CommandStack change is needed** — `SnapshotComponent` is already idempotent per `(entity, descriptor)` and `Commit()` packs every pending snapshot into ONE transaction, so a group drag is one undo step for free (verified: `CommandStack.cpp:23-65`, proven by `CommandStackTest.cpp:144-170`).

**Tech Stack:** C++23, glm (2D), Dear ImGui, Astra ECS + reflection, NVRHI/HLSL, Catch2. Spec: `docs/superpowers/specs/2026-07-25-editor-outliner-design.md` §4.

## Global Constraints

- /MD runtime everywhere in the Arcane workspace; UTF-8 without BOM; ASCII comments; no `/fp:fast`.
- Tests run FROM THE EXE DIR: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`. Running from anywhere else false-fails PluginHost/Msdfgen.
- The dev-loop gate is `ArcaneTests.exe "~[gpu]"`. **`[gpu]` suites are desk-only on this machine** — Task 4 changes GPU code whose real verification is `[gpu]`, so its gate is build + `~[gpu]` green + `[gpu]` DEFERRED to the user's desk/CI. Say so; do not claim GPU verification you did not run.
- Prefer `-d yes` on long gate runs. A gate whose log stops growing is NOT proof of a hang — without `-d yes` Catch2 writes nothing until the end. Confirm a suspected hang with process CPU over several seconds. ArcaneTests has hung at startup twice on this box; kill + re-run was clean both times.
- `EditorPanels.cpp` / `EditorApp.cpp` are NOT compiled into ArcaneTests (ImGui shells excluded, AssetBrowser precedent). A green gate proves NOTHING about them — prove they compiled by checking `bin-int\Debug-windows-x86_64-md\ArcaneEditor\<file>.obj` postdates the source.
- Shaders are data: editing `Arcane/shaders/*.hlsl` requires a rebuild so the DXC prebuild step regenerates DXIL+SPIR-V. C++/HLSL constant buffers are hand-mirrored — every change must carry a `static_assert(sizeof(...))` and a comment naming the HLSL cbuffer, matching the existing idiom at `SelectionOutline.cpp:27-49`.
- New files => re-run `Arcane\GenerateProjects.bat` before building. No task here adds a file.
- MSBuild: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo` from `Arcane\`.
- Commit messages with special characters: write to a scratch file, `git commit -F <file>` (PS 5.1 quoting).
- ABI rule: Task 1 appends two free functions to `Arcane/Edit/Gizmo.hpp` and one to `EntityOps.hpp`, and Task 4 changes `SelectionOutline::Params`. All are ARCANE_API surface but none is a plugin vtable / EngineContext / component layout, and no plugin consumes them (verified: Sandbox/Loom/PlaygroundGame have ZERO SelectionOutline or Gizmo references). Expect NO ABI bump; note the reasoning in the commit message.

---

### Task 1: Group-transform math + SelectionRoots (pure, headless)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Edit/Gizmo.hpp` (append after `ApplyDrag`, ~:63)
- Modify: `Arcane/Arcane/src/Arcane/Edit/Gizmo.cpp` (append)
- Modify: `Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp` (append after `RemoveComponent`, ~:67)
- Modify: `Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp` (append)
- Test: `Arcane/Tests/src/GizmoTest.cpp` (extend, tag `[gizmo]`), `Arcane/Tests/src/EntityOpsTest.cpp` (extend, tag `[outliner]`)

**Interfaces:**
- Consumes: existing `GizmoTransform`, `Astra::Registry::GetParent/IsValid`.
- Produces (Task 2 depends on exactly these):
  `struct GizmoGroupDelta { glm::vec2 translate; float rotate; glm::vec2 scale; glm::vec2 pivot; };`
  `GizmoGroupDelta MakeGroupDelta(const GizmoTransform& start, const GizmoTransform& end);`
  `GizmoTransform ApplyGroupDelta(const GizmoTransform& t, const GizmoGroupDelta& d);`
  `std::vector<Astra::Entity> Edit::SelectionRoots(Astra::Registry&, std::span<const Astra::Entity>);`

- [ ] **Step 1: Write the failing tests.** Append to `Arcane/Tests/src/GizmoTest.cpp`:

```cpp
TEST_CASE("Gizmo group delta: translate is shared, pivot-independent", "[gizmo]")
{
    // Translate mode: ApplyDrag moves only position, so the delta carries a
    // pure translation and every member shifts by the same world vector.
    const Arcane::GizmoTransform start{ glm::vec2(1.0f, 1.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.position = glm::vec2(4.0f, -1.0f);

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.translate.x, WithinAbs(3.0f, 1e-5f));
    CHECK_THAT(d.translate.y, WithinAbs(-2.0f, 1e-5f));

    const Arcane::GizmoTransform other{ glm::vec2(-5.0f, 10.0f), 0.5f, glm::vec2(2.0f, 3.0f) };
    const Arcane::GizmoTransform moved = Arcane::ApplyGroupDelta(other, d);
    CHECK_THAT(moved.position.x, WithinAbs(-2.0f, 1e-5f));
    CHECK_THAT(moved.position.y, WithinAbs(8.0f, 1e-5f));
    CHECK_THAT(moved.rotation, WithinAbs(0.5f, 1e-5f));   // untouched
    CHECK_THAT(moved.scale.x, WithinAbs(2.0f, 1e-5f));    // untouched
}

TEST_CASE("Gizmo group delta: rotate ORBITS others about the primary's pivot", "[gizmo]")
{
    // The whole point of a group rotate: a member one unit to the +X of the
    // pivot swings to +Y under a quarter turn, AND spins by the same angle.
    const float quarter = 1.5707963268f;
    const Arcane::GizmoTransform start{ glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.rotation = quarter;

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.rotate, WithinAbs(quarter, 1e-5f));
    CHECK_THAT(d.translate.x, WithinAbs(0.0f, 1e-5f));   // rotate does not translate

    const Arcane::GizmoTransform other{ glm::vec2(1.0f, 0.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(other, d);
    CHECK_THAT(r.position.x, WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(r.position.y, WithinAbs(1.0f, 1e-5f));    // orbited
    CHECK_THAT(r.rotation, WithinAbs(quarter, 1e-5f));   // and spun
}

TEST_CASE("Gizmo group delta: scale multiplies and moves others along the pivot ray", "[gizmo]")
{
    const Arcane::GizmoTransform start{ glm::vec2(2.0f, 2.0f), 0.0f, glm::vec2(1.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.scale = glm::vec2(2.0f, 2.0f);

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.scale.x, WithinAbs(2.0f, 1e-5f));

    const Arcane::GizmoTransform other{ glm::vec2(3.0f, 2.0f), 0.0f, glm::vec2(4.0f, 0.5f) };
    const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(other, d);
    CHECK_THAT(r.position.x, WithinAbs(4.0f, 1e-5f));    // pivot + 1*2
    CHECK_THAT(r.position.y, WithinAbs(2.0f, 1e-5f));    // on the pivot line: unmoved
    CHECK_THAT(r.scale.x, WithinAbs(8.0f, 1e-5f));       // 4 * 2
    CHECK_THAT(r.scale.y, WithinAbs(1.0f, 1e-5f));       // 0.5 * 2
}

TEST_CASE("Gizmo group delta: replaying onto the primary reproduces ApplyDrag", "[gizmo]")
{
    // EditorApp applies the delta UNIFORMLY across the selection, primary
    // included -- that is only sound if the primary round-trips exactly.
    const Arcane::GizmoTransform start{ glm::vec2(1.0f, -2.0f), 0.3f, glm::vec2(2.0f, 0.5f) };
    Arcane::GizmoTransform end{ glm::vec2(4.0f, 1.0f), 1.1f, glm::vec2(3.0f, 1.5f) };

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(start, d);
    CHECK_THAT(r.position.x, WithinAbs(end.position.x, 1e-5f));
    CHECK_THAT(r.position.y, WithinAbs(end.position.y, 1e-5f));
    CHECK_THAT(r.rotation, WithinAbs(end.rotation, 1e-5f));
    CHECK_THAT(r.scale.x, WithinAbs(end.scale.x, 1e-5f));
    CHECK_THAT(r.scale.y, WithinAbs(end.scale.y, 1e-5f));
}

TEST_CASE("Gizmo group delta: a degenerate start scale yields ratio 1, not infinity", "[gizmo]")
{
    const Arcane::GizmoTransform start{ glm::vec2(0.0f, 0.0f), 0.0f, glm::vec2(0.0f, 1.0f) };
    Arcane::GizmoTransform end = start;
    end.scale = glm::vec2(5.0f, 2.0f);

    const Arcane::GizmoGroupDelta d = Arcane::MakeGroupDelta(start, end);
    CHECK_THAT(d.scale.x, WithinAbs(1.0f, 1e-5f));   // guarded
    CHECK_THAT(d.scale.y, WithinAbs(2.0f, 1e-5f));
}
```

Append to `Arcane/Tests/src/EntityOpsTest.cpp`:

```cpp
TEST_CASE("SelectionRoots drops entities covered by a selected ancestor", "[outliner]")
{
    // Transform edits must apply to roots ONLY: a selected child already
    // rides its selected parent through WorldTransform propagation, so
    // transforming both would double-move the child.
    World w;
    Astra::Entity a = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity b = Edit::CreateEntity(w.reg, a);          // child of a
    Astra::Entity c = Edit::CreateEntity(w.reg, b);          // grandchild of a
    Astra::Entity lone = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());

    SECTION("ancestor in the set covers descendants at any depth")
    {
        const std::array<Astra::Entity, 4> set{ a, b, c, lone };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, set);
        REQUIRE(roots.size() == 2);
        CHECK(roots[0] == a);        // input order preserved
        CHECK(roots[1] == lone);
    }

    SECTION("a child selected WITHOUT its parent is its own root")
    {
        const std::array<Astra::Entity, 2> set{ c, lone };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, set);
        REQUIRE(roots.size() == 2);
        CHECK(roots[0] == c);
    }

    SECTION("dead entities are skipped and duplicates collapse")
    {
        Astra::Entity dead = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
        w.reg.DestroyEntity(dead);
        const std::array<Astra::Entity, 4> set{ lone, dead, lone, a };
        const std::vector<Astra::Entity> roots = Edit::SelectionRoots(w.reg, set);
        REQUIRE(roots.size() == 2);      // lone once, a once
        CHECK(roots[0] == lone);
        CHECK(roots[1] == a);
    }
}
```

- [ ] **Step 2: Build, run, verify the new tests FAIL to compile** (`MakeGroupDelta` / `SelectionRoots` undeclared). That is the correct red state for new API.

- [ ] **Step 3: Implement.** Append to `Gizmo.hpp` (inside `namespace Arcane`, after `ApplyDrag`):

```cpp
    // A drag's effect on the PRIMARY, expressed so it can be replayed onto the
    // rest of a multi-selection. `translate` is a shared world delta;
    // `rotate`/`scale` act about `pivot`, so a group rotate ORBITS the other
    // members rather than spinning each in place.
    struct GizmoGroupDelta
    {
        glm::vec2 translate{0.0f, 0.0f};
        float     rotate = 0.0f;        // radians
        glm::vec2 scale{1.0f, 1.0f};    // ratio, component-wise
        glm::vec2 pivot{0.0f, 0.0f};    // the primary's PRE-drag position
    };

    // Delta from the primary's pre-drag pose to its post-drag pose. A start
    // scale component under 1e-6 yields a ratio of 1 on that axis instead of
    // infinity.
    ARCANE_API GizmoGroupDelta MakeGroupDelta(const GizmoTransform& start,
                                              const GizmoTransform& end);

    // Replay a group delta onto a member's PRE-drag pose. Replaying onto the
    // primary's own start reproduces ApplyDrag's result, so callers may apply
    // this uniformly across the whole selection without special-casing.
    ARCANE_API GizmoTransform ApplyGroupDelta(const GizmoTransform& t,
                                              const GizmoGroupDelta& d);
```

Append to `Gizmo.cpp` (inside `namespace Arcane`; `<cmath>` is already included):

```cpp
    GizmoGroupDelta MakeGroupDelta(const GizmoTransform& start, const GizmoTransform& end)
    {
        GizmoGroupDelta d;
        d.translate = end.position - start.position;
        d.rotate    = end.rotation - start.rotation;
        d.pivot     = start.position;
        d.scale.x   = std::abs(start.scale.x) > 1e-6f ? end.scale.x / start.scale.x : 1.0f;
        d.scale.y   = std::abs(start.scale.y) > 1e-6f ? end.scale.y / start.scale.y : 1.0f;
        return d;
    }

    GizmoTransform ApplyGroupDelta(const GizmoTransform& t, const GizmoGroupDelta& d)
    {
        // Scale then rotate the member's offset from the pivot, then shift.
        // (Equivalent to the mat3 T*R*S about the pivot, written directly:
        // the 2x2 has no shear, so composing matrices would only obscure it.)
        glm::vec2 rel = t.position - d.pivot;
        rel *= d.scale;
        const float c = std::cos(d.rotate);
        const float s = std::sin(d.rotate);
        rel = glm::vec2(rel.x * c - rel.y * s, rel.x * s + rel.y * c);

        GizmoTransform r;
        r.position = d.pivot + rel + d.translate;
        r.rotation = t.rotation + d.rotate;
        r.scale    = t.scale * d.scale;
        return r;
    }
```

Append to `EntityOps.hpp` (inside `namespace Arcane::Edit`; needs `<vector>`, already included):

```cpp
    // The entities in `set` that have NO ancestor also in `set`. Transform
    // edits must apply to these only: moving a parent already carries its
    // children through WorldTransform propagation, so applying to both
    // double-moves the children. Dead entities are skipped and duplicates
    // collapse; surviving order follows `set`.
    ARCANE_API std::vector<Astra::Entity> SelectionRoots(Astra::Registry& reg,
                                                         std::span<const Astra::Entity> set);
```

Append to `EntityOps.cpp` (inside `namespace Arcane::Edit`; `<unordered_set>` and `<vector>` are already included):

```cpp
    std::vector<Astra::Entity> SelectionRoots(Astra::Registry& reg,
                                              std::span<const Astra::Entity> set)
    {
        const std::unordered_set<Astra::Entity> members(set.begin(), set.end());
        std::unordered_set<Astra::Entity> emitted;
        std::vector<Astra::Entity> roots;
        for (Astra::Entity e : set)
        {
            if (!reg.IsValid(e) || emitted.contains(e))
                continue;
            bool covered = false;
            for (Astra::Entity a = reg.GetParent(e); a.IsValid(); a = reg.GetParent(a))
            {
                if (members.contains(a))
                {
                    covered = true;
                    break;
                }
            }
            if (covered)
                continue;
            emitted.insert(e);
            roots.push_back(e);
        }
        return roots;
    }
```

- [ ] **Step 4: Build + run.** From the exe dir: `.\ArcaneTests.exe "[gizmo]"` then `.\ArcaneTests.exe "[outliner]"` — all green.

- [ ] **Step 5: Commit.** `feat(arcane): gizmo group-transform delta + Edit::SelectionRoots (multi-edit, slice 3a)` — body notes the NO-ABI-bump reasoning (two appended free functions, no plugin consumer).

---

### Task 2: EditorApp — group gizmo drag + viewport Ctrl+click toggle

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (the `GizmoDrag` struct, ~:115-131)
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (gizmo block ~:870-976; pick block ~:1298-1309; add `#include <Arcane/Edit/EntityOps.hpp>`)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (`ViewportPanelResult`, ~:66-80)
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (viewport click block, ~:346-356)

**Interfaces:**
- Consumes: Task 1's `MakeGroupDelta`/`ApplyGroupDelta`/`SelectionRoots`; existing `SelectionContext::{Entities,Primary,Toggle,Select,Clear}`; existing `CommandStack::{Begin,SnapshotComponent,Commit,Cancel}` (unchanged — it already groups N snapshots into one transaction).
- Produces: none consumed by later tasks.

- [ ] **Step 1: Widen the drag state.** In `EditorApp.hpp`, replace the `GizmoDrag` struct body with:

```cpp
    struct GizmoDrag
    {
        bool                   active = false;
        Arcane::GizmoAxis      axis   = Arcane::GizmoAxis::None;
        Arcane::GizmoTransform start;                    // the PRIMARY's pre-drag pose (gizmo anchor)
        glm::vec2              mouseStartScreen{0.0f, 0.0f};
        // Every selection ROOT carrying a Transform, with its pre-drag pose.
        // Rebuilt on press. Roots only: a selected child already rides its
        // selected parent through WorldTransform propagation.
        std::vector<std::pair<Astra::Entity, Arcane::GizmoTransform>> targets;
    } m_gizmoDrag;
```

(`<vector>` and `<utility>` come in via existing includes; add them to `EditorApp.hpp` if the build says otherwise.)

- [ ] **Step 2: Snapshot every root on press.** In `EditorApp.cpp`, replace the press branch's `if (desc) { ... }` body (currently `m_undo->Begin("Gizmo"); m_undo->SnapshotComponent(sel, desc); m_gizmoDrag.active = true; ...`) with:

```cpp
                                    if (desc)
                                    {
                                        // Roots only -- see GizmoDrag::targets. One
                                        // Begin + N SnapshotComponent = ONE undo step:
                                        // CommandStack dedupes per (entity, descriptor)
                                        // and Commit packs them all into one transaction.
                                        const std::vector<Astra::Entity> roots =
                                            Arcane::Edit::SelectionRoots(*regPtr, m_selection.Entities());
                                        m_gizmoDrag.targets.clear();
                                        m_gizmoDrag.targets.reserve(roots.size());
                                        m_undo->Begin("Gizmo");
                                        for (Astra::Entity e : roots)
                                        {
                                            Arcane::Transform* et = regPtr->GetComponent<Arcane::Transform>(e);
                                            if (!et)
                                                continue;   // non-spatial node in the selection
                                            const Astra::ComponentDescriptor* ed =
                                                FindTransformDescriptor(*regPtr, e);
                                            if (!ed)
                                                continue;
                                            m_undo->SnapshotComponent(e, ed);
                                            m_gizmoDrag.targets.push_back(
                                                { e, Arcane::GizmoTransform{ et->position, et->rotation, et->scale } });
                                        }
                                        m_gizmoDrag.active           = true;
                                        m_gizmoDrag.axis             = m_gizmoHovered;
                                        m_gizmoDrag.start            = gt;
                                        m_gizmoDrag.mouseStartScreen = mouseScreen;
                                    }
```

- [ ] **Step 3: Apply the delta to every target per frame.** Replace the three lines that write the primary (`lt->position = nt.position; lt->rotation = nt.rotation; lt->scale = nt.scale;`) with:

```cpp
                            // One delta from the primary's drag, replayed onto every
                            // target's PRE-drag pose -- recomputed from `start` each
                            // frame, so nothing accumulates drift. The primary is in
                            // `targets` when it is itself a root and round-trips
                            // exactly (see ApplyGroupDelta).
                            const Arcane::GizmoGroupDelta gd =
                                Arcane::MakeGroupDelta(m_gizmoDrag.start, nt);
                            for (const auto& [e, startPose] : m_gizmoDrag.targets)
                            {
                                Arcane::Transform* et = regPtr->GetComponent<Arcane::Transform>(e);
                                if (!et)
                                    continue;   // destroyed mid-drag
                                const Arcane::GizmoTransform r = Arcane::ApplyGroupDelta(startPose, gd);
                                et->position = r.position;
                                et->rotation = r.rotation;
                                et->scale    = r.scale;
                            }
```

- [ ] **Step 4: Clear targets on both exits.** In the release branch, `m_gizmoDrag = {};` already clears `targets` (it is a fresh value). Confirm the cancel branch (`m_undo->Cancel(); m_gizmoDrag = {};`) does the same — no edit needed if both assign `{}`; if either clears fields individually, make it assign `{}`.

- [ ] **Step 5: Ctrl+click toggle.** In `EditorPanels.hpp`, add to `ViewportPanelResult` next to `altHeld`:

```cpp
        bool ctrlHeld = false;   // ctrl modifier at click time (multi-select toggle)
```

In `EditorPanels.cpp`, in the click block beside `r.altHeld = ImGui::GetIO().KeyAlt;`:

```cpp
                r.ctrlHeld = ImGui::GetIO().KeyCtrl;
```

In `EditorApp.cpp`, replace the pick block's select/clear with:

```cpp
                if (picked.IsValid())
                {
                    if (vp.ctrlHeld)
                        m_selection.Toggle(picked);
                    else
                        m_selection.Select(picked);
                }
                else if (!vp.ctrlHeld)
                {
                    // Ctrl+click on empty space is a miss, not a deselect-all --
                    // otherwise one stray click discards a built-up selection.
                    m_selection.Clear();
                }
```

- [ ] **Step 6: Build + gate.** Solution build 0 errors; from the exe dir `.\ArcaneTests.exe "~[gpu]" -d yes` — all green (this task adds no tests; the gate is a no-regression proof). Report `EditorApp.obj` and `EditorPanels.obj` timestamps postdating their sources, since neither compiles into ArcaneTests.

- [ ] **Step 7: Commit.** `feat(arcane-editor): gizmo group-transforms the selection + Ctrl+click toggle (multi-edit, slice 3b)`

---

### Task 3: Inspector fan-out across the selection

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`ImGuiFieldVisitor` ~:731-902 and `DrawInspectorPanel` ~:905-947)

**Interfaces:**
- Consumes: `Astra::Registry::GetComponentByHash(Astra::Entity, uint64_t)` -> `void*` (null when the entity lacks the component), `Astra::ComponentDescriptor::hash`, `Edit::DisplayName`, `SelectionContext::{Primary,Entities,Count}`.
- Produces: none.

- [ ] **Step 1: Give the visitor the selection.** Add these members to `ImGuiFieldVisitor` (after `project`):

```cpp
        // Fan-out targets. `selection` includes the primary; entities lacking
        // this component are skipped (the panel only shows components the whole
        // selection shares, but selection and panel are a frame apart).
        Astra::Registry*                  registry = nullptr;
        const std::vector<Astra::Entity>* selection = nullptr;
```

Add this helper next to `BeginGestureIfActivated`:

```cpp
        // Run `fn(instanceOfThatEntity)` for every selected entity carrying
        // this component. Falls back to the primary's own instance when the
        // fan-out context is absent, so a field is never silently un-editable.
        template<typename Fn>
        void ForEachTarget(void* primaryInstance, Fn&& fn)
        {
            if (!registry || !selection || selection->empty())
            {
                fn(entity, primaryInstance);
                return;
            }
            for (Astra::Entity e : *selection)
                if (void* data = registry->GetComponentByHash(e, descriptor->hash))
                    fn(e, data);
        }
```

- [ ] **Step 2: Snapshot every target when the gesture opens.** Replace `BeginGestureIfActivated`'s body:

```cpp
        void BeginGestureIfActivated(const std::string& field, void* primaryInstance)
        {
            if (stack && ImGui::IsItemActivated())
            {
                stack->Begin("Edit " + typeName + "." + field);
                // One Begin + N snapshots + one Commit = one undo step for the
                // whole fan-out (CommandStack dedupes per (entity, descriptor)).
                ForEachTarget(primaryInstance,
                              [&](Astra::Entity e, void*) { stack->SnapshotComponent(e, descriptor); });
            }
        }
```

Update every call site in `Visit` from `BeginGestureIfActivated(label);` to `BeginGestureIfActivated(label, instance);`.

- [ ] **Step 3: Fan each apply.** In `Visit`, wrap every write so it runs per target. The writes are currently one line per case; each becomes:

```cpp
            case Arcane::Editor::FieldKind::Bool:
            {
                bool v = f.Get<bool>(instance);
                bool changed = ImGui::Checkbox(label.c_str(), &v);
                BeginGestureIfActivated(label, instance);
                if (changed)
                    ForEachTarget(instance, [&](Astra::Entity, void* d)
                                  { Arcane::Editor::ApplyBoolEdit(f, d, v); });
                break;
            }
```

Apply the identical shape to the other kinds, keeping each case's existing widget and value read (which stay on the PRIMARY's `instance` — the widget shows the primary's value by design; mixed-value dashes are an explicit non-goal):
- `Int32` -> `Arcane::Editor::ApplyIntEdit(f, d, v)`
- `Float` -> `Arcane::Editor::ApplyFloatEdit(f, d, v)`
- `Vec2`  -> `if (glm::vec2* p = f.GetPtr<glm::vec2>(d)) *p = v;`
- `Vec3`  -> `if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) *p = v;`
- `AssetRef` -> inside `ApplyGuidImmediate` (next step)
- `ReadOnly` -> unchanged, no write

- [ ] **Step 4: Fan the single-shot Guid path.** Replace `ApplyGuidImmediate`'s body:

```cpp
        void ApplyGuidImmediate(const std::string& field, const Astra::FieldInfo& f,
                                void* instance, const Arcane::Guid& v)
        {
            if (stack)
            {
                stack->Begin("Edit " + typeName + "." + field);
                ForEachTarget(instance,
                              [&](Astra::Entity e, void*) { stack->SnapshotComponent(e, descriptor); });
            }
            ForEachTarget(instance, [&](Astra::Entity, void* d)
                          { Arcane::Editor::ApplyGuidEdit(f, d, v); });
            if (stack) stack->Commit();
        }
```

- [ ] **Step 5: Intersection + multi header.** Replace `DrawInspectorPanel`'s body between the `HasSelection` guard and `ImGui::End()`:

```cpp
        const Astra::Entity primary = sel.Primary();
        const std::string primaryName = Arcane::Edit::DisplayName(registry, primary);
        if (sel.Count() > 1)
            ImGui::Text("%s (+%zu)", primaryName.c_str(), sel.Count() - 1);
        else
            ImGui::TextUnformatted(primaryName.c_str());
        ImGui::Separator();

        for (const Astra::Registry::ComponentInfo& ci : registry.InspectEntity(primary))
        {
            if (!ci.descriptor || !ci.descriptor->visitFields || !ci.data)
                continue;
            const std::string typeName = ci.meta ? std::string(ci.meta->typeName)
                                                 : std::string("<unreflected>");
            if (typeName == "Arcane::WorldTransform" || typeName == "Arcane::PreviousTransform")
                continue;

            // Component-type INTERSECTION: editing a component only some of the
            // selection carries would silently edit a subset, so hide it entirely.
            bool sharedByAll = true;
            for (Astra::Entity e : sel.Entities())
            {
                if (e != primary && !registry.GetComponentByHash(e, ci.descriptor->hash))
                {
                    sharedByAll = false;
                    break;
                }
            }
            if (!sharedByAll)
                continue;

            if (ImGui::CollapsingHeader(typeName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGuiFieldVisitor visitor;
                visitor.stack      = editMode ? &undo : nullptr;
                visitor.entity     = primary;
                visitor.descriptor = ci.descriptor;
                visitor.typeName   = typeName;
                visitor.project    = project;
                visitor.registry   = &registry;
                visitor.selection  = &sel.Entities();
                ci.descriptor->visitFields(ci.data, visitor);
            }
        }
```

Add `#include <Arcane/Edit/EntityOps.hpp>` to `EditorPanels.cpp` if `DisplayName` is not already reachable (it is used by the Outliner in the same file, so it should be).

- [ ] **Step 6: Build + gate.** Solution build 0 errors; `.\ArcaneTests.exe "~[gpu]" -d yes` from the exe dir — all green. Report `EditorPanels.obj` postdating its source.

- [ ] **Step 7: Commit.** `feat(arcane-editor): Inspector fans field edits across the selection (multi-edit, slice 3c)`

---

### Task 4: Selection outline highlights the whole selection (up to 64)

**GPU-VERIFY CAVEAT — read before starting:** the real proof of this task is the `[gpu][selection]` suite, which is desk-only on this machine. Your gate is: solution build 0 errors (which includes the DXC shader compile — a malformed cbuffer FAILS the build, so the layout is machine-checked), `static_assert`s holding, and `~[gpu]` green. Report the `[gpu]` verification as DEFERRED. Do not claim you verified the rendering.

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Render/SelectionOutline.hpp` (`Params`, ~:32-44)
- Modify: `Arcane/Arcane/src/Arcane/Render/SelectionOutline.cpp` (`SeedCB` ~:27-31, write site ~:158-166)
- Modify: `Arcane/shaders/outline_seed.hlsl`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (outline call site ~:1152-1157)
- Modify: `Arcane/Tests/src/SelectionOutlineTest.cpp` (extend the `[gpu][selection]` cases to two ids)

**Interfaces:**
- Consumes: `PickBuffer::PassIdOf(Astra::Entity)`, `SelectionContext::Entities()`.
- Produces: `Params::selectedIds` (a `std::span<const uint32_t>`) replacing `Params::selectedId`; `kMaxSelectedOutlineIds`.

- [ ] **Step 1: Widen Params.** In `SelectionOutline.hpp`, add `#include <span>` and `#include <cstddef>`, then replace the `selectedId` field:

```cpp
    // Max ids the seed pass can outline in one frame -- the CB array is fixed
    // size. Beyond this, the first kMaxSelectedOutlineIds are outlined and the
    // rest are dropped with a one-time warning.
    inline constexpr std::size_t kMaxSelectedOutlineIds = 64;
```
and inside `Params`:
```cpp
        // Pass ids (PickBuffer::PassIdOf) of every selected entity; empty = no
        // selection. The ids are treated as ONE silhouette: the outline traces
        // their UNION, so two touching selected entities show a single outline
        // with no seam between them. Hover stays single-id.
        std::span<const uint32_t> selectedIds;
```

- [ ] **Step 2: Widen the CB mirror.** In `SelectionOutline.cpp` replace the `SeedCB` struct + assert:

```cpp
    // BYTE-IDENTICAL to the HLSL `cbuffer SeedCB` in outline_seed.hlsl. HLSL
    // packing: selectedCount(0), int2 cursor(4..12), superSample(12),
    // int2 dim(16..24), uint2 pad(24..32), then the id array (32..288).
    // The ids are declared `uint4 gSelectedIds[16]` in HLSL, NOT `uint[64]`:
    // an array of SCALARS pads every element to its own 16-byte register
    // (1024 bytes), while uint4[16] packs 4 per register and mirrors a tight
    // uint32_t[64] here exactly.
    struct SeedCB
    {
        uint32_t selectedCount;
        int32_t  cursorX, cursorY;
        uint32_t superSample;
        int32_t  dimX, dimY;
        uint32_t pad0, pad1;
        uint32_t selectedIds[64];
    };
    static_assert(sizeof(SeedCB) == 288, "SeedCB must match outline_seed.hlsl SeedCB");
    static_assert(offsetof(SeedCB, selectedIds) == 32, "id array starts at offset 32");
```

- [ ] **Step 3: Fill it.** Replace the `sc.selectedId = p.selectedId;` write with:

```cpp
        const std::size_t idCount = std::min(p.selectedIds.size(), kMaxSelectedOutlineIds);
        if (p.selectedIds.size() > kMaxSelectedOutlineIds)
        {
            static bool s_warnedOverflow = false;
            if (!s_warnedOverflow)
            {
                s_warnedOverflow = true;
                ARC_WARN("SelectionOutline: {} selected ids exceeds the {} the seed CB holds -- "
                         "outlining the first {}", p.selectedIds.size(),
                         kMaxSelectedOutlineIds, kMaxSelectedOutlineIds);
            }
        }
        sc.selectedCount = static_cast<uint32_t>(idCount);
        for (std::size_t i = 0; i < idCount; ++i)
            sc.selectedIds[i] = p.selectedIds[i];
```

(`<algorithm>` for `std::min` and the logging header are already included; add them if the build says otherwise.)

- [ ] **Step 4: Shader membership.** In `Arcane/shaders/outline_seed.hlsl`, replace the cbuffer and add the helper:

```hlsl
cbuffer SeedCB : register(b0)
{
    uint  gSelectedCount;   // 0 = no selection
    int2  gCursorPx;        // 1x viewport px; x<0 => no hover
    uint  gSuperSample;     // id-buffer supersample factor (e.g. 2)
    int2  gDim;             // 1x (composite) dimensions
    uint2 _pad;
    // 64 ids packed 4 per register. A `uint gSelectedIds[64]` would pad each
    // element to its own 16-byte register (1024B); uint4[16] mirrors the C++
    // uint32_t[64] exactly. Index as gSelectedIds[i >> 2][i & 3].
    uint4 gSelectedIds[16];
};

Texture2D<uint> gIds : register(t0);   // supersampled id buffer (ss*gDim)

// Is `id` part of the selection? The selection is ONE silhouette (union), so
// adjacent selected entities produce a single outline with no seam. Cost is
// gSelectedCount (typically 1-3), not the 64 capacity, and background (id 0)
// early-outs before the loop.
bool IsSelected(uint id)
{
    if (id == 0u) return false;
    [loop] for (uint i = 0u; i < gSelectedCount; ++i)
        if (gSelectedIds[i >> 2u][i & 3u] == id) return true;
    return false;
}
```

Then the three comparison sites:

```hlsl
        if (IsSelected(hoveredId)) hoveredId = 0u;   // hovering the selection => amber only
```
```hlsl
        if (IsSelected(id))                              { nSel++; sumSel += sub; }
        else if (hoveredId != 0u && id == hoveredId)     { nHov++; sumHov += sub; }
```
and the chosen-silhouette block plus its boundary test, which must become union-aware — a neighbour is "outside" when it is not in the selection, not merely when it differs from one id:

```hlsl
    float  tag, cov;
    float2 ctr;
    uint   chosenId;
    bool   chosenIsSelection;
    if (nSel > 0 && covSel >= covHov) { tag =  1.0; cov = covSel; ctr = sumSel / (float)nSel; chosenId = 0u;        chosenIsSelection = true;  }
    else if (nHov > 0)                { tag = -1.0; cov = covHov; ctr = sumHov / (float)nHov; chosenId = hoveredId; chosenIsSelection = false; }
    else return float4(0, 0, 0, 0);   // background: empty seed
```
```hlsl
        int2 q = clamp(base + int2(bx, by), int2(0, 0), idMax);
        uint qid = gIds.Load(int3(q, 0));
        bool inside = chosenIsSelection ? IsSelected(qid) : (qid == chosenId);
        if (!inside) boundary = true;
```

- [ ] **Step 5: Feed it from the editor.** In `EditorApp.cpp`, replace the `op.selectedId = ...` line:

```cpp
            // Every selected entity that made it into this frame's id pass.
            // SelectionOutline caps and warns; no clamping needed here.
            std::vector<uint32_t> selectedIds;
            selectedIds.reserve(m_selection.Count());
            for (Astra::Entity e : m_selection.Entities())
            {
                const uint32_t id = m_pick->PassIdOf(e);
                if (id != 0u)
                    selectedIds.push_back(id);
            }

            Arcane::SelectionOutline::Params op;
            op.selectedIds = selectedIds;
```
(keeping the existing `op.cursorPx` assignment; `selectedIds` must outlive the `Render` call, so declare it in the same scope as `op`.)

- [ ] **Step 6: Extend the GPU tests to two ids.** In `SelectionOutlineTest.cpp`, in each `[gpu][selection]` case, seed the synthetic id texture with TWO distinct rect ids and pass both, asserting both are outlined in select-amber (tag +1). Keep one single-id case so the common path stays pinned. These do not run on this machine — write them correctly and report them as deferred.

- [ ] **Step 7: Build + gate.** Solution build 0 errors (this compiles the shader — a bad cbuffer fails here); `.\ArcaneTests.exe "~[gpu]" -d yes` from the exe dir — all green. Report `[gpu]` as DEFERRED to desk/CI, and report the SelectionOutline `.obj` timestamp.

- [ ] **Step 8: Commit.** `feat(arcane): selection outline traces the whole selection, up to 64 ids (multi-edit, slice 3d)` — body notes the union-silhouette decision and the NO-ABI-bump reasoning (Params is not plugin surface; Sandbox/Loom/PlaygroundGame have zero references).

---

## Decisions this plan makes that the spec left open

- **Union silhouette.** The spec says "membership loop in-shader" without saying whether touching selected entities share one outline or each keep their own. This plan traces the UNION (no seam between adjacent selected entities), because a seam would read as a rendering bug. The hover path stays single-id. Reversible: keep `chosenId` per-pixel and compare against it instead.
- **Group math lives in the Gizmo core, not EditorApp.** The spec says "2D mat3"; the implementation composes the same transform directly in pivot-relative form (no shear is possible here, so matrices would only obscure it) and lives in `Arcane/Edit/Gizmo.hpp` so it is headless-testable rather than buried in the untested ImGui shell.
- **Ctrl+click on empty space is a miss, not a deselect-all** — otherwise one stray click discards a built-up selection.
- **Components not shared by the whole selection are hidden, not shown-and-partially-applied.** The spec says "component-type intersection"; hiding is the reading that never silently edits a subset.

## Out of scope (verified against the spec)

- Mixed-value dashes in multi-edit widgets; multi-rename with numbering — explicit spec non-goals.
- Outlining more than 64 selected entities — explicit spec non-goal (first 64 + one-time log).
- Add/Remove Component UI — slice 4 (spec §5).
- The seven can-ride items triaged in slice 2's final review (AddRange self-enforcing its invariant, drag-hover auto-expand, drop-accept dedup, eye-icon dead zone, Type-cell right-click, sort-comparator string rebuilds, collapsed/lastClicked sweep). If a task happens to touch that exact code, folding one in is welcome; do not go hunting.

## Self-review notes

- Spec §4 coverage: SelectionContext consumers fanned out (T2/T3), viewport Ctrl+click (T2), gizmo anchors at primary + applies to selection roots + one drag = one undo step (T1/T2), rotate/scale as true group transforms about the primary's pivot (T1), Inspector intersection + primary values + fan-out gesture (T3), outline 64-id CB array with hover staying single (T4).
- Type consistency: `GizmoGroupDelta{translate,rotate,scale,pivot}`, `MakeGroupDelta(start,end)`, `ApplyGroupDelta(t,d)`, `Edit::SelectionRoots(reg,set)`, `Params::selectedIds`, `kMaxSelectedOutlineIds` — used identically in T1-T4.
- Known accepted risks: the shader membership loop is O(gSelectedCount) per id comparison inside both the coverage loop (ss*ss) and the boundary loop ((ss+2)^2), so a 64-entity selection costs ~1280 comparisons per boundary pixel; typical selections are 1-3 and background early-outs, so this is accepted rather than optimized. Tasks 2 and 3 have NO automated coverage (ImGui shells) — their correctness rests on this plan's reviewer and the user's desk-verify.
