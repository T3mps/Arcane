# Inspector Multi-Select UE Parity — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Inspector honest about a multi-selection: stop fanning out
half-finished drag values to every selected entity, and stop showing the primary's
value as though the whole selection agreed.

**Architecture:** Two behaviours, both taken verbatim from the vendored UE 5.6 source
rather than from memory. The mixed-value computation is pure and lands in
`InspectorFields.{hpp,cpp}` (already source-compiled into ArcaneTests, so it is
headless-testable); the widget changes land in `ImGuiFieldVisitor` in `EditorPanels.cpp`
(no automated coverage by design — desk-verified).

**Tech Stack:** C++23, Astra reflection (`Astra::FieldInfo`), Dear ImGui, Catch2.

## Ground truth (read from `Arcane/.example/UnrealEngine-release/`, 2026-07-26)

All three citations are from
`Engine/Source/Editor/DetailCustomizations/Private/ComponentTransformDetails.cpp`.

1. **UE disables the spinner entirely on a multi-selection.**
   `.AllowSpin(SelectedObjects.Num() == 1)` — Location `:505`, Rotation `:551`,
   Scale `:628`. There is no "buffer the drag and apply on commit" in UE; on a
   multi-selection the widget is simply a type-and-commit numeric entry.

2. **UE additionally hard-ignores any non-committed change when N > 1** (`:1246`):
   ```cpp
   void FComponentTransformDetails::OnSetTransform(..., bool bMirror, bool bCommitted)
   {
       if (!bCommitted && SelectedObjects.Num() > 1)
       {
           // Ignore interactive changes when we have more than one selected object
           return;
       }
   ```
   Both belts. The `OnXChanged` delegates pass `bCommitted = false`, the
   `OnXCommitted` delegates pass `true` (`:497-502`).

3. **Mixed values are per-axis, sticky-unset** (`:1215-1225`). The first object seeds
   `CachedLocation/Rotation/Scale`; every later object clears any axis that differs,
   and the `&& Cached<X>.IsSet()` term means a cleared axis never comes back:
   ```cpp
   CachedLocation.X = Loc.X == CurLoc.X && CachedLocation.X.IsSet() ? Loc.X : TOptional<FVector::FReal>();
   ```
   An unset `TOptional` renders as `SNumericEntryBox`'s `UndeterminedString`, and
   `GetLocationResetVisibility` (`:1026`) states the convention outright:
   *"unset means multiple differing values"*.

**Our ImGui constraint (checked in `ThirdParty/imgui/imgui_internal.h:984`):**
`ImGuiItemFlags_MixedValue` is *"Currently only supported by Checkbox()"*. So the
tri-state dash is free for `Bool`, but numeric fields need their own blanking — done
here by drawing per-component widgets and passing an empty `format` string, which makes
ImGui print nothing for that component.

## Global Constraints

- Branch `shader-editor-slice1-material-core`. Do not merge.
- UTF-8 without BOM, **ASCII comments only**.
- Gate from the exe dir; verify under `--rng-seed 6` and `--rng-seed 17`.
- `EditorPanels.cpp` is NOT compiled into ArcaneTests — prove it built by checking
  `EditorPanels.obj` postdates the source.
- Baseline to beat: **29608 assertions / 514 cases**.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/ArcaneEditor/src/InspectorFields.hpp` (modify) | `FieldMixedMask`, `FieldComponentCount`, `ComputeFieldMixed` declarations |
| `Arcane/ArcaneEditor/src/InspectorFields.cpp` (modify) | Pure implementation of the UE sticky-unset compare |
| `Arcane/Tests/src/EditorInspectorTest.cpp` (modify) | Headless coverage of the mask |
| `Arcane/ArcaneEditor/src/EditorPanels.cpp` (modify) | `ImGuiFieldVisitor`: commit-only widgets when N>1, blank rendering when mixed |

---

### Task 1: Pure mixed-value computation

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorFields.hpp`
- Modify: `Arcane/ArcaneEditor/src/InspectorFields.cpp`
- Modify: `Arcane/Tests/src/EditorInspectorTest.cpp`

**Interfaces:**
- Consumes: `Astra::FieldInfo::GetPtr<T>`, `Astra::Registry::GetComponentByHash`, `ClassifyField`.
- Produces (Task 2 uses these exact signatures):
  - `struct Arcane::Editor::FieldMixedMask { std::uint32_t bits; bool Any() const; bool Test(int) const; }`
  - `int Arcane::Editor::FieldComponentCount(FieldKind) noexcept`
  - `FieldMixedMask Arcane::Editor::ComputeFieldMixed(Astra::Registry&, std::span<const Astra::Entity>, std::uint64_t componentHash, const Astra::FieldInfo&)`

- [ ] **Step 1: Declare the API**

Append to `InspectorFields.hpp` inside `namespace Arcane::Editor`, after the
`ApplyGuidEdit` declaration:

```cpp
    // Per-scalar-component "these differ across the selection" mask.
    //
    // Mirrors Unreal's FComponentTransformDetails::CacheDetails
    // (ComponentTransformDetails.cpp:1215-1225): the first entity carrying the
    // component seeds the values, every later entity marks each component that
    // differs, and a marked component STAYS marked (UE's `&& Cached.IsSet()`
    // term). UE's convention, stated at :1026, is that an unset value means
    // "multiple differing values" and renders blank.
    struct FieldMixedMask
    {
        std::uint32_t bits = 0;   // bit i set = scalar component i differs
        [[nodiscard]] bool Any() const noexcept { return bits != 0; }
        [[nodiscard]] bool Test(int i) const noexcept
        { return (bits >> i) & 1u; }
    };

    // Scalar components a kind occupies: Vec3 = 3, Vec2 = 2, everything else 1.
    [[nodiscard]] int FieldComponentCount(FieldKind kind) noexcept;

    // `componentHash` is the OWNING component's descriptor hash (used to fetch
    // each entity's instance). Entities that are dead or lack the component are
    // skipped, matching the fan-out in ImGuiFieldVisitor::ForEachTarget. A
    // selection of 0 or 1 live carriers is never mixed (empty mask).
    [[nodiscard]] FieldMixedMask ComputeFieldMixed(
        Astra::Registry& reg,
        std::span<const Astra::Entity> selection,
        std::uint64_t componentHash,
        const Astra::FieldInfo& f);
```

Add these includes at the top of `InspectorFields.hpp`, after the existing ones:

```cpp
#include <Astra/Entity/Entity.hpp>

#include <cstdint>
#include <span>
```

and the forward declaration `namespace Astra { class Registry; }` beside the existing
namespace decls.

- [ ] **Step 2: Implement it**

Append to `InspectorFields.cpp` inside `namespace Arcane::Editor`. Add
`#include <Astra/Registry/Registry.hpp>` and `#include <span>` to its include block
first.

```cpp
    int FieldComponentCount(FieldKind kind) noexcept
    {
        switch (kind)
        {
            case FieldKind::Vec3: return 3;
            case FieldKind::Vec2: return 2;
            default:              return 1;
        }
    }

    FieldMixedMask ComputeFieldMixed(Astra::Registry& reg,
                                     std::span<const Astra::Entity> selection,
                                     std::uint64_t componentHash,
                                     const Astra::FieldInfo& f)
    {
        FieldMixedMask mask;
        const FieldKind kind = ClassifyField(f);
        const int count = FieldComponentCount(kind);

        // Seed from the FIRST live carrier, then mark any later disagreement --
        // UE's CacheDetails shape. `seeded` stands in for UE's "ObjectIndex == 0"
        // branch, which is not simply "index 0" for us because a selection entry
        // can be dead or lack the component.
        bool seeded = false;
        float  seedF[3] = {};
        int32_t seedI = 0;
        bool   seedB = false;
        Arcane::Guid seedG{};

        for (Astra::Entity e : selection)
        {
            void* data = reg.GetComponentByHash(e, componentHash);
            if (!data)
                continue;   // dead entity or missing component: not a voter

            float  curF[3] = {};
            int32_t curI = 0;
            bool   curB = false;
            Arcane::Guid curG{};

            switch (kind)
            {
                case FieldKind::Bool:
                    if (const bool* p = f.GetPtr<bool>(data)) curB = *p;
                    break;
                case FieldKind::Int32:
                    if (const int32_t* p = f.GetPtr<int32_t>(data)) curI = *p;
                    break;
                case FieldKind::Float:
                    if (const float* p = f.GetPtr<float>(data)) curF[0] = *p;
                    break;
                case FieldKind::Vec2:
                    if (const glm::vec2* p = f.GetPtr<glm::vec2>(data))
                    { curF[0] = p->x; curF[1] = p->y; }
                    break;
                case FieldKind::Vec3:
                    if (const glm::vec3* p = f.GetPtr<glm::vec3>(data))
                    { curF[0] = p->x; curF[1] = p->y; curF[2] = p->z; }
                    break;
                case FieldKind::AssetRef:
                    if (const Arcane::Guid* p = f.GetPtr<Arcane::Guid>(data)) curG = *p;
                    break;
                case FieldKind::ReadOnly:
                default:
                    return mask;   // nothing comparable; never mixed
            }

            if (!seeded)
            {
                seeded = true;
                seedB = curB; seedI = curI; seedG = curG;
                for (int i = 0; i < count; ++i) seedF[i] = curF[i];
                continue;
            }

            // Exact comparison on purpose: this asks "did the user author the
            // same value", not "are these near each other". UE compares with ==
            // too (Loc.X == CurLoc.X).
            switch (kind)
            {
                case FieldKind::Bool:
                    if (curB != seedB) mask.bits |= 1u;
                    break;
                case FieldKind::Int32:
                    if (curI != seedI) mask.bits |= 1u;
                    break;
                case FieldKind::AssetRef:
                    if (!(curG == seedG)) mask.bits |= 1u;
                    break;
                default:
                    for (int i = 0; i < count; ++i)
                        if (curF[i] != seedF[i])
                            mask.bits |= (1u << i);   // sticky: never cleared
                    break;
            }
        }
        return mask;
    }
```

If `Arcane::Guid` has no `operator==`, compare its members instead — check
`Arcane/Guid.hpp` rather than assuming.

- [ ] **Step 3: Write the tests**

Append to `Arcane/Tests/src/EditorInspectorTest.cpp` (match its existing fixture
idiom — read the top of the file first; it must use
`Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());`, never a bare Runtime):

Cover, using `Arcane::Transform` (has `position` vec2 / `rotation` float / `scale` vec2
— confirm the real field names by reading `Arcane/Scene/Components.hpp`):

1. Single-entity selection is never mixed (`mask.bits == 0`).
2. Two entities with identical values: not mixed.
3. Two entities differing in ONE axis only: exactly that bit set, others clear.
4. Three entities where a third re-matches the seed on an axis already marked by the
   second: the bit STAYS set (the sticky rule — this is the one that catches a naive
   "compare against previous" implementation).
5. A dead entity and an entity lacking the component are skipped, not counted as
   differing.
6. `FieldComponentCount`: Vec3 → 3, Vec2 → 2, Float/Int32/Bool/AssetRef → 1.

- [ ] **Step 4: Build and run**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]" --rng-seed 6
```
Expected: all pass, including the six new assertions groups.

- [ ] **Step 5: Full gate + commit**

```
.\ArcaneTests.exe ~[gpu] --rng-seed 6
.\ArcaneTests.exe ~[gpu] --rng-seed 17
```
Expected: `All tests passed`, count above the 29608/514 baseline.

```bash
git add Arcane/ArcaneEditor/src/InspectorFields.hpp Arcane/ArcaneEditor/src/InspectorFields.cpp Arcane/Tests/src/EditorInspectorTest.cpp
git commit -m "feat(arcane-editor): pure per-component mixed-value mask for multi-select (UE parity task 1)"
```

---

### Task 2: Commit-only editing on a multi-selection

Implements ground truths 1 and 2 together: on a multi-selection the drag widget is
replaced by a type-and-commit input, and nothing is written until commit.

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`ImGuiFieldVisitor::Visit`)

**Interfaces:**
- Consumes: Task 1's `ComputeFieldMixed` / `FieldComponentCount`.
- Produces: no new public API.

- [ ] **Step 1: Add the multi-select predicate to the visitor**

In `ImGuiFieldVisitor`, beside the existing members, add:

```cpp
            // UE parity: a multi-selection gets NO drag widget and ignores every
            // non-committed change (ComponentTransformDetails.cpp:505 AllowSpin
            // and :1248 "Ignore interactive changes when we have more than one
            // selected object"). One entity keeps the drag exactly as before.
            [[nodiscard]] bool Multi() const noexcept
            { return selection && selection->size() > 1; }
```

- [ ] **Step 2: Replace the numeric cases**

For `Int32`, `Float`, `Vec2`, `Vec3`, replace the current
`ImGui::DragX(...)` + `if (changed) ForEachTarget(...)` shape with:

- `Multi() == false`: **unchanged** — keep `ImGui::DragInt` / `DragFloat` /
  `DragFloat2` / `DragFloat3` and the existing per-frame `changed` fan-out. Single
  selection behaviour must not regress.
- `Multi() == true`: draw `ImGui::InputInt` / `ImGui::InputFloat` /
  per-component `ImGui::InputFloat` for vectors, each with
  `ImGuiInputTextFlags_EnterReturnsTrue`, and apply the value ONLY when the widget
  returns true **or** `ImGui::IsItemDeactivatedAfterEdit()` fires. A component whose
  `mask.Test(i)` is set is drawn with an empty format string so it renders blank until
  the user types into it; typing into one blank component must write only that
  component, leaving the others untouched on every target.

Write the per-component vector path by hand (`ImGui::PushMultiItemsWidths` +
`ImGui::PushID(i)` per component) — `DragFloat2/3` share one format across components
and so cannot blank per-axis, which is what UE requires.

Keep `BeginGestureIfActivated` / `EndGesture` bracketing exactly as-is: the gesture
still opens on activation and commits on deactivate-after-edit, so one edit is still
one undo step across the fan-out.

- [ ] **Step 3: Build, prove the TU recompiled, gate**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
Get-ChildItem bin-int\Debug-windows-x86_64-md\ArcaneEditor\EditorPanels.obj, ArcaneEditor\src\EditorPanels.cpp | Select-Object Name, LastWriteTime
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu] --rng-seed 6
```
Expected: build clean, `.obj` postdates `.cpp`, gate green.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(arcane-editor): multi-select edits commit-only, no drag (UE parity task 2)"
```

---

### Task 3: Mixed values render blank

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp`

- [ ] **Step 1: Bool tri-state**

In the `Bool` case, when `Multi()` and the mask is set, wrap the `ImGui::Checkbox`
call in ImGui's native tri-state:

```cpp
                        const bool mixed = Multi() && ComputeFieldMixed(
                            *registry, *selection, descriptor->hash, f).Any();
                        if (mixed)
                            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                        bool changed = ImGui::Checkbox(label.c_str(), &v);
                        if (mixed)
                            ImGui::PopItemFlag();
```

`ImGuiItemFlags_MixedValue` lives in `imgui_internal.h`, which `EditorPanels.cpp`
already includes. Clicking a mixed checkbox resolves the whole selection to one value,
which is ImGui's documented tri-state behaviour and matches UE.

- [ ] **Step 2: Numeric blanking**

Feed the Task-1 mask into the Task-2 multi-select widgets so a differing component
draws blank. Compute the mask ONCE per field (not per component) and pass it down.

- [ ] **Step 3: Build, prove the TU recompiled, gate under both seeds, commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(arcane-editor): mixed multi-select values render blank (UE parity task 3)"
```

---

### Task 4: Record it

- [ ] **Step 1** Add a short section to
`docs/superpowers/specs/2026-07-25-editor-outliner-design.md` §4 recording that the two
follow-ups it deferred are now built, with the three UE citations.
- [ ] **Step 2** Ledger in `.superpowers/sdd/progress.md`, including the desk-verify
list below.
- [ ] **Step 3** Commit.

## Desk-verify checklist (no automated coverage for the ImGui half)

1. Select ONE entity: Transform drags behave exactly as before (smooth drag, one
   Ctrl+Z per drag). This is the no-regression check.
2. Select TWO entities with different X but the same Y: X renders blank, Y shows the
   shared value.
3. With two selected, try to DRAG a numeric field: nothing should drag (it is a text
   box now). Type a value + Enter: both entities take it, as ONE undo step.
4. With two selected, click into a field and click away without typing: no undo entry.
5. Select two entities whose bool field differs: the checkbox shows the filled-square
   mixed state; clicking it sets both.
6. Select three where the 1st and 3rd agree on an axis but the 2nd differs: that axis
   must still render blank (the sticky rule).
