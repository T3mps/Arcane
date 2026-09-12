# 2D Physics Wiring — Plan 2: Inspector `FieldKind::Vector` Editor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Inspector a real editor for `std::vector<ReflectedStruct>` fields — `Collider2D::fixtures` first — so fixture lists are authored in the editor instead of by hand in `.arcscene` JSON, with every add / remove / reorder / in-element edit landing as one undoable `ComponentEditCommand`; and land the α = 0.25 blend-direction pin Plan 1 owed.

**Architecture:** `ClassifyField` grows a `Vector` arm (pure, ImGui-free, `InspectorFields`) alongside four pure list operations over Astra's `FieldInfo` vector accessors. `InspectorView.cpp`'s field visitor grows a `Vector` arm that draws a header row (count + **+**) and, per element, a collapsible block whose rows are the element struct's own reflected fields drawn by the *existing* editors through a recursive `Visit` — an "element context" on the visitor re-targets the fan-out from the component to the element, and prefixes undo labels with the element path. Every mutation goes through the visitor's existing brackets (`ApplyImmediate` for one-shot list ops, the activation/deactivation gesture for in-element drags), so undo is a whole-component snapshot exactly as today. A test seam on `InspectorState` records the screen centre of every list control so a device-less ImGui drive can click the real path.

**Tech Stack:** C++23, Astra reflection (`FieldInfo::{isVector, elementTypeHash, elementSize, vectorSize, vectorResize, vectorElement, vectorErase, vectorInsert}` — vendored at ABI 28 by Plan 1 Task 2), Dear ImGui (vendored; tables, tree nodes, `io.AddMousePosEvent`/`AddMouseButtonEvent` for the drive), Catch2, premake5 / MSBuild (VS 18).

**Spec:** `docs/specs/2026-09-11-physics-2d-wiring-design.md` — §7.3 (the editor), §8's "§7.3" and "Plan 2's owed case" test rows, §9 (build ritual, plans). Plan 1 (`docs/plans/2026-09-11-physics-2d-wiring-plan1-runtime.md`, closed at `371fa042`) is the record of everything this plan consumes; its `## Closeout` section and the SDD ledger `.superpowers/sdd/2026-09-11-physics-2d-wiring-plan1-runtime/progress.md` carry the rulings.

## Global Constraints

- **No ABI bump, no Gacha restamp.** This plan touches `ArcaneEditor/src`, `ArcaneTests/src` and the test project's premake file list only. Nothing under `ArcaneClient/src` or `ThirdParty/Astra` changes. If a task finds it must change an `ARCANE_API` type, a reflect block, or Astra: **stop and report** — that is an ABI bump (ledger entry + `ReferenceProject.arcproj` + Gacha restamp) and out of this plan's scope. Task 5 proves it with `git diff --stat <plan-start>..HEAD -- ArcaneClient ThirdParty/Astra` being empty.
- **Build ritual (spec §9, unchanged):** `GenerateProjects.bat` when `premake5.lua` changes (Task 2 only) → `ReferenceProject.slnx` first per configuration (`/t:Rebuild` on every config flip of the single-slot `ReferenceProject\Binaries\`) → `Arcane.slnx` → suites `FROM the exe directory`. MSBuild is `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe`; from Git Bash pass `/p:`-style switches through PowerShell (MSYS path conversion mangles them — Plan 1's Task 9 lesson). Absolute `--project` for every host launch.
- **Every task ends green** on the filters it names (Debug); Task 5 runs Debug unfiltered + `~[gpu]` and Release `~[gpu]` plus `check-baselines.ps1` and `golden-gate.ps1 -Configuration Release`.
- **Output hygiene:** `msbuild ... /v:m > log; Select-String 'error|Warning\(s\)|Error\(s\)'`; `ArcaneTests.exe "<filter>" | Select-String 'seeded|test cases|All tests passed|FAILED'`; never `cat` a suite log; `Read` files with offset/limit at the symbol.
- **Baseline:** 56399 / 1655 (`~[gpu]`, Debug and Release, Plan 1 close, `scripts/automation-baselines.json` @ `6400581c`). Every case this plan adds is device-less and tagged `[editor][physics]` or `[interp]` — **none is `[gpu]`**, so the `~[gpu]` rise equals the unfiltered rise. Expected: T1 +2, T2 +2, T3 +5, T4 +1 = **+10 cases**; assertions attributed at close from the per-case JSON.
- **Golden lanes:** untouched *by construction* — the editor-ui golden boots `main.arcscene` with **no selection** ("No selection" in the Inspector), so no Collider2D row ever draws in it. Task 5 still runs the gate rather than assuming (Plan 1's Task 11 lesson: the last "no render change" premise was false for editor chrome). The View menu is not touched here.
- **Ledger:** `.superpowers/sdd/2026-09-12-physics-2d-wiring-plan2-inspector/progress.md` — `Task N: complete (sha)` lines, every controller `Ruling:` inline, counts per task. Commit per task; never push without being asked.
- **Manifold2D:** the editor and the tests compile it (`RegisterPhysicsComponents`, `Manifold2D::Physics::ShapeKind`); game modules do not. `ArcaneEditor.exe` does **not** link `Manifold2D.lib` — nothing in this plan calls an out-of-line `PhysicsWorld` method from the editor (Plan 1 Task 8's ruling).

---

## Plan-time rulings (spec amendments, each with its reason)

These are decisions the code forced while planning; the executor applies them as written and does not re-derive them.

| # | Ruling | Why |
|---|---|---|
| A1 | **`Vector` classifies only a `std::vector` of a *reflected struct* every one of whose fields classifies to a non-`ReadOnly` kind.** Vectors of arithmetic / `glm` / enum elements stay `ReadOnly`; a struct containing a nested vector stays `ReadOnly`. (Spec §7.3 listed scalar/glm/enum elements as drawable.) | Every scalar editor in `InspectorView.cpp` keys off an `Astra::FieldInfo` (`f.Get<T>`, `ApplyFloatEdit(f, …)`, `RangeOfField(f)`); an element of `std::vector<float>` has none. This is the same line `ReflectionJson.hpp::IsHandledType` drew in Plan 1 Task 3 for the same reason — and the bridge could not save what such an editor produced anyway. No roster field is such a vector. Recorded follow-up, not a gap. |
| A2 | **A multi-selection draws the count row only** — no **+**, no element rows. | `ForEachTarget` hands editors the *component* instance and `ComputeFieldMixed` / `MultiScalarRow` read each field at *its* offset from each entity's component; an element field's offset is element-relative, and the lists can differ in length across the selection. UE's Details panel shows "Multiple Values" for an array in the same case. |
| A3 | **Reorder swaps elements bytewise** (`vectorElement` + `elementSize`, `memcpy` through a temp). | `FieldInfo` records no trivially-copyable bit and Astra has no `vectorSwap`. Every vector element on the roster is trivially copyable (`Fixture`, `MeshSlot`). A future non-trivial element needs an Astra `vectorSwap` first — recorded follow-up; the helper's comment says so. |
| A4 | **Element rows bypass the category selector and the search filter** (they keep the `Astra::Hidden` check). | The vector field's own row already passed both; an element's fields have no category of their own to be drawn under elsewhere, and the panel's existing rule for a header hit ("show every field") is the right analogue for a list hit. |
| A5 | **`InspectorState::vectorProbe`** — an optional `std::unordered_map<std::string, glm::vec2>*` the vector editor fills with each list control's screen centre when non-null; production leaves it null. | ImGui exposes no item-rect registry; the spec's "device-less ImGui drive" needs a mouse target. Same "exposed on purpose so the test can reach it" shape as `AssetGraphPanelState::graphCanvas`; one null-check per row in production. |
| A6 | **`InspectorView.cpp` joins the test exe's explicit source list.** | It is not there today (only `InspectorFields.cpp` / `InspectorMeta.cpp`, the pure halves, are). Precedent and comment style: `AssetGraphPanel.cpp`'s entry in `premake5.lua`. Link closure was checked at plan time: every symbol it calls outside its TU is already compiled into the tests (`InspectorFields`, `InspectorMeta`, `EditorWidgets`, `ColorPickerPopup`, `EditGesture`, `AssetPanelModel`) or header-inline (`CreateAssetDialog.hpp`'s `MaterialSurfacePillText`, `AssetPanelModel.hpp`'s three field-name heuristics). |
| A7 | **List ops are deferred past the element walk** — a button records a `PendingListOp`; it is applied after the loop. | The walk reads `vectorElement(instance, i)` for each `i`; resizing under it would invalidate the element pointers mid-frame. |
| A8 | **Undo labels carry the element path** — `Edit Arcane::Collider2D.fixtures[0].radius`, `…fixtures.add` / `.remove` / `.move`. | A bare `radius` could be any element's; the label is what the Edit menu shows. |

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `ArcaneEditor/src/Panels/InspectorFields.hpp` / `.cpp` | Modify | `FieldKind::Vector`; `VectorElementsClassify`; `VectorSize` / `ApplyVectorInsert` / `ApplyVectorErase` / `ApplyVectorSwap` (pure, ImGui-free, unit-driven). |
| `ArcaneEditor/src/Panels/EditorPanels.hpp` | Modify (`InspectorState`) | `vectorProbe` test seam (A5). |
| `ArcaneEditor/src/Panels/InspectorView.cpp` | Modify | The `Vector` arm; `ElementContext`; `ForEachTarget` element remap; prefixed undo labels; `PendingListOp` + `ApplyListOp`; `ElementHeaderRow`; `RecordProbe`. |
| `ArcaneEditor/src/Panels/EditorPanels.cpp` | Modify (comment only) | The `(no fields editable here)` branch's comment names Collider2D as "the case today" — stale since Plan 1; rewritten. |
| `premake5.lua` (`ArcaneTests` `files {}`) | Modify | `+ InspectorView.cpp` (A6). |
| `ArcaneTests/src/EditorInspectorVectorTest.cpp` | Create | Part 1: classification + list-op units. Part 2: `VectorHarness` (device-less ImGui over one Collider2D) + the drive cases. |
| `ArcaneTests/src/EditorInspectorMetaTest.cpp` | Modify (comments only) | Two comments still describe `Collider2D::fixtures` as `Serializable(false)` / "Collider2D's shape today"; rewritten (the probes the test uses are unchanged). |
| `ArcaneTests/src/RenderInterpolationTest.cpp` | Modify | + the α = 0.25 integration case. |
| `scripts/automation-baselines.json` | Modify (Task 5) | Book the rise. |
| `docs/plans/2026-09-12-physics-2d-wiring-plan2-inspector.md` | Modify (Task 5) | `## Closeout`. |
| `docs/specs/2026-09-11-physics-2d-wiring-design.md` | Modify (Task 5, one line) | Status line: Plans 1–2 closed; §7.3 gains a one-line pointer to rulings A1–A3. |

---

### Task 1: `FieldKind::Vector` classification + the pure list operations

**Files:**
- Modify: `ArcaneEditor/src/Panels/InspectorFields.hpp` (enum at :31-32; new declarations after `ApplyEnumEdit` at :189)
- Modify: `ArcaneEditor/src/Panels/InspectorFields.cpp` (`ClassifyField` at :24-66; new definitions after `ApplyEnumEdit`'s)
- Create: `ArcaneTests/src/EditorInspectorVectorTest.cpp` (part 1)

**Interfaces:**
- Consumes: `Astra::FieldInfo::{isVector, isPointer, elementTypeHash, elementSize, vectorSize, vectorResize, vectorElement, vectorErase, vectorInsert}` (`ThirdParty/Astra/include/Astra/Reflection/FieldInfo.hpp:60-76` — every accessor takes the **containing** instance, `vectorElement` is nullptr past the end, erase past the end no-ops, insert past the end appends); `Astra::GetMeta(uint64_t)` → `const Astra::TypeMeta*` with `->fields` and `->GetEnumInfo()`.
- Produces (used by Tasks 2–3):
  ```cpp
  enum class FieldKind { Bool, Int32, UInt32, Float, Vec2, Vec3, Vec4, Quat, AssetRef, String, Enum, Vector, ReadOnly };
  [[nodiscard]] bool VectorElementsClassify(const Astra::FieldInfo& f) noexcept;
  [[nodiscard]] std::size_t VectorSize(const Astra::FieldInfo& f, const void* instance) noexcept;
  void ApplyVectorInsert(const Astra::FieldInfo& f, void* instance, std::size_t at) noexcept;
  void ApplyVectorErase (const Astra::FieldInfo& f, void* instance, std::size_t at) noexcept;
  void ApplyVectorSwap  (const Astra::FieldInfo& f, void* instance, std::size_t a, std::size_t b);
  ```

- [ ] **Step 1: Write the failing tests** — create `ArcaneTests/src/EditorInspectorVectorTest.cpp` (the test glob `ArcaneTests/src/**.cpp` picks it up; no premake change for this task):

```cpp
// 2D physics wiring Plan 2 (spec s7.3): the Inspector's FieldKind::Vector.
// Part 1 -- the PURE half (classification + list ops over Astra's element
// accessors), driven headlessly like every other InspectorFields unit.
// Part 2 (Task 2 onward) -- the device-less ImGui drive of the REAL row.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Astra/Reflection/Macros.hpp>
#include <Astra/Reflection/TypeMeta.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Panels/InspectorFields.hpp>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <string>
#include <vector>

using Catch::Approx;

// Witness types for the Vector arm's refusals. Named namespace, not
// anonymous, for the same reason EditorInspectorTest.cpp's probes are:
// ASTRA_REFLECT_TYPE declares a static inline registrar.
namespace ArcaneEditorVectorTest
{
    // A reflected struct with a field this panel has no widget for --
    // glm::mat4, WorldTransform::matrix's own kind -- so a vector of it must
    // refuse WHOLE (ruling A1): an element the editors could only half-draw
    // is not offered at all.
    struct Opaque
    {
        glm::mat4 m{1.0f};
    };

    ASTRA_REFLECT_TYPE(Opaque)
        ASTRA_REFLECT_FIELD(Opaque, m)
    ASTRA_END_REFLECT_TYPE()

    struct VectorProbe
    {
        std::vector<int>             ints;      // scalar elements: ReadOnly (bridge parity, A1)
        std::vector<Arcane::Fixture> fixtures;  // reflected struct, every field classifies: Vector
        std::vector<Opaque>          opaques;   // reflected struct, one field ReadOnly: ReadOnly
    };

    ASTRA_REFLECT_TYPE(VectorProbe)
        ASTRA_REFLECT_FIELD(VectorProbe, ints)
        ASTRA_REFLECT_FIELD(VectorProbe, fixtures)
        ASTRA_REFLECT_FIELD(VectorProbe, opaques)
    ASTRA_END_REFLECT_TYPE()
}

namespace
{
    const Astra::FieldInfo* FieldOf(const Astra::TypeMeta* m, const char* name)
    {
        if (!m) return nullptr;
        for (const Astra::FieldInfo& f : m->fields)
            if (f.name == name)
                return &f;
        return nullptr;
    }
}

TEST_CASE("ClassifyField: Vector arm -- a vector of a fully-classifiable reflected struct, nothing else",
          "[editor][physics]")
{
    using K = Arcane::Editor::FieldKind;

    // The roster's own witness: Collider2D::fixtures, serializable again
    // since Plan 1 Task 3 and visited by Astra ever since.
    const Astra::FieldInfo* fixtures = FieldOf(Astra::GetMeta<Arcane::Collider2D>(), "fixtures");
    REQUIRE(fixtures != nullptr);
    REQUIRE(fixtures->isVector);
    REQUIRE(static_cast<bool>(fixtures->vectorElement));   // Astra populated the accessors
    CHECK(Arcane::Editor::VectorElementsClassify(*fixtures));
    CHECK(Arcane::Editor::ClassifyField(*fixtures) == K::Vector);

    // The refusals (ruling A1), each by name so deleting a clause fails a
    // named line.
    const Astra::TypeMeta* probe = Astra::GetMeta<ArcaneEditorVectorTest::VectorProbe>();
    REQUIRE(probe != nullptr);
    REQUIRE(FieldOf(probe, "ints") != nullptr);
    REQUIRE(FieldOf(probe, "opaques") != nullptr);
    CHECK(Arcane::Editor::ClassifyField(*FieldOf(probe, "fixtures")) == K::Vector);
    CHECK(Arcane::Editor::ClassifyField(*FieldOf(probe, "ints"))     == K::ReadOnly);
    CHECK_FALSE(Arcane::Editor::VectorElementsClassify(*FieldOf(probe, "ints")));
    CHECK(Arcane::Editor::ClassifyField(*FieldOf(probe, "opaques"))  == K::ReadOnly);
    CHECK_FALSE(Arcane::Editor::VectorElementsClassify(*FieldOf(probe, "opaques")));

    // A vector is one "component" for the mixed-mask machinery (which never
    // diffs it -- ComputeFieldMixed's default arm returns an empty mask).
    CHECK(Arcane::Editor::FieldComponentCount(K::Vector) == 1);
}

TEST_CASE("Vector list ops: insert appends a default element, erase removes, swap exchanges whole elements",
          "[editor][physics]")
{
    namespace P = Manifold2D::Physics;
    const Astra::FieldInfo* f = FieldOf(Astra::GetMeta<Arcane::Collider2D>(), "fixtures");
    REQUIRE(f != nullptr);

    Arcane::Collider2D col;
    CHECK(Arcane::Editor::VectorSize(*f, &col) == 0);

    // Append on empty: a DEFAULT Fixture (Circle, r 0.5 -- the struct's own
    // initialisers), nothing copied from anywhere.
    Arcane::Editor::ApplyVectorInsert(*f, &col, 0);
    REQUIRE(col.fixtures.size() == 1);
    CHECK(col.fixtures[0].kind == P::ShapeKind::Circle);
    CHECK(col.fixtures[0].radius == Approx(0.5f));
    CHECK(Arcane::Editor::VectorSize(*f, &col) == 1);

    // Past-the-end appends (Astra's contract); the existing element is untouched.
    col.fixtures[0].radius = 2.0f;
    Arcane::Editor::ApplyVectorInsert(*f, &col, 99);
    REQUIRE(col.fixtures.size() == 2);
    CHECK(col.fixtures[0].radius == Approx(2.0f));
    CHECK(col.fixtures[1].radius == Approx(0.5f));

    // Insert at the front shifts the rest down.
    Arcane::Editor::ApplyVectorInsert(*f, &col, 0);
    REQUIRE(col.fixtures.size() == 3);
    CHECK(col.fixtures[0].radius == Approx(0.5f));
    CHECK(col.fixtures[1].radius == Approx(2.0f));

    // Swap moves WHOLE elements (every field), not just the one looked at.
    col.fixtures[2].kind  = P::ShapeKind::Aabb;
    col.fixtures[2].halfW = 3.0f;
    col.fixtures[2].isSensor = true;
    Arcane::Editor::ApplyVectorSwap(*f, &col, 1, 2);
    CHECK(col.fixtures[1].kind  == P::ShapeKind::Aabb);
    CHECK(col.fixtures[1].halfW == Approx(3.0f));
    CHECK(col.fixtures[1].isSensor);
    CHECK(col.fixtures[2].kind  == P::ShapeKind::Circle);
    CHECK(col.fixtures[2].radius == Approx(2.0f));
    CHECK_FALSE(col.fixtures[2].isSensor);

    // Out-of-range and self swaps are no-ops.
    Arcane::Editor::ApplyVectorSwap(*f, &col, 0, 7);
    Arcane::Editor::ApplyVectorSwap(*f, &col, 1, 1);
    CHECK(col.fixtures[0].radius == Approx(0.5f));
    CHECK(col.fixtures[1].kind == P::ShapeKind::Aabb);

    // Erase removes exactly that element; past-the-end is a no-op.
    Arcane::Editor::ApplyVectorErase(*f, &col, 0);
    REQUIRE(col.fixtures.size() == 2);
    CHECK(col.fixtures[0].kind == P::ShapeKind::Aabb);
    CHECK(col.fixtures[1].radius == Approx(2.0f));
    Arcane::Editor::ApplyVectorErase(*f, &col, 5);
    CHECK(col.fixtures.size() == 2);

    // Null-instance guards: every op is a no-op rather than a crash.
    CHECK(Arcane::Editor::VectorSize(*f, nullptr) == 0);
    Arcane::Editor::ApplyVectorInsert(*f, nullptr, 0);
    Arcane::Editor::ApplyVectorErase(*f, nullptr, 0);
    Arcane::Editor::ApplyVectorSwap(*f, nullptr, 0, 1);
    CHECK(col.fixtures.size() == 2);
}
```

- [ ] **Step 2: Build the tests and confirm they fail to compile** (`FieldKind::Vector`, `VectorElementsClassify`, `VectorSize`, `ApplyVector*` do not exist yet):

```powershell
cd D:\dev\starworks\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:m > build.log 2>&1; $LASTEXITCODE
Select-String -Path build.log -Pattern 'error' | Select-Object -First 5
```
Expected: exit 1; `error C2065`/`C2039` naming `Vector` / `VectorElementsClassify` in `EditorInspectorVectorTest.cpp`.

- [ ] **Step 3: The header** — `ArcaneEditor/src/Panels/InspectorFields.hpp`. Replace the enum (:31-32) and add the declarations after `ApplyEnumEdit` (:189):

```cpp
    // Vector = a std::vector of a reflected struct, drawn as a header row
    // (count + add) over one collapsible block per element whose rows are
    // the element's OWN reflected fields through these same editors -- see
    // the FieldKind::Vector section below for what qualifies.
    enum class FieldKind
    { Bool, Int32, UInt32, Float, Vec2, Vec3, Vec4, Quat, AssetRef, String, Enum, Vector, ReadOnly };
```

```cpp
    // ---- FieldKind::Vector (2D physics wiring Plan 2, spec s7.3) ------------
    //
    // A std::vector field classifies Vector when its ELEMENT is a reflected
    // struct every one of whose fields classifies to a kind this panel has a
    // widget for. The element rows recurse through the existing editors, so
    // an element the editors could only half-draw is refused WHOLE (ReadOnly,
    // as every other compound type is). Vectors of scalars / glm / enums stay
    // ReadOnly too: every scalar editor keys off a FieldInfo (f.Get<T>,
    // ApplyFloatEdit(f, ..), RangeOfField(f)) and an element has none -- the
    // same line ReflectionJson.hpp's container branch drew, for the same
    // reason (and the bridge could not save what such an editor produced). A
    // struct carrying a nested vector is refused one level down for the same
    // reason. Recorded follow-ups, not gaps: no roster field is any of these.
    [[nodiscard]] bool VectorElementsClassify(const Astra::FieldInfo& f) noexcept;

    // Pure list mutations over the field's Astra element accessors
    // (FieldInfo::vectorSize / vectorInsert / vectorErase / vectorElement --
    // every one takes the CONTAINING component instance, never the vector).
    // ImGui-free so the [editor] units drive them directly; the view brackets
    // each into one ComponentEditCommand (a whole-component snapshot --
    // Collider2D::Serialize carries the vector) exactly as it brackets an
    // AssetRef pick. Null instance or a missing accessor: no-op, never a crash.
    [[nodiscard]] std::size_t VectorSize(const Astra::FieldInfo& f, const void* instance) noexcept;
    // Default-constructed element at `at`; `at >= size` appends (Astra's contract).
    void ApplyVectorInsert(const Astra::FieldInfo& f, void* instance, std::size_t at) noexcept;
    // `at >= size` is a no-op (Astra's contract).
    void ApplyVectorErase (const Astra::FieldInfo& f, void* instance, std::size_t at) noexcept;
    // Exchanges elements a and b BYTEWISE through vectorElement + elementSize.
    // Sound for a trivially copyable element, which every vector element on
    // the roster is (Fixture, MeshSlot): FieldInfo records no trivially-
    // copyable bit and Astra offers no vectorSwap, so a future non-trivial
    // element needs that accessor FIRST (recorded follow-up). Either index
    // past the end, or a == b, is a no-op. Not noexcept: the temp is a heap
    // buffer sized elementSize.
    void ApplyVectorSwap  (const Astra::FieldInfo& f, void* instance, std::size_t a, std::size_t b);
```

Add `#include <cstddef>` to the header's includes (next to `<cstdint>`).

- [ ] **Step 4: The implementation** — `ArcaneEditor/src/Panels/InspectorFields.cpp`. Add includes `<Astra/Reflection/TypeMeta.hpp>`, `<cstring>`, `<vector>` (keep whatever is already there). In `ClassifyField`, insert immediately after the `if (f.isPointer) return FieldKind::ReadOnly;` line (:26):

```cpp
        // A std::vector's own typeHash is the CONTAINER's, which no arm below
        // names -- so it is asked first, and the answer is about the ELEMENT
        // (VectorElementsClassify). Before the enum check too: a vector is
        // never isEnum, but the order says which question is being asked.
        if (f.isVector)
            return VectorElementsClassify(f) ? FieldKind::Vector : FieldKind::ReadOnly;
```

Then, after `ApplyEnumEdit`'s definition, add:

```cpp
    bool VectorElementsClassify(const Astra::FieldInfo& f) noexcept
    {
        if (!f.isVector)
            return false;
        // Astra populates the accessors only for a default-constructible,
        // non-bool element (FieldInfo.hpp:419) -- no accessors, no editor:
        // nothing here could add or address an element.
        if (!f.vectorSize || !f.vectorElement || !f.vectorInsert || !f.vectorErase)
            return false;
        // The element must be a reflected STRUCT: GetMeta resolves enums too,
        // so the enum info is checked away, and a fieldless struct has no
        // rows to draw. A scalar or glm element has no meta at all.
        const Astra::TypeMeta* em = Astra::GetMeta(f.elementTypeHash);
        if (!em || em->GetEnumInfo() != nullptr || em->fields.empty())
            return false;
        for (const Astra::FieldInfo& nf : em->fields)
        {
            if (nf.isVector)
                return false;   // no nesting (ruling A1 -- recorded follow-up)
            if (ClassifyField(nf) == FieldKind::ReadOnly)
                return false;   // refuse whole: no half-drawn elements
        }
        return true;
    }

    std::size_t VectorSize(const Astra::FieldInfo& f, const void* instance) noexcept
    {
        return (f.vectorSize && instance) ? f.vectorSize(instance) : 0;
    }

    void ApplyVectorInsert(const Astra::FieldInfo& f, void* instance, std::size_t at) noexcept
    {
        if (f.vectorInsert && instance)
            f.vectorInsert(instance, at);
    }

    void ApplyVectorErase(const Astra::FieldInfo& f, void* instance, std::size_t at) noexcept
    {
        if (f.vectorErase && instance)
            f.vectorErase(instance, at);
    }

    void ApplyVectorSwap(const Astra::FieldInfo& f, void* instance, std::size_t a, std::size_t b)
    {
        if (!f.vectorElement || !instance || a == b || f.elementSize == 0)
            return;
        void* pa = f.vectorElement(instance, a);
        void* pb = f.vectorElement(instance, b);
        if (!pa || !pb)
            return;   // either index past the end
        // Bytewise -- see the header on why this is sound for the roster's
        // elements and what a non-trivial element would need first.
        std::vector<std::byte> tmp(f.elementSize);
        std::memcpy(tmp.data(), pa, f.elementSize);
        std::memcpy(pa, pb, f.elementSize);
        std::memcpy(pb, tmp.data(), f.elementSize);
    }
```

- [ ] **Step 5: Build Debug, run the touched suites** — build as in Step 2 (expect exit 0, `0 Warning(s)`), then:

```powershell
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]" | Select-String 'seeded|test cases|All tests passed|FAILED'
```
Expected: `All tests passed`; the `[editor]` case count is the pre-task count **+2**. (`EditorInspectorTest.cpp`'s "every arm has a witness" case is unaffected: it never asserted `fixtures` was `ReadOnly`.) Then `.\ArcaneTests.exe "~[gpu]"` → all pass, **1657 cases** (+2 over 56399/1655's 1655); the assertion figure is whatever it measures — record it in the ledger, it is attributed at close, not predicted here.

- [ ] **Step 6: Commit**

```bash
cd D:/dev/starworks/Arcane
git add ArcaneEditor/src/Panels/InspectorFields.hpp ArcaneEditor/src/Panels/InspectorFields.cpp ArcaneTests/src/EditorInspectorVectorTest.cpp
git commit -m "feat(editor): FieldKind::Vector -- a vector of a fully-classifiable reflected struct classifies as an editable list; pure insert/erase/swap over Astra's element accessors"
```
(+ the session's commit trailer.)

---

### Task 2: The `Vector` arm — header row, **+**, the test seam, and the device-less drive

**Files:**
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp` (`InspectorState` at :411-461; includes at :15-21)
- Modify: `ArcaneEditor/src/Panels/InspectorView.cpp` (includes at :1-34; visitor members at :93-168; `ApplyImmediate` at :254-265; the `switch (kind)` at :463; `DrawReflectedComponent` at :1379-1407)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp` (comment at :2355-2369)
- Modify: `ArcaneTests/src/EditorInspectorMetaTest.cpp` (comments at :158-161 and :174-176)
- Modify: `premake5.lua` (`ArcaneTests` `files {}` — append after :914, before the closing `}` at :915)
- Modify: `ArcaneTests/src/EditorInspectorVectorTest.cpp` (part 2)

**Interfaces:**
- Consumes: Task 1's `FieldKind::Vector`, `VectorSize`, `ApplyVectorInsert`/`Erase`/`Swap`; the visitor's existing `ApplyImmediate(const std::string& field, void* primaryInstance, Fn&& apply)` (one-shot: `ScopedTransaction` + `Snapshot` per target + apply + `Modified` — `InspectorView.cpp:254-265`); `Arcane::Editor::FieldGrid` (`Widgets/EditorWidgets.hpp:110`, the RAII 2-column grid `DrawReflectedComponent` draws rows into); `Arcane::Editor::ReflectedComponentArgs` (`Panels/InspectorView.hpp:31-53`, aggregate in declaration order); `Arcane::CommandStack(std::function<Astra::Registry&()>)` with `CanUndo/CanRedo/Undo/Redo/UndoLabel/Clear`; `Arcane::Test::SharedTypeContext()` (`Helpers/TestTypeContext.hpp`) — the type-context pin every registry-building editor test takes first.
- Produces:
  ```cpp
  // EditorPanels.hpp, InspectorState
  std::unordered_map<std::string, glm::vec2>* vectorProbe = nullptr;
  // InspectorView.cpp, ImGuiFieldVisitor (private to the TU)
  struct PendingListOp { enum Kind { Insert, Erase, Swap } kind; std::size_t a; std::size_t b; };
  void ApplyListOp(const std::string& rawName, const Astra::FieldInfo& f, void* instance, const PendingListOp& op);
  void RecordProbe(const std::string& key);
  ```
  Probe keys this task records: `"<rawName>.add"`. (Task 3 adds `"<rawName>[i].remove|up|down"` and `"<rawName>[i].<elementField>"`.)

- [ ] **Step 1: The test seam** — `ArcaneEditor/src/Panels/EditorPanels.hpp`. Add `#include <glm/vec2.hpp>   // InspectorState::vectorProbe` beside the existing `<glm/vec4.hpp>` include, and this member at the end of `InspectorState` (after `labelColWidth`):

```cpp
        // TEST SEAM (2D physics wiring Plan 2, FieldKind::Vector). When
        // non-null, the vector editor records the screen-space CENTRE of every
        // list control it draws this frame -- "<field>.add",
        // "<field>[i].remove" / ".up" / ".down", and each element field row as
        // "<field>[i].<elementField>" -- so a device-less test can aim
        // io.AddMousePosEvent at them and click through the REAL ImGui path
        // (EditorInspectorVectorTest.cpp). ImGui keeps no item-rect registry a
        // test could read instead. Production never sets it: nullptr, one
        // branch per control. Same "exposed on purpose so the test can reach
        // it" shape as AssetGraphPanelState::graphCanvas. Nothing here clears
        // it -- the test owns the map and clears it per frame.
        std::unordered_map<std::string, glm::vec2>* vectorProbe = nullptr;
```

- [ ] **Step 2: The test exe compiles the view TU** — `premake5.lua`, append inside the `ArcaneTests` `files {}` list, after the `AssetBrowserPanel.cpp` entry (:914) and before the closing `}`:

```lua
        -- 2D physics wiring Plan 2 (FieldKind::Vector): InspectorView -- the
        -- Inspector's reflected-field visitor, the ImGui half whose PURE
        -- halves (InspectorFields, InspectorMeta) are listed above. NOT a
        -- pure-logic unit: it is here so EditorInspectorVectorTest.cpp can
        -- drive the REAL DrawReflectedComponent through device-less ImGui
        -- frames and click its list controls -- the same reason, and the same
        -- precedent, as AssetGraphPanel.cpp above. The link closed with
        -- nothing else added: every symbol it calls outside its own TU is
        -- already compiled here (InspectorFields/InspectorMeta/EditorWidgets/
        -- ColorPickerPopup/EditGesture/AssetPanelModel) or header-inline
        -- (CreateAssetDialog.hpp's pill text, AssetPanelModel.hpp's three
        -- field-name heuristics).
        "%{wks.location}/ArcaneEditor/src/Panels/InspectorView.cpp",
```

Then regenerate and build:

```powershell
cd D:\dev\starworks\Arcane
.\GenerateProjects.bat
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:m > build.log 2>&1; $LASTEXITCODE
Select-String -Path build.log -Pattern 'error|LNK2019|Warning\(s\)|Error\(s\)'
```
Expected: exit 0, no `LNK2019`. **If an `LNK2019` names a symbol:** find its defining TU (`grep -rn "<symbol>(" ArcaneEditor/src --include=*.cpp`), add THAT file to the same list with a comment in the same voice, rebuild, and record the addition as a `Ruling:` in the ledger (the plan-time closure check missed it). Do not stub the symbol.

- [ ] **Step 3: Write the failing drive test** — append to `ArcaneTests/src/EditorInspectorVectorTest.cpp` (below part 1). Add these includes at the top of the file with the others:

```cpp
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>

#include <Panels/InspectorView.hpp>     // DrawReflectedComponent, ReflectedComponentArgs
#include <Widgets/EditorWidgets.hpp>    // FieldGrid: the grid DrawReflectedComponent draws rows into

#include <imgui.h>

#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>

#include "Helpers/TestTypeContext.hpp"
```

and the harness + first two cases:

```cpp
// ===========================================================================
// Part 2 -- the device-less ImGui drive (spec s8, the s7.3 row). The REAL
// DrawReflectedComponent, inside the REAL FieldGrid, over ONE entity's
// Collider2D, with the window pinned at a known origin so a recorded item
// centre is a mouse target -- the GraphMouseHarness shape
// (AssetsGraphCanvasTest.cpp), which also reads its targets off a seam the
// panel exposes on purpose (there, ed::GetNodePosition; here,
// InspectorState::vectorProbe, ruling A5).
// ===========================================================================

namespace
{
    struct VectorHarness
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        Astra::Entity   e{};       // the primary: two fixtures, [0] a Circle, [1] an Aabb
        Astra::Entity   other{};   // a second carrier, for the multi-selection case
        Arcane::CommandStack undo{ [this]() -> Astra::Registry& { return reg; } };
        Arcane::Editor::InspectorState state;
        std::unordered_map<std::string, glm::vec2> probe;
        std::vector<Astra::Entity> selection;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx  = nullptr;

        VectorHarness()
        {
            // Pin Arcane.dll's TypeContext to the shared one BEFORE any
            // registration (EditorInspectorTest.cpp's MixedWorld rule: a bare
            // Runtime installs an unshared context and Edit ops then report 0).
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            Arcane::RegisterSceneComponents(reg);
            Arcane::RegisterPhysicsComponents(reg);
            e     = Make(/*radius*/ 0.5f,  /*halfW*/ 1.0f);
            other = Make(/*radius*/ 0.25f, /*halfW*/ 2.0f);
            selection = { e };

            // Device-less ImGui: no backend; a software font atlas satisfies
            // NewFrame. 1024 tall so every row of two 13-field fixtures is
            // inside the viewport and therefore hoverable.
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx  = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.vectorProbe = &probe;
        }

        ~VectorHarness()
        {
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev);
        }

        // [0] a Circle, [1] an Aabb: distinguishable by kind, so a reorder is
        // observable, and by a scalar each, so an in-element edit is too.
        Astra::Entity Make(float radius, float halfW)
        {
            namespace P = Manifold2D::Physics;
            Astra::Entity ent = reg.CreateEntity();
            Arcane::Collider2D col;
            Arcane::Fixture a; a.kind = P::ShapeKind::Circle; a.radius = radius;
            Arcane::Fixture b; b.kind = P::ShapeKind::Aabb;   b.halfW  = halfW;
            col.fixtures = { a, b };
            reg.AddComponent<Arcane::Collider2D>(ent, col);
            return ent;
        }

        const std::vector<Arcane::Fixture>& Fixtures()
        {
            return reg.GetComponent<Arcane::Collider2D>(e)->fixtures;
        }

        Astra::Registry::ComponentInfo Collider()
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
                if (ci.meta && ci.meta->typeName == "Arcane::Collider2D")
                    return ci;
            FAIL("the harness entity carries no Collider2D");
            return {};
        }

        // One frame of the REAL row path: the window pinned at the origin, the
        // grid opened the way DrawInspectorPanel opens it, one component, the
        // uncategorised pass (Collider2D's only field has no category).
        void Frame()
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(640.0f, 1000.0f), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            {
                Arcane::Editor::FieldGrid grid("##fields", state.labelColWidth);
                if (grid)
                {
                    const Astra::Registry::ComponentInfo ci = Collider();
                    Arcane::Editor::ReflectedComponentArgs args{
                        reg, ci, e, std::span<const Astra::Entity>(selection),
                        &undo, /*project*/ nullptr, /*services*/ nullptr, state,
                        "Collider 2D", std::string_view{}, std::string_view{} };
                    Arcane::Editor::DrawReflectedComponent(args);
                }
            }
            ImGui::End();
            ImGui::Render();   // draw data discarded -- no backend
        }

        glm::vec2 Centre(const std::string& key)
        {
            INFO("probe key: " << key);
            REQUIRE(probe.count(key) == 1);
            return probe.at(key);
        }

        // Hover, press, release -- one frame each, so each input lands on its
        // own NewFrame; ImGui::Button fires on the RELEASE frame.
        void Click(const std::string& key)
        {
            const glm::vec2 c = Centre(key);
            ImGui::GetIO().AddMousePosEvent(c.x, c.y);    Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, true);  Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, false); Frame();
        }

        // Press on the widget, move `dx` pixels right in ONE frame (past
        // ImGui's 3 px drag threshold, so DragBehavior applies the whole
        // delta that frame), release.
        void Drag(const std::string& key, float dx)
        {
            const glm::vec2 c = Centre(key);
            ImGui::GetIO().AddMousePosEvent(c.x, c.y);       Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, true);     Frame();
            ImGui::GetIO().AddMousePosEvent(c.x + dx, c.y);  Frame();
            ImGui::GetIO().AddMouseButtonEvent(0, false);    Frame();
        }
    };
}

TEST_CASE("Vector row: [+] appends one default element as ONE undo step; undo restores the list",
          "[editor][physics]")
{
    VectorHarness h;
    h.Frame();
    h.Frame();   // frame 1 seeds the grid's label column; the row is a target from frame 2
    REQUIRE(h.probe.count("fixtures.add") == 1);
    REQUIRE(h.Fixtures().size() == 2);
    REQUIRE_FALSE(h.undo.CanUndo());

    h.Click("fixtures.add");
    REQUIRE(h.Fixtures().size() == 3);
    CHECK(h.Fixtures()[2].kind == Manifold2D::Physics::ShapeKind::Circle);   // Fixture's defaults
    CHECK(h.Fixtures()[2].radius == Approx(0.5f));
    CHECK(h.Fixtures()[0].radius == Approx(0.5f));                            // the two existing, untouched
    CHECK(h.Fixtures()[1].halfW  == Approx(1.0f));
    REQUIRE(h.undo.CanUndo());
    CHECK(std::string(h.undo.UndoLabel()).find("fixtures.add") != std::string::npos);

    // Exactly one step: undo empties the stack, and it restores the list.
    h.undo.Undo();
    CHECK(h.Fixtures().size() == 2);
    CHECK_FALSE(h.undo.CanUndo());
    REQUIRE(h.undo.CanRedo());
    h.undo.Redo();
    CHECK(h.Fixtures().size() == 3);
}

TEST_CASE("Vector row under a multi-selection draws the count and no list controls",
          "[editor][physics]")
{
    // Ruling A2: the fan-out and the mixed-value seeds read at component
    // offsets, which an element field does not have.
    VectorHarness h;
    h.selection = { h.e, h.other };
    h.Frame();
    h.Frame();
    CHECK(h.probe.count("fixtures.add") == 0);
    CHECK(h.probe.empty());
    CHECK(h.Fixtures().size() == 2);
    CHECK_FALSE(h.undo.CanUndo());
}
```

- [ ] **Step 4: Build and run — the first case fails** (the `Vector` kind hits the `default:` arm and draws "unsupported"; no probe key is recorded):

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane\Arcane.slnx /p:Configuration=Debug /m /v:m > build.log 2>&1; $LASTEXITCODE
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "Vector row*" | Select-String 'seeded|test cases|FAILED|REQUIRE|probe key'
```
Expected: the `[+]` case FAILS at `REQUIRE(h.probe.count("fixtures.add") == 1)`; the multi-selection case passes vacuously (it will keep passing — that is fine, its assertion is the absence).

- [ ] **Step 5: The arm** — `ArcaneEditor/src/Panels/InspectorView.cpp`.

  (a) Includes: add `#include "Widgets/IconsLucide.h"` (the **+** glyph) beside the other `Widgets/` includes.

  (b) Visitor members — after `quatEulerViews` (:156), add:

```cpp
            // The vector editor's test seam -- InspectorState::vectorProbe,
            // null in production (see its comment). Wired by
            // DrawReflectedComponent like every other state pointer here.
            std::unordered_map<std::string, glm::vec2>* probe = nullptr;

            // Record the centre of the item JUST SUBMITTED under `key`. One
            // null-check per control when the seam is unwired.
            void RecordProbe(const std::string& key)
            {
                if (!probe)
                    return;
                const ImVec2 lo = ImGui::GetItemRectMin();
                const ImVec2 hi = ImGui::GetItemRectMax();
                (*probe)[key] = glm::vec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            }

            // One deferred list mutation for the Vector arm (ruling A7):
            // recorded by a button while the elements are being walked, applied
            // after the walk -- the walk holds vectorElement pointers that a
            // resize under it would invalidate.
            struct PendingListOp
            {
                enum Kind { Insert, Erase, Swap } kind;
                std::size_t a;   // Insert: position; Erase: index; Swap: first index
                std::size_t b;   // Swap: second index
            };

            // ONE immediate command per list op -- snapshot, mutate, snapshot,
            // push -- the AssetRef pick's exact shape (spec s7.3). The verb
            // is the undo label's tail: "Edit Arcane::Collider2D.fixtures.add".
            void ApplyListOp(const std::string& rawName, const Astra::FieldInfo& f, void* instance,
                             const PendingListOp& op)
            {
                const char* verb = op.kind == PendingListOp::Insert ? "add"
                                 : op.kind == PendingListOp::Erase  ? "remove"
                                 :                                    "move";
                ApplyImmediate(rawName + "." + verb, instance, [&](void* d)
                {
                    switch (op.kind)
                    {
                        case PendingListOp::Insert: Arcane::Editor::ApplyVectorInsert(f, d, op.a);       break;
                        case PendingListOp::Erase:  Arcane::Editor::ApplyVectorErase (f, d, op.a);       break;
                        case PendingListOp::Swap:   Arcane::Editor::ApplyVectorSwap  (f, d, op.a, op.b); break;
                    }
                });
            }
```

  (c) The arm — in the `switch (kind)` (:463), add a case before `case Arcane::Editor::FieldKind::ReadOnly:` (:1296):

```cpp
                    case Arcane::Editor::FieldKind::Vector:
                    {
                        // The HEADER row: the count + [+] in the value cell (the
                        // field's display name is in the label cell, opened
                        // above like every row). Element rows follow it (the
                        // walk below); an insert is DEFERRED past them (ruling
                        // A7) and bracketed as ONE immediate command.
                        const std::size_t n = Arcane::Editor::VectorSize(f, instance);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%zu element%s", n, n == 1 ? "" : "s");
                        if (Multi())
                        {
                            // A multi-selection draws the count and nothing
                            // else (ruling A2): the fan-out reads a field at
                            // ITS offset from each entity's COMPONENT
                            // (ComputeFieldMixed, the multi rows' seeds) --
                            // wrong for an element, whose offset is element-
                            // relative -- and the lists can differ in length
                            // across the selection. UE's Details panel shows
                            // "Multiple Values" for the array here; same
                            // refusal, said in the tooltip.
                            tooltipValue = "multi-selection: edit one entity at a time";
                            hovered = false;   // the count text is not a hover target
                            break;
                        }
                        ImGui::SameLine();
                        std::optional<PendingListOp> pending;
                        if (ImGui::SmallButton(ICON_LC_PLUS "##add"))
                            pending = PendingListOp{ PendingListOp::Insert, n, 0 };
                        RecordProbe(rawName + ".add");

                        // (Task 3: the per-element blocks are walked here.)

                        if (pending)
                            ApplyListOp(rawName, f, instance, *pending);
                        // The last item is a list control or an element row,
                        // never this row's own content: the header tooltip
                        // follows the label alone.
                        hovered = false;
                        break;
                    }
```

  (d) Wire the seam — in `DrawReflectedComponent` (:1379), after `visitor.quatEulerViews = &args.state.quatEulerViews;`:

```cpp
        visitor.probe          = args.state.vectorProbe;   // null in production
```

- [ ] **Step 6: Stale prose, both sides** (comment-only; no behaviour):

  `ArcaneEditor/src/Panels/EditorPanels.cpp` :2355-2369 — replace the paragraph from `// Every reflected field is non-serializable or Hidden, so the` through `// indistinguishable from a rendering glitch.` with:

```cpp
                    // Every reflected field is non-serializable or Hidden, so the
                    // grid below would open, visit nothing and close -- a header
                    // over a void. Arcane::Collider2D WAS the case when this row
                    // was added: its only field, `fixtures`, was Serializable(false)
                    // because the reflection->JSON bridge had no container branch,
                    // and adding a Collider2D from the catalog gave NO confirmation
                    // it had done anything. Both halves are gone (2D physics wiring
                    // Plans 1-2: the bridge grew the branch, the Inspector grew
                    // FieldKind::Vector), so Collider2D now draws a real list; the
                    // row stays for the next component that reflects only what it
                    // cannot show.
```

  `ArcaneTests/src/EditorInspectorMetaTest.cpp` :158-161 — replace the comment above `CHECK_FALSE(FieldIsDrawable(ProbeField("unwritten")));` with:

```cpp
    // Serializable(false) -- dropped one frame out, by Astra's VisitFields,
    // before Visit() is ever called. PhysicsBodyRef's two fields are the
    // roster's case (Collider2D::fixtures was, until 2D physics wiring Plan 1
    // made it serializable again); missing it is what once left a component
    // drawing an empty header.
```

  and :174-176 — replace `// Every field undrawable: the caller draws the disabled hint row instead of` / `// opening a grid that would visit nothing. Collider2D's shape today.` with:

```cpp
    // Every field undrawable: the caller draws the disabled hint row instead of
    // opening a grid that would visit nothing. (Collider2D's shape once; it
    // draws a FieldKind::Vector list since 2D physics wiring Plan 2.)
```

- [ ] **Step 7: Build and run** — build (exit 0, `0 Warning(s)`), then:

```powershell
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]" | Select-String 'seeded|test cases|All tests passed|FAILED'
```
Expected: `All tests passed`, `[editor]` case count = Task 1's **+2**. If the `[+]` case fails with the probe key present but `Fixtures().size()` still 2: the click did not land — `INFO` the recorded centre and check it is inside the window (y < 1000) and that `Click` ran three `Frame()`s; do not widen the window blindly, read the centre first.

- [ ] **Step 8: Commit**

```bash
cd D:/dev/starworks/Arcane
git add premake5.lua ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp ArcaneEditor/src/Panels/InspectorView.cpp ArcaneTests/src/EditorInspectorVectorTest.cpp ArcaneTests/src/EditorInspectorMetaTest.cpp
git commit -m "feat(editor): Inspector Vector row -- count + add as one immediate command; InspectorState::vectorProbe test seam; InspectorView.cpp joins the test exe for the device-less drive"
```

---

### Task 3: Element blocks — collapsible per-element rows through the existing editors; remove / up / down; the in-element gesture

**Files:**
- Modify: `ArcaneEditor/src/Panels/InspectorView.cpp` (visitor members; `ForEachTarget` at :185-201; `BeginGestureIfActivated` at :207-224; `ApplyImmediate` at :254-265; `ApplyGuidImmediate` at :371-; `Visit`'s prologue at :385-419; the tail after the `switch` at :1324-1325; the `Vector` arm from Task 2)
- Modify: `ArcaneTests/src/EditorInspectorVectorTest.cpp` (part 2, more cases)

**Interfaces:**
- Consumes: Task 2's arm, `PendingListOp`, `ApplyListOp`, `RecordProbe`, `VectorHarness::{Click, Drag, Fixtures, probe, undo}`; Task 1's `ApplyVectorErase`/`ApplyVectorSwap`; `Astra::GetMeta(f.elementTypeHash)->fields`; `f.vectorElement(instance, i)`.
- Produces (private to the TU):
  ```cpp
  struct ElementContext { const Astra::FieldInfo* vectorField; std::size_t index; std::string prefix; };
  const ElementContext* elementCtx = nullptr;   // non-null while an element's rows draw
  bool ElementHeaderRow(std::size_t i, std::size_t n, const std::string& rawName, std::optional<PendingListOp>& pending);
  ```
  Probe keys: `"<rawName>[i].remove"`, `"<rawName>[i].up"`, `"<rawName>[i].down"`, `"<rawName>[i].<elementField>"` (every element field row, keyed at the tail of `Visit`).

- [ ] **Step 1: Write the failing tests** — append to `EditorInspectorVectorTest.cpp`:

```cpp
TEST_CASE("Vector elements draw their reflected fields as rows through the existing editors",
          "[editor][physics]")
{
    VectorHarness h;
    h.Frame();
    h.Frame();
    // Every Fixture field, for both elements, is a row (and therefore a
    // recorded target): the enum, a float, the vec2, a uint32, the bool.
    for (const char* field : { "kind", "radius", "halfLen", "halfW", "halfH", "localPos",
                               "localAngle", "density", "friction", "restitution",
                               "categoryBits", "maskBits", "isSensor" })
    {
        INFO("field: " << field);
        CHECK(h.probe.count(std::string("fixtures[0].") + field) == 1);
        CHECK(h.probe.count(std::string("fixtures[1].") + field) == 1);
    }
    CHECK(h.probe.count("fixtures[0].remove") == 1);
    CHECK(h.probe.count("fixtures[0].up") == 1);
    CHECK(h.probe.count("fixtures[0].down") == 1);
    CHECK(h.probe.count("fixtures[1].down") == 1);
    CHECK(h.probe.count("fixtures[2].remove") == 0);   // no third element, no third block
}

TEST_CASE("Vector row: [-] removes exactly that element as ONE undo step; undo restores it in place",
          "[editor][physics]")
{
    namespace P = Manifold2D::Physics;
    VectorHarness h;
    h.Frame();
    h.Frame();

    h.Click("fixtures[0].remove");
    REQUIRE(h.Fixtures().size() == 1);
    CHECK(h.Fixtures()[0].kind == P::ShapeKind::Aabb);     // the Circle went, the Aabb stayed
    CHECK(h.Fixtures()[0].halfW == Approx(1.0f));
    REQUIRE(h.undo.CanUndo());
    CHECK(std::string(h.undo.UndoLabel()).find("fixtures.remove") != std::string::npos);

    h.undo.Undo();
    REQUIRE(h.Fixtures().size() == 2);
    CHECK(h.Fixtures()[0].kind == P::ShapeKind::Circle);   // back at index 0, not appended
    CHECK(h.Fixtures()[0].radius == Approx(0.5f));
    CHECK(h.Fixtures()[1].kind == P::ShapeKind::Aabb);
    CHECK_FALSE(h.undo.CanUndo());
}

TEST_CASE("Vector row: down / up reorder as ONE undo step each; the end buttons are inert",
          "[editor][physics]")
{
    namespace P = Manifold2D::Physics;
    VectorHarness h;
    h.Frame();
    h.Frame();

    h.Click("fixtures[0].down");
    REQUIRE(h.Fixtures().size() == 2);
    CHECK(h.Fixtures()[0].kind == P::ShapeKind::Aabb);
    CHECK(h.Fixtures()[0].halfW == Approx(1.0f));          // the WHOLE element moved
    CHECK(h.Fixtures()[1].kind == P::ShapeKind::Circle);
    CHECK(h.Fixtures()[1].radius == Approx(0.5f));
    REQUIRE(h.undo.CanUndo());
    CHECK(std::string(h.undo.UndoLabel()).find("fixtures.move") != std::string::npos);
    h.undo.Undo();
    CHECK(h.Fixtures()[0].kind == P::ShapeKind::Circle);
    CHECK_FALSE(h.undo.CanUndo());
    h.Frame();   // re-record the targets over the restored list before aiming again

    h.Click("fixtures[1].up");
    CHECK(h.Fixtures()[0].kind == P::ShapeKind::Aabb);
    REQUIRE(h.undo.CanUndo());
    h.undo.Undo();
    CHECK(h.Fixtures()[0].kind == P::ShapeKind::Circle);
    CHECK_FALSE(h.undo.CanUndo());
    h.Frame();

    // [0].up and [last].down are disabled: a click is a no-op with no step.
    h.Click("fixtures[0].up");
    CHECK(h.Fixtures()[0].kind == P::ShapeKind::Circle);
    CHECK_FALSE(h.undo.CanUndo());
    h.Click("fixtures[1].down");
    CHECK(h.Fixtures()[1].kind == P::ShapeKind::Aabb);
    CHECK_FALSE(h.undo.CanUndo());
}

TEST_CASE("Vector row: an in-element scalar drag is ONE gesture -- one undo step, labelled by element path",
          "[editor][physics]")
{
    VectorHarness h;
    h.Frame();
    h.Frame();
    REQUIRE(h.Fixtures()[0].radius == Approx(0.5f));

    // 40 px right on the radius drag (speed 0.1/px -> +4.0). The exact figure
    // is the widget's speed contract, not this test's: only the direction and
    // the bracket are asserted.
    h.Drag("fixtures[0].radius", 40.0f);
    CHECK(h.Fixtures()[0].radius > 0.5f);
    CHECK(h.Fixtures()[1].halfW == Approx(1.0f));           // the OTHER element untouched
    CHECK(h.Fixtures()[1].radius == Approx(0.5f));
    REQUIRE(h.undo.CanUndo());
    CHECK(std::string(h.undo.UndoLabel()).find("fixtures[0].radius") != std::string::npos);

    h.undo.Undo();
    CHECK(h.Fixtures()[0].radius == Approx(0.5f));
    CHECK_FALSE(h.undo.CanUndo());                          // exactly one step
    CHECK(h.undo.CanRedo());
}

TEST_CASE("Vector row: a pure click on an element drag pushes nothing", "[editor][physics]")
{
    // The existing bracket's Cancel-on-no-edit rule, proven to hold one level
    // down: activation opened a transaction, deactivation without an edit
    // cancels it, and the stack stays empty.
    VectorHarness h;
    h.Frame();
    h.Frame();
    h.Click("fixtures[1].halfW");
    CHECK(h.Fixtures()[1].halfW == Approx(1.0f));
    CHECK_FALSE(h.undo.CanUndo());
    CHECK_FALSE(h.undo.InTransaction());                    // nothing stranded open
}
```

- [ ] **Step 2: Build and run — all five fail** at their first probe `REQUIRE` (no element rows exist yet):

```powershell
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "Vector *" | Select-String 'test cases|FAILED|probe key'
```

- [ ] **Step 3: The element context** — `InspectorView.cpp`, visitor members (next to `probe`):

```cpp
            // FieldKind::Vector: set while ONE element's rows are being drawn
            // (a recursive Visit over the element struct's own fields), null
            // otherwise. It does two things. ForEachTarget re-targets the
            // fan-out from each entity's COMPONENT to that entity's element --
            // an element field's offset is element-relative, so the editors'
            // write-backs (ApplyFloatEdit(f, d, v) etc.) need `d` to BE the
            // element. And the undo-label builders prefix the field with the
            // element path (ruling A8): "fixtures[0].radius", never a bare
            // "radius" that could be any element's.
            struct ElementContext
            {
                const Astra::FieldInfo* vectorField;   // the vector field on the component
                std::size_t             index;         // this element
                std::string             prefix;        // "fixtures[0]."
            };
            const ElementContext* elementCtx = nullptr;

            // The undo-label path for `field` in the current context.
            [[nodiscard]] std::string LabelPath(const std::string& field) const
            {
                return elementCtx ? elementCtx->prefix + field : field;
            }
```

- [ ] **Step 4: The fan-out remap** — replace `ForEachTarget` (:185-201) with:

```cpp
            template<typename Fn>
            void ForEachTarget(void* primaryInstance, Fn&& fn)
            {
                if (!registry || selection.empty())
                {
                    // primaryInstance is already the element when an element
                    // row is drawing (Visit was handed the element pointer).
                    fn(entity, primaryInstance);
                    if (registry && descriptor)
                        (void)registry->Modified(entity, descriptor->id);
                    return;
                }
                for (Astra::Entity e : selection)
                    if (void* data = registry->GetComponentByHash(e, descriptor->hash))
                    {
                        // Element context (FieldKind::Vector): the fan-out
                        // hands fn the COMPONENT, but an element field's
                        // offset is relative to the ELEMENT -- re-derive this
                        // target's element from its own component through the
                        // vector field's accessor. A target whose list is
                        // shorter than the primary's has no such element and
                        // is skipped; unreachable today (ruling A2: elements
                        // draw for a single selection only), kept as the
                        // correct answer if that ever widens.
                        void* target = data;
                        if (elementCtx)
                        {
                            target = elementCtx->vectorField->vectorElement(data, elementCtx->index);
                            if (!target)
                                continue;
                        }
                        fn(e, target);
                        (void)registry->Modified(e, descriptor->id);
                    }
            }
```

- [ ] **Step 5: Prefixed undo labels** — three one-line edits:

  `BeginGestureIfActivated` (:210): `[&] { return "Edit " + typeName + "." + field; },` → `[&] { return "Edit " + typeName + "." + LabelPath(field); },`

  `ApplyImmediate` (:260): `txn.emplace(*stack, "Edit " + typeName + "." + field);` → `txn.emplace(*stack, "Edit " + typeName + "." + LabelPath(field));`

  `ApplyGuidImmediate` — its `txn.emplace(*stack, "Edit " + typeName + "." + field);` line likewise → `LabelPath(field)`.

- [ ] **Step 6: The prologue skips (ruling A4) and the tail probe** — in `Visit` (:385):

  (:390-391) `if (Arcane::Editor::CategoryOfField(f) != activeCategory) return;` → 
```cpp
                // Element rows (FieldKind::Vector) skip the group selector and
                // the search below (ruling A4): the VECTOR field's own row
                // already passed both, and an element's fields have no
                // category of their own to be drawn under elsewhere. They keep
                // the Hidden check -- an element can hide a field like anyone.
                if (!elementCtx && Arcane::Editor::CategoryOfField(f) != activeCategory)
                    return;
```
  (:417-419) `if (!Arcane::Editor::MatchesInspectorFilter(...)) return;` → guard it the same way: `if (!elementCtx && !Arcane::Editor::MatchesInspectorFilter(componentDisplayName, label, rawName, query)) return;`

  After the `switch (kind) { ... }` closes (:1324) and BEFORE `EndGesture();` (:1325), add:
```cpp
                // Element rows are mouse targets for the device-less drive:
                // every arm but the asset-ref one ends on the row's own
                // widget, so this is the widget's rect (recorded before
                // EndGesture reads item state, which a rect read leaves alone).
                if (elementCtx)
                    RecordProbe(elementCtx->prefix + rawName);
```

- [ ] **Step 7: The element header row + the walk** — add this member to the visitor (after `ApplyListOp`):

```cpp
            // One element's header row: a tree node in the label column (the
            // element index; DefaultOpen, so a fresh list shows its fields
            // without a click), the three list buttons in the value column.
            // Returns whether the node is open -- the caller draws the element's
            // field rows and TreePops. The "###" id suffix keeps the node's id
            // off its visible text, so the open state survives whatever a later
            // preview might add to the label.
            [[nodiscard]] bool ElementHeaderRow(std::size_t i, std::size_t n, const std::string& rawName,
                                                std::optional<PendingListOp>& pending)
            {
                const std::string elementKey = rawName + "[" + std::to_string(i) + "]";
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                const std::string nodeLabel = "[" + std::to_string(i) + "]###element";
                const bool open = ImGui::TreeNodeEx(nodeLabel.c_str(),
                                                    ImGuiTreeNodeFlags_DefaultOpen |
                                                    ImGuiTreeNodeFlags_SpanAvailWidth);
                ImGui::TableSetColumnIndex(1);
                if (ImGui::SmallButton(ICON_LC_TRASH_2 "##remove"))
                    pending = PendingListOp{ PendingListOp::Erase, i, 0 };
                RecordProbe(elementKey + ".remove");
                ImGui::SameLine();
                // The end buttons are DISABLED rather than hidden: the row keeps
                // its shape, and a disabled button is a target that does nothing
                // -- which the drive pins.
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton(ICON_LC_ARROW_UP "##up"))
                    pending = PendingListOp{ PendingListOp::Swap, i, i - 1 };
                ImGui::EndDisabled();
                RecordProbe(elementKey + ".up");
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 >= n);
                if (ImGui::SmallButton(ICON_LC_ARROW_DOWN "##down"))
                    pending = PendingListOp{ PendingListOp::Swap, i, i + 1 };
                ImGui::EndDisabled();
                RecordProbe(elementKey + ".down");
                return open;
            }
```

  and in the `Vector` arm, replace the `// (Task 3: the per-element blocks are walked here.)` line with:

```cpp
                        // The element walk. Each block is a tree node row plus
                        // the element struct's OWN reflected fields, drawn by
                        // this same visitor one level down: Visit is handed the
                        // ELEMENT pointer, and elementCtx re-targets the fan-out
                        // and the undo labels (Step 3-5). Nothing here resizes
                        // the list -- `pending` is applied after the walk.
                        const Astra::TypeMeta* em = Astra::GetMeta(f.elementTypeHash);   // non-null: Classify vetted it
                        for (std::size_t i = 0; i < n && em; ++i)
                        {
                            ImGui::PushID(static_cast<int>(i));
                            const bool open = ElementHeaderRow(i, n, rawName, pending);
                            if (open)
                            {
                                void* elem = f.vectorElement(instance, i);
                                const ElementContext ctx{ &f, i, rawName + "[" + std::to_string(i) + "]." };
                                const ElementContext* saved = elementCtx;
                                elementCtx = &ctx;
                                for (const Astra::FieldInfo& nf : em->fields)
                                    Visit(nf, elem);
                                elementCtx = saved;
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
```

- [ ] **Step 8: Build and run** — build (exit 0, `0 Warning(s)`), then:

```powershell
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]" | Select-String 'seeded|test cases|All tests passed|FAILED'
.\ArcaneTests.exe "[editor][physics]" | Select-String 'test cases|All tests passed|FAILED'
```
Expected: `All tests passed`; `[editor]` count = Task 2's **+5**. Diagnosis guide if a case fails:
  - rows/keys missing → the `TreeNodeEx` did not open (check `DefaultOpen` and that `PushID(i)` wraps it) or `em` was null (Classify and the walk disagree — `GetMeta(f.elementTypeHash)`).
  - drag changes nothing → the press did not activate the drag: `INFO` the centre and confirm it sits on the value cell (not the label cell — `RecordProbe` at the tail records the last ITEM, which for the Float arm is the drag).
  - drag changes the WRONG element / the component's first bytes → the remap in Step 4 is not being hit: `elementCtx` null during the write (the `saved`/restore around the inner loop) or `vectorField` pointing at a copy.
  - "one undo step" fails with two → `ApplyListOp` ran inside an open gesture; confirm it runs after the walk (Step 7's order) and that the drag case's release frame ran.

- [ ] **Step 9: A desk look (2 minutes, not a gate):** launch the Debug editor on the physics demo scene, select `Crate`, and eyeball the Collider 2D section: `[0]` open, `Aabb` in the kind combo, halfW/halfH 0.5, the three buttons; press **+** and Ctrl+Z.

```powershell
D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Arcane\ReferenceProject
```
(The editor boots `main.arcscene`; open `physics.arcscene` from the Asset Browser. The Debug host must be the staged config: if the tree was last built Release, `ReferenceProject.slnx` Debug `/t:Rebuild` first.) Note what you saw in the ledger — one line.

- [ ] **Step 10: Commit**

```bash
cd D:/dev/starworks/Arcane
git add ArcaneEditor/src/Panels/InspectorView.cpp ArcaneTests/src/EditorInspectorVectorTest.cpp
git commit -m "feat(editor): Vector element blocks -- per-element tree rows through the existing editors, remove/up/down as one command each, in-element gestures re-targeted to the element and labelled by path"
```

---

### Task 4: The owed α = 0.25 blend-direction pin (spec §8, "Plan 2's owed case")

**Files:**
- Modify: `ArcaneTests/src/RenderInterpolationTest.cpp` (append after the "snaps to the current pose on any buffer miss" case at :255-289)

**Interfaces:**
- Consumes: `Arcane::PhysicsSystem(float fixedDt, bool stepWorld = true)` (`PhysicsSystem.hpp:275`) — PASS 1.5 auto-adds `PhysicsBodyRef`, PASS 2.5 captures the pre-step pose into `PhysicsInterpBuffer`, PASS 4 writes the stepped pose back to `Transform`; `Arcane::TransformPropagationSystem` (composes `WorldTransform`); `Arcane::RenderSubmissionSystem` reading `RenderContext2D{batcher, cameraOffset, zoom, alpha}` (`SceneResources.hpp:92-102`); the file's own `RecBatcher` (:119-146, `lastRectCenter()`).
- Produces: one `[interp]` case. Nothing else consumes it.

- [ ] **Step 1: Write the test** — append:

```cpp
TEST_CASE("RenderSubmissionSystem blends FROM the captured pose TOWARD the current one: alpha 0.25 lands a quarter of the way",
          "[interp]")
{
    // The owed case (spec 2026-09-11-physics-2d-wiring s8, "Plan 2's owed
    // case"): the two hand-built cases above use alpha 0.5, which is
    // SYMMETRIC -- a reversed Lerp endpoint order would still pass them. This
    // one runs the REAL chain (PhysicsSystem PASS 2.5 capture -> step -> PASS
    // 4 write-back -> propagation) and asks at 0.25 and 0.75, which only the
    // right direction satisfies. The endpoints are MEASURED through the same
    // submit at alpha 0 and 1 rather than computed from world units, so the
    // assertion is about the blend and not about the batcher's screen mapping.
    namespace P = Manifold2D::Physics;
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    P::WorldDef wd; wd.gravityY = 10.0f;
    reg.SetResource(Arcane::PhysicsResource{ std::make_unique<P::PhysicsWorld>(wd), {} });
    reg.SetResource(Arcane::PhysicsInterpBuffer{});   // opt in to capture

    // One dynamic circle free-falling from the origin, with a sprite on it.
    // Untextured Rect sprite: nil .arcsprite -> a 1x1 m quad scaled 4x4.
    Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f); lt.scale = glm::vec3(4.0f, 4.0f, 1.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = P::BodyType::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col;
    { Arcane::Fixture fx; fx.kind = P::ShapeKind::Circle; fx.radius = 0.5f; col.fixtures.push_back(fx); }
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
    // No PhysicsBodyRef on purpose: PASS 1.5 adds it (Plan 1 Task 5).

    Arcane::PhysicsSystem physics(1.0f / 60.0f);
    Arcane::TransformPropagationSystem propagate;
    physics(reg);      // mint, capture prev = the authored pose, step, write back
    propagate(reg);    // WorldTransform = the post-step pose
    REQUIRE(reg.GetComponent<Arcane::PhysicsBodyRef>(e) != nullptr);
    REQUIRE(reg.GetResource<Arcane::PhysicsInterpBuffer>()->captured);

    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ nullptr, glm::vec2(0.0f), 1.0f, 0.0f });
    auto submitAt = [&](float alpha)
    {
        RecBatcher rec;
        Arcane::RenderContext2D* ctx = reg.GetResource<Arcane::RenderContext2D>();
        ctx->batcher = &rec;
        ctx->alpha   = alpha;
        Arcane::RenderSubmissionSystem{}(reg);
        REQUIRE(rec.rectCalls == 1);
        return rec.lastRectCenter().y;
    };

    const float atPrev = submitAt(0.0f);
    const float atCur  = submitAt(1.0f);
    REQUIRE(atPrev != Approx(atCur));   // the body moved this step: the endpoints differ
    const float span = atCur - atPrev;
    CHECK(submitAt(0.25f) == Approx(atPrev + 0.25f * span));
    CHECK(submitAt(0.75f) == Approx(atPrev + 0.75f * span));
    // And the reversed direction is what these two would read under a
    // swapped Lerp -- stated so the failure mode is named, not just implied.
    CHECK(submitAt(0.25f) != Approx(atPrev + 0.75f * span));
}
```

- [ ] **Step 2: Build and run** — build, then:

```powershell
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[interp]" | Select-String 'seeded|test cases|All tests passed|FAILED'
```
Expected: `All tests passed`, `[interp]` count **+1** (from the file's current 9). If `atPrev == atCur`: propagation did not compose the written-back Transform — check `propagate(reg)` ran after `physics(reg)` and that the entity has a `WorldTransform`. If the 0.25 check lands at 0.75: that is the defect this case exists to catch — **report it, do not flip the expectation**; the fix belongs in `RenderSubmissionSystem` (`RenderSystems.hpp:72` region), and it would be the first product bug this plan finds.

- [ ] **Step 3: Commit**

```bash
cd D:/dev/starworks/Arcane
git add ArcaneTests/src/RenderInterpolationTest.cpp
git commit -m "test(interp): pin the sprite blend DIRECTION at alpha 0.25/0.75 through the real PhysicsSystem capture -> step -> propagate chain (the case Plan 1 owed)"
```

---

### Task 5: Plan 2 close — sweep, both configs, counts, guard, gate, baselines, closeout

- [ ] **Step 1: Sweeps.**
  (a) `git diff --stat 371fa042..HEAD -- ArcaneClient ThirdParty/Astra` (THIS plan's range: `371fa042` is Plan 1's closeout commit) → **empty** (no ABI bump owed; Gacha needs nothing).
  (b) `git grep -n "Collider2D's shape today\|its only field is .fixtures., Serializable(false)" -- ArcaneEditor/src ArcaneTests/src` → empty (Task 2's prose fixes landed).
  (c) `git grep -n "FieldKind::Vector" -- ArcaneEditor/src ArcaneTests/src | wc -l` → non-zero; `git grep -n "vectorProbe" -- ArcaneEditor/src` → exactly the header declaration and the one `DrawReflectedComponent` wiring line (no production writer).
- [ ] **Step 2: Build order, both configs, foreground, `/t:Rebuild` on `ReferenceProject.slnx` for each** (Debug first): `ReferenceProject.slnx` Debug `/t:Rebuild` → `Arcane.slnx` Debug → Debug **unfiltered** (all pass, incl. `[witness][gpu]`) → Debug `~[gpu]` with `-r json -o D:\dev\starworks\Arcane\bin\t5-debug-nogpu.json` → `scripts/check-baselines.ps1 -ReportPath bin\t5-debug-nogpu.json -Configuration Debug -Invocation "~[gpu]"` → a RISE over 56399/1655, exit 0. Then `ReferenceProject.slnx` Release `/t:Rebuild` → `Arcane.slnx` Release → Release `~[gpu]` (`-r json` too) → the guard for Release → same figures as Debug. Then `powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Release` → **4/4 lanes `diffCount=0`, exit 0** (the gate rebuilds ReferenceProject Release itself). If an editor-ui lane goes red: read the diff PNG FIRST — this plan draws nothing without a selection, so a red lane here is a real finding, not a re-bless.
  Derive the counts: expected **+10 cases** (T1 +2, T2 +2, T3 +5, T4 +1), all inside `~[gpu]`; attribute assertions per case from the JSON (`test-run.test-cases[].test-info.name` + `totals.assertions`).
- [ ] **Step 3: Book the rise** into `scripts/automation-baselines.json` — Debug + Release rows to the measured figures; a dated `///` paragraph in `note` in the file's voice attributing by task and case name, with the per-case sums reproducing each task's delta (and any residual NAMED, as Plan 1's entry did); the `measured` sentence rewritten with the previous one pushed into "Prior measurements:"; Dist untouched and said so. Re-run the guard: `+0/+0`, exit 0, both configs. Commit — `chore(tests): baselines -- 2D physics wiring plan 2 (56399 -> <n> attributed)`.
- [ ] **Step 4: Closeout notes** — append `## Closeout (<date>)` to THIS plan doc in the shape of Plan 1's: HEAD range; state handed off (`FieldKind::Vector` in the Inspector; fixture lists authored in the editor; rulings A1–A8 as shipped; the α = 0.25 pin; no ABI change, Gacha untouched); the final counts with seeds and the attribution table; the guard and golden-gate outcomes; every controller `Ruling:` transcribed from the SDD ledger; **follow-ups recorded, not actioned:** scalar-element vectors in both the bridge and the editor (A1), nested vectors (A1), Astra `vectorSwap` for non-trivial elements (A3), multi-selection list editing (A2), `SetGravity` upstream in Manifold2D (carried from Plan 1). One-line touches to the spec: the Status line → "Plans 1–2 closed"; §7.3 gains "*Shipped with rulings A1–A3 of Plan 2 — reflected-struct elements only, single-selection editing, bytewise reorder.*" Commit — `docs: 2D physics wiring plan 2 closeout notes`.

---

## Self-review record (run at authoring time)

**Spec coverage — every clause of §7.3 and the two §8 rows to a task:**

| Spec | Task |
|---|---|
| §7.3 `ClassifyField` returns `Vector` for a drawable element type, else `ReadOnly` | T1 (element set narrowed by ruling A1 — reflected structs whose fields all classify; the reason is the editors' `FieldInfo` dependency, the same line the bridge drew) |
| §7.3 header row — name, count, **+** (`vectorInsert` at end) | T2 |
| §7.3 per element — indented collapsible block recursing the element's fields through the existing editors | T3 (`ElementHeaderRow` + the recursive `Visit` under `elementCtx`) |
| §7.3 **−** (`vectorErase`), up / down (swap through `vectorElement`) | T3 (`ApplyVectorErase`, `ApplyVectorSwap` from T1; bytewise per A3) |
| §7.3 every mutation through `ComponentEditCommand`, whole-component snapshot, `Modified` mark | T2/T3 — list ops via `ApplyImmediate` (ScopedTransaction + Snapshot + Modified), in-element edits via the existing activation gesture; the drive asserts one step per op and undo restoring the list |
| §7.3 "in-element scalar drags use the same transaction bracket scalar fields use today" | T3 (`BeginGestureIfActivated`/`EndGesture` untouched; only the label path and the fan-out target change) |
| §8 row "§7.3": device-less ImGui drive, add / remove / reorder / in-element edit each one undoable command, undo restores | T2 (add), T3 (remove, reorder, in-element drag, pure click pushes nothing) — via the real `DrawReflectedComponent` (A5/A6) |
| §8 row "Plan 2's owed case": real PASS 2.5 output through `RenderSubmissionSystem` at α = 0.25 pins the blend direction | T4 |
| §9 build ritual, baseline, no ABI change | Global Constraints; T5 |

**Placeholder scan:** no "TBD"/"later"/"similar to Task N" — every step carries its code; Task 3 restates the arm's insertion point rather than referring back; the LNK2019 contingency in Task 2 names the exact recovery rather than "fix link errors".

**Type consistency:** `FieldKind::Vector` (T1) is the case label in T2's switch. `VectorSize/ApplyVectorInsert/ApplyVectorErase/ApplyVectorSwap(const FieldInfo&, void*, size_t[, size_t])` (T1) are what `ApplyListOp` (T2) and the arm (T2/T3) call, with `ApplyVectorSwap` non-`noexcept` in both places. `PendingListOp{kind, a, b}` (T2) is what `ElementHeaderRow` (T3) fills and `ApplyListOp` consumes. `ElementContext{vectorField, index, prefix}` (T3) is read by `ForEachTarget`, `LabelPath`, the prologue skips and the tail probe — all in T3. `InspectorState::vectorProbe` (T2) ↔ `visitor.probe` ↔ `RecordProbe` (T2) ↔ `VectorHarness::probe` keys `"fixtures.add"` (T2), `"fixtures[i].remove|up|down"`, `"fixtures[i].<field>"` (T3). `ReflectedComponentArgs` is brace-initialised in its declared order (registry, component, primary, selection, undo, project, services, state, componentDisplayName, activeCategory, filterQuery). `RenderContext2D{batcher, cameraOffset, zoom, alpha}` (T4) matches `SceneResources.hpp:92-102`.

**Contradictions found in the code, resolved:**
1. Spec §7.3 lists arithmetic / glm / enum elements as drawable → every editor keys off a `FieldInfo`; refused (A1), mirroring the bridge.
2. Spec §7.3 "swap through `vectorElement`" assumes a copyable element; Astra has no `vectorSwap` → bytewise with the trivially-copyable roster policy stated (A3).
3. The spec's "device-less ImGui drive (`AssetsGraphCanvasTest` pattern)" assumed the view TU is in the test exe; it is not → premake entry (A6), with the link closure checked at plan time and a named recovery if it was checked wrong.
4. Multi-selection was not addressed by the spec; the fan-out's component-offset reads make element editing wrong under it → count-only (A2).

**Known intentional gaps, each with its owner:** scalar-element and nested vectors (follow-up, no consumer); `vectorSwap` in Astra (follow-up, on the first non-trivial element); multi-selection list editing (follow-up, on demand); `SetGravity` upstream (Manifold2D, carried); an element-kind preview in the tree-node label (polish, not owed).
