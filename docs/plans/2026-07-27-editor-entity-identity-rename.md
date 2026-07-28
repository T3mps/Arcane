# Editor Entity Identity + Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `EntityInfo` visible in the Inspector with `name` editable and `id` view-only, add generic string editing for every component, and retire the rename jank — one rename mechanism (a `ComponentEditCommand` on `EntityInfo`) shared by the Outliner and the Inspector.

**Architecture:** `Edit::RenameEntity` stops minting identity and becomes a pure edit of an existing `EntityInfo`. `FieldKind::String` + `ApplyStringEdit` extend the tested pure TU (`InspectorFields`); the panel gains one string row pattern (per-frame local, commit-on-deactivate, equality guard). `IsSystemManagedComponent` splits into `IsHiddenInInspector` (derived caches) and `IsStructureLocked` (those + `EntityInfo`), which is what makes `EntityInfo` visible without becoming addable/removable.

**Tech Stack:** C++23, Astra reflection, Dear ImGui (docking), Catch2, premake5.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-27-editor-entity-identity-rename-design.md`. Decisions there are locked.
- **Branch:** `arcane-entity-rename` (already checked out), cut from `arcane-inspector-polish` @ `182722b0`. Entry gate baseline: **30164 assertions / 577 cases** under `~[gpu]`, seeds 6 AND 17. Counts grow as tasks add tests; every task ends with a green full gate.
- **Build with the VS 18 toolchain explicitly.** Plain `msbuild` on PATH is VS 2022 and fails MSB8020:
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal`
- **Gate runs from the exe directory:** `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests` then `.\ArcaneTests.exe "~[gpu]" --rng-seed 6` and `--rng-seed 17`. Random order under a time-based seed — both seeds required.
- **`EditorPanels.cpp` is NOT compiled by the test gate.** After every panel change, confirm `ArcaneEditor.exe` is newer than every file under `Arcane/ArcaneEditor/src` and report the timestamps. That is the only compile evidence.
- **Never write a bare `Arcane::Runtime rt;` in a test** — it steals Arcane.dll's TypeContext slot and reflection ops silently report zero changes. Build registries via `std::make_shared<Astra::ComponentRegistry>()` + `Arcane::RegisterSceneComponents`, or reuse each test file's existing fixture.
- **Nothing in vendored Astra, ImGui, or the UE tree may be modified.** Read freely; never edit.
- Exception-free. UTF-8 without BOM, ASCII-only comments and strings. **Comments explain WHY and must never claim more than the code delivers.** When citing vendored behaviour, cite the IMPLEMENTATION line, not a header doc comment — doc-comment cites contradicted by the implementation are this project's recurring failure class.
- **Naming collisions:** `Astra::Hidden` (field attribute) vs `Arcane::Hidden` (marker component); `Astra::ReadOnly` (field attribute) vs `InspectorFields::FieldKind::ReadOnly` (classification). Namespace-qualify at every mention.
- **Inspector invariants that must not regress:** per-component `PushID` scoping (nothing between a push and its pop may `continue`), deferred `pendingRemove`, component-type intersection, mixed-value blanking, the `label` (display) / `rawName` (identifier) split (`rawName` feeds the three transaction-description builders and `AssetKindFilterForFieldName`), and `CommandStack` token ownership (`Begin` returns the owner token; never close a transaction the panel does not own).
- **Verified-this-session vendored facts the plan builds on** (implementers re-verify at the step that uses them): ImGui reloads a passed buffer while active ONLY when `WantReloadUserBuf` is set (`imgui_widgets.cpp:4834-4849`); Escape restores `TextToRevertTo` and sets `value_changed = true` (`imgui_widgets.cpp:5301-5310`), so `IsItemDeactivatedAfterEdit` may fire after a revert — the equality guard IS the cancel mechanism; `CommandStack::Commit` drops unchanged components and pushes nothing when all are unchanged (`CommandStack.cpp:47-50`, `:61-62`; pinned by `"CommandStack: Commit is the safe close for an ABANDONED gesture"` and `"CommandStack: empty / no-op transaction is not pushed"` in `CommandStackTest.cpp`); `misc/cpp/imgui_stdlib` is NOT vendored.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp` / `.cpp` (modify) | `RenameEntity`'s new contract: requires `EntityInfo`, never mints identity; stated uniqueness policy. |
| `Arcane/Tests/src/EntityOpsTest.cpp` (modify) | The inverted rename-contract test. |
| `Arcane/Tests/src/CommandStackTest.cpp` (modify) | Rename as one `ComponentEditCommand` undo step; no-op rename pushes nothing. |
| `Arcane/ArcaneEditor/src/InspectorFields.hpp` / `.cpp` (modify) | `FieldKind::String`, `ApplyStringEdit`, `ComputeFieldMixed` string support. Pure, gate-covered. |
| `Arcane/Tests/src/EditorInspectorTest.cpp` (modify) | Units for the above. |
| `Arcane/ArcaneEditor/src/EditorPanels.hpp` (modify) | `OutlinerState::renameBuf` becomes `std::string`. |
| `Arcane/ArcaneEditor/src/EditorPanels.cpp` (modify) | `InputTextString` helper; String row in the visitor; Outliner rename rework; display-gate predicate swap. |
| `Arcane/ArcaneEditor/src/ComponentCatalog.hpp` / `.cpp` (modify) | `IsSystemManagedComponent` -> `IsHiddenInInspector` + `IsStructureLocked`. |

---

### Task 1: RenameEntity's new contract

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp` (`RenameEntity`, lines ~176-190), `Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp` (`RenameEntity` doc, ~line 81)
- Test: `Arcane/Tests/src/EntityOpsTest.cpp` (REWRITE the test at ~line 172)

**Interfaces:**
- Produces: `bool Edit::RenameEntity(Astra::Registry&, Astra::Entity, std::string)` — false on invalid entity, false (and NO component added) when `EntityInfo` is missing, false on unchanged name, true + rename otherwise. Signature unchanged; contract inverted for the missing case.

**This task deliberately inverts an existing test.** `EntityOpsTest.cpp:172` — `TEST_CASE("RenameEntity adds EntityInfo when missing", "[outliner]")` — asserts the OLD contract (add + mint `Guid`). Rewrite it; do not append alongside it.

- [ ] **Step 1: Rewrite the failing test**

Replace the whole `"RenameEntity adds EntityInfo when missing"` test case with (adapt entity-creation calls to the file's `World` fixture idioms — read neighbouring cases first):

```cpp
TEST_CASE("RenameEntity requires EntityInfo -- rename never mints identity", "[outliner]")
{
    World w;

    // A raw registry entity is the runtime-spawn shape: no EntityInfo. The
    // editor disables rename for these; the op must refuse, not repair.
    Astra::Entity raw = w.reg.CreateEntity();
    CHECK_FALSE(Edit::RenameEntity(w.reg, raw, "Boss Arena"));
    CHECK(w.reg.GetComponent<EntityInfo>(raw) == nullptr);   // nothing added

    // An authored entity renames normally, and its Guid is untouched --
    // identity is creation-time only (UE: ActorLabel/ActorGuid are intrinsic
    // AActor fields, Actor.h:1055/:1188; there is no "add identity" edit).
    Astra::Entity authored = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    EntityInfo* info = w.reg.GetComponent<EntityInfo>(authored);
    REQUIRE(info != nullptr);
    const Guid before = info->id;
    CHECK(Edit::RenameEntity(w.reg, authored, "Boss Arena"));
    CHECK(w.reg.GetComponent<EntityInfo>(authored)->name == "Boss Arena");
    CHECK(w.reg.GetComponent<EntityInfo>(authored)->id == before);
}
```

Keep the existing no-op test at ~line 271 (`Rename... unchanged -> false`) untouched — it still holds.

- [ ] **Step 2: Run to verify it fails**

```
cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[outliner]" --rng-seed 6
```
(after a build) Expected: FAIL — `CHECK_FALSE(Edit::RenameEntity(...))` fails because the current implementation adds `EntityInfo` and returns true.

- [ ] **Step 3: Implement**

Replace `RenameEntity`'s body (`EntityOps.cpp:176-190`):

```cpp
    bool RenameEntity(Astra::Registry& reg, Astra::Entity e, std::string name)
    {
        if (!reg.IsValid(e))
            return false;
        EntityInfo* info = reg.GetComponent<EntityInfo>(e);
        // Identity is never minted by a rename. UE's equivalents (ActorLabel,
        // ActorGuid) are intrinsic AActor fields (Actor.h:1055/:1188) -- there
        // is no "add identity" edit to mirror. Entities without EntityInfo are
        // runtime spawns; the editor disables rename for them.
        if (!info)
            return false;
        if (info->name == name)
            return false;   // no-op rename: no change, no undo step
        info->name = std::move(name);
        return true;
    }
```

Update the `RenameEntity` doc comment in `EntityOps.hpp` to state the contract AND the uniqueness policy:

```cpp
    // Rename an existing EntityInfo. Returns false (mutating nothing) when the
    // entity is invalid, lacks EntityInfo, or the name is unchanged -- so a
    // false return always means "no undo step needed". Never adds the
    // component: identity is minted at creation only.
    //
    // Names are deliberately NOT unique. Uniqueness is a creation-time policy
    // (AutoEntityName); duplicates are legal on rename -- UE's split exactly
    // (SetActorLabelUnique on spawn/paste/duplicate, EditorEngine.cpp:6154;
    // plain SetActorLabel on rename, EditorEngine.cpp:6193-6205).
```

- [ ] **Step 4: Build, run to verify it passes**

Build (VS 18 command from Global Constraints), then `[outliner]` under seeds 6 and 17. Expected: PASS. Then the full `~[gpu]` gate under seed 6 — expected: all green (the only intentional behaviour change is the rewritten case).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Edit/EntityOps.cpp Arcane/Arcane/src/Arcane/Edit/EntityOps.hpp Arcane/Tests/src/EntityOpsTest.cpp
git commit -m "feat(arcane): rename requires EntityInfo -- identity is creation-time only"
```

---

### Task 2: Rename is one ComponentEditCommand undo step

**Files:**
- Test: `Arcane/Tests/src/CommandStackTest.cpp` (append)

No production code. This pins the mechanism Tasks 4 and 6 build on: a rename bracketed by `Begin`/`SnapshotComponent`/`Commit` on the `EntityInfo` descriptor undoes and redoes correctly (the snapshot is a SERIALIZED blob via `descriptor->serialize`, `ComponentEditCommand.cpp:23-38`, so `std::string` round-trips), and a no-op rename pushes no history entry.

**Interfaces:**
- Consumes: Task 1's `Edit::RenameEntity`; the file's existing `MakeReg()` + `DescriptorFor(reg, e, typeName)` helpers (`CommandStackTest.cpp:41-48`).

- [ ] **Step 1: Write the failing test**

Append (mirror the file's existing `CommandStack` construction and undo/redo call idioms — read the neighbouring cases first; if `MakeReg()` does not register `EntityInfo`, mirror `EditorInspectorMetaTest.cpp`'s `RegisterSceneComponents` setup instead):

```cpp
TEST_CASE("a rename is one ComponentEditCommand undo step", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    // SSO-defeating on purpose: a heap-owning string is the case a raw byte
    // snapshot would corrupt; the serialized-blob path must round-trip it.
    const std::string longName = "A name long enough to defeat SSO ................";
    reg->AddComponent<Arcane::EntityInfo>(e, Arcane::EntityInfo{ Arcane::Guid::Generate(), longName });

    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::EntityInfo");
    REQUIRE(desc != nullptr);

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    SECTION("undo restores the exact prior name; redo reapplies")
    {
        const Arcane::TransactionId id = stack.Begin("Rename");
        stack.SnapshotComponent(e, desc);
        REQUIRE(Arcane::Edit::RenameEntity(*reg, e, "Short"));
        stack.Commit(id);

        stack.Undo();
        CHECK(reg->GetComponent<Arcane::EntityInfo>(e)->name == longName);
        stack.Redo();
        CHECK(reg->GetComponent<Arcane::EntityInfo>(e)->name == "Short");
    }

    SECTION("a no-op rename pushes no history entry")
    {
        const Arcane::TransactionId id = stack.Begin("Rename");
        stack.SnapshotComponent(e, desc);
        CHECK_FALSE(Arcane::Edit::RenameEntity(*reg, e, longName));   // unchanged
        stack.Commit(id);
        // Commit re-snapshots and drops unchanged components
        // (CommandStack.cpp:47-50), then pushes nothing (:61-62).
        CHECK_FALSE(stack.CanUndo());
    }
}
```

- [ ] **Step 2: Run to verify it fails or passes honestly**

Build, then `.\ArcaneTests.exe "[edit]" --rng-seed 6`. This test SHOULD pass immediately (it pins existing machinery + Task 1). If it fails, the mechanism assumption is wrong — STOP and report rather than adapting the test to the failure.

- [ ] **Step 3: Run both seeds, commit**

`[edit]` under seeds 6 and 17, then the full `~[gpu]` gate under seed 6.

```bash
git add Arcane/Tests/src/CommandStackTest.cpp
git commit -m "test(arcane): pin rename as one ComponentEditCommand undo step"
```

---

### Task 3: FieldKind::String in the pure TU

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorFields.hpp`, `Arcane/ArcaneEditor/src/InspectorFields.cpp`
- Test: `Arcane/Tests/src/EditorInspectorTest.cpp` (append)

**Interfaces:**
- Produces:
  - `FieldKind::String` (new enumerator, inserted before `ReadOnly`: `enum class FieldKind { Bool, Int32, Float, Vec2, Vec3, AssetRef, String, ReadOnly };`)
  - `void ApplyStringEdit(const Astra::FieldInfo& f, void* instance, const std::string& v) noexcept`
  - `ComputeFieldMixed` handles `String` (one bit; exact `==` comparison)
  - `FieldComponentCount(String) == 1` (falls into the existing `default:` — no change needed, but pinned by a test)

- [ ] **Step 1: Write the failing test**

Append to `EditorInspectorTest.cpp` (reuse the file's existing registry fixture idioms; `Arcane::EntityInfo` is the natural probe component — its `name` is a reflected `std::string`. Never construct a bare `Arcane::Runtime`):

```cpp
TEST_CASE("std::string fields classify as String and round-trip edits", "[editor]")
{
    auto creg = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*creg);
    const Astra::ComponentDescriptor* desc =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::EntityInfo>::Hash());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->meta != nullptr);

    const Astra::FieldInfo* nameField = nullptr;
    for (const Astra::FieldInfo& f : desc->meta->fields)
        if (f.name == "name") nameField = &f;
    REQUIRE(nameField != nullptr);

    CHECK(Arcane::Editor::ClassifyField(*nameField) == Arcane::Editor::FieldKind::String);
    CHECK(Arcane::Editor::FieldComponentCount(Arcane::Editor::FieldKind::String) == 1);

    Arcane::EntityInfo info;
    // SSO-defeating: the write-back must survive a heap-owning assignment.
    Arcane::Editor::ApplyStringEdit(*nameField, &info,
        "A name long enough to defeat SSO ................");
    CHECK(info.name == "A name long enough to defeat SSO ................");
    Arcane::Editor::ApplyStringEdit(*nameField, &info, "");
    CHECK(info.name.empty());
}

TEST_CASE("ComputeFieldMixed sees string disagreement", "[editor]")
{
    // Mirror this file's existing registry fixture for a live Astra::Registry
    // with scene components registered; create three entities: two with
    // EntityInfo (same name), one with a different name.
    // Assert per the existing ComputeFieldMixed test idioms:
    //   same-name pair              -> !mask.Any()
    //   pair plus differing third   -> mask.Test(0)
    //   entity lacking EntityInfo   -> not a voter (mask unchanged)
}
```

The second case's body follows the file's existing `ComputeFieldMixed` tests — write real assertions in their idiom (three entities, `std::span` selection, the `"name"` field), not the comment sketch above.

- [ ] **Step 2: Run to verify it fails**

Build. Expected: FAIL to compile — `FieldKind::String` and `ApplyStringEdit` do not exist.

- [ ] **Step 3: Implement**

`InspectorFields.hpp`: add `String` to the enum before `ReadOnly`; declare after `ApplyGuidEdit`:

```cpp
    void ApplyStringEdit(const Astra::FieldInfo& f, void* instance, const std::string& v) noexcept;
```

Add `#include <string>` to the header's includes.

`InspectorFields.cpp` — in `ClassifyField`, alongside the existing hash statics:

```cpp
        static const uint64_t kStr  = Astra::TypeID<std::string>::Hash();
```
and with the other comparisons:
```cpp
        if (f.typeHash == kStr)  return FieldKind::String;
```

After `ApplyGuidEdit`:

```cpp
    void ApplyStringEdit(const Astra::FieldInfo& f, void* instance, const std::string& v) noexcept
    { if (std::string* p = f.GetPtr<std::string>(instance)) *p = v; }
```

In `ComputeFieldMixed`: add `std::string seedS;` beside the other seeds and `std::string curS;` beside the other locals; a read case:

```cpp
                case FieldKind::String:
                    if (const std::string* p = f.GetPtr<std::string>(data)) curS = *p;
                    break;
```

and a compare case beside `AssetRef`:

```cpp
                case FieldKind::String:
                    if (curS != seedS) mask.bits |= 1u;
                    break;
```

and seed `seedS = curS;` in the seeding block beside the others. Add `#include <string>` if not already present.

- [ ] **Step 4: Run to verify it passes**

Build; `[editor]` under seeds 6 and 17; then the full `~[gpu]` gate under seed 6. Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/InspectorFields.hpp Arcane/ArcaneEditor/src/InspectorFields.cpp Arcane/Tests/src/EditorInspectorTest.cpp
git commit -m "feat(editor): classify and edit std::string fields (FieldKind::String)"
```

---

### Task 4: The string row in the panel

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (the `ImGuiFieldVisitor::Visit` switch; a new helper in the file's anonymous namespace)

**Interfaces:**
- Consumes: Task 3's `FieldKind::String` / `ApplyStringEdit`; the visitor's existing `label`, `rawName`, `Multi()`, `MixedFor(f)`, `ApplyImmediate(field, instance, fn)` members.
- Produces: `bool InputTextString(const char* label, std::string* s, ImGuiInputTextFlags flags = 0)` in the anonymous namespace — Task 6 reuses it.

**No unit test — by design.** This is ImGui; the decisions it consumes are tested by Task 3. Verification is the build, an unchanged gate, the exe timestamp, and desk-verify.

- [ ] **Step 1: Add the InputTextString helper**

In `EditorPanels.cpp`'s anonymous namespace (near the other small helpers). `misc/cpp/imgui_stdlib` is NOT vendored (verified), so this inlines its resize-callback pattern:

```cpp
        // std::string-backed InputText. imgui_stdlib is not vendored, so this
        // inlines its CallbackResize pattern: ImGui asks for more room via the
        // callback and the string reallocates in place. While the widget is
        // ACTIVE, ImGui edits its own internal buffer and ignores the one
        // passed in (imgui_widgets.cpp:4834-4849 -- a reload happens only when
        // WantReloadUserBuf is set), which is what makes a per-frame seeded
        // local safe at every call site.
        int StringResizeCallback(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                std::string* s = static_cast<std::string*>(data->UserData);
                s->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = s->data();
            }
            return 0;
        }

        bool InputTextString(const char* label, std::string* s, ImGuiInputTextFlags flags = 0)
        {
            flags |= ImGuiInputTextFlags_CallbackResize;
            return ImGui::InputText(label, s->data(), s->capacity() + 1, flags,
                                    StringResizeCallback, s);
        }
```

Verify the exact `ImGuiInputTextCallbackData` member spellings (`EventFlag`, `BufTextLen`, `Buf`, `UserData`) against the vendored `imgui.h` before writing.

- [ ] **Step 2: Add the String arm to the visitor switch**

In `ImGuiFieldVisitor::Visit`, add a `case Arcane::Editor::FieldKind::String:` arm before the `ReadOnly`/`default` arm. Read the neighbouring arms first and match their member spellings exactly (`label`, `rawName`, `instance` parameter name, `Multi()`, `MixedFor`, `ApplyImmediate`):

```cpp
                case Arcane::Editor::FieldKind::String:
                {
                    const std::string* live = f.GetPtr<std::string>(instance);
                    const bool mixed = Multi() && MixedFor(f).Any();
                    // Per-frame local, reseeded from the component while idle;
                    // ImGui's internal state owns the text while active (see
                    // InputTextString). Blank-when-mixed is the panel's
                    // existing multi-select convention.
                    std::string text = (mixed || !live) ? std::string() : *live;
                    const std::string seed = text;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    InputTextString(label.c_str(), &text);
                    // Commit-on-deactivate with an equality guard. The guard IS
                    // the cancel path: Escape restores the activation-time text
                    // AND sets value_changed (imgui_widgets.cpp:5301-5310), so
                    // IsItemDeactivatedAfterEdit would misreport a revert as an
                    // edit -- deactivation plus "did it end up different" is
                    // the honest predicate.
                    if (ImGui::IsItemDeactivated() && text != seed)
                        ApplyImmediate(rawName, instance, [&](void* d)
                            { Arcane::Editor::ApplyStringEdit(f, d, text); });
                    break;
                }
```

Notes binding this step:
- `ApplyImmediate` fans out to every selected entity and brackets one `ScopedTransaction` — a multi-select string commit is one undo step, matching every other field kind. The visitor's null-stack Play behaviour is inherited unchanged.
- The label goes through `label` (display name) and the transaction description through `rawName` — do not swap them.
- The widget label: if the neighbouring arms render the label separately (e.g. via a two-column layout or `TextUnformatted` + `SameLine`), match THAT shape instead of the inline `label.c_str()` form — the arm above shows intent, the file shows the house pattern.

- [ ] **Step 3: Build, gate, exe timestamp**

Build the whole solution. Full `~[gpu]` gate under seed 6: expected unchanged-green (this task adds no assertions). Confirm `ArcaneEditor.exe` is newer than every file under `Arcane/ArcaneEditor/src`; record both timestamps.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): editable string fields in the Inspector"
```

---

### Task 5: The visibility split — EntityInfo becomes visible

**Files:**
- Modify: `Arcane/ArcaneEditor/src/ComponentCatalog.hpp` (~lines 27-51), `Arcane/ArcaneEditor/src/ComponentCatalog.cpp` (~lines 39-45, ~line 67), `Arcane/ArcaneEditor/src/EditorPanels.cpp` (~line 1695 and the Remove Component gate)

**Interfaces:**
- Produces: `bool IsHiddenInInspector(std::string_view typeName)` and `bool IsStructureLocked(std::string_view typeName)`, replacing `IsSystemManagedComponent` (which is DELETED — no alias).

- [ ] **Step 1: Split the predicate**

In `ComponentCatalog.cpp`, replace `IsSystemManagedComponent` (lines 39-45) with:

```cpp
    bool IsHiddenInInspector(std::string_view typeName)
    {
        // Derived per-frame caches: showing them is noise and editing them is
        // overwritten by the next propagation pass.
        return typeName == "Arcane::WorldTransform"
            || typeName == "Arcane::PreviousTransform"
            || typeName == "Arcane::PhysicsBodyRef";
    }

    bool IsStructureLocked(std::string_view typeName)
    {
        // The hidden caches, plus identity. EntityInfo is VISIBLE (name edits,
        // id view-only) but never user-added or user-removed -- the ECS
        // equivalent of AActor's intrinsic ActorLabel/ActorGuid
        // (Actor.h:1055/:1188), which are not components at all.
        return IsHiddenInInspector(typeName) || typeName == "Arcane::EntityInfo";
    }
```

Update `ComponentCatalog.hpp`: replace the `IsSystemManagedComponent` declaration with both new declarations, and rewrite the per-entry WHY comment block (lines ~27-51) so the `EntityInfo` entry says visible-but-structure-locked with the AActor citation, and the block explains the two predicates and which call sites consume each.

- [ ] **Step 2: Reassign the three call sites**

Find every `IsSystemManagedComponent` caller (grep; the old comment at `EditorPanels.cpp:1692-1694` names all three):
- the Inspector **display gate** (`EditorPanels.cpp:1695`) -> `IsHiddenInInspector`
- the **Add Component catalog** filter (`ComponentCatalog.cpp:67`) -> `IsStructureLocked`
- the **Remove Component** affordance (the component header's context menu in `DrawInspectorPanel`) -> `IsStructureLocked` — this one now matters: `EntityInfo` renders a header for the first time, and its context menu must not offer removal.

Update the `EditorPanels.cpp:1692-1694` comment: the three sites no longer share one predicate, and the comment must say which uses which and why — it must not claim a parity that no longer exists.

- [ ] **Step 3: Build, gate, exe timestamp**

Build; full `~[gpu]` gate under seed 6 (expected unchanged-green); exe-timestamp check; record timestamps.

Desk-visible result (note in the report for the human): every entity now shows an `Entity Info` section — `Name` editable (Task 4's row), `Id` disabled (its `Astra::ReadOnly` annotation + the existing disable path), no Remove Component on its header, and no `Entity Info` entry in Add Component.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/ComponentCatalog.hpp Arcane/ArcaneEditor/src/ComponentCatalog.cpp Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): show EntityInfo -- visible identity, structure-locked"
```

---

### Task 6: Outliner rename on the same mechanism

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.hpp` (`OutlinerState::renameBuf`, ~line 117), `Arcane/ArcaneEditor/src/EditorPanels.cpp` (`BeginRename` ~line 419, the inline-rename block ~lines 627-648, the F2 site ~line 562, the double-click site ~line 709, the menu site ~line 742)

**Interfaces:**
- Consumes: Task 1's `RenameEntity` contract; Task 4's `InputTextString`; `Arcane::ScopedTransaction`.
- **CORRECTED 2026-07-27 (whole-branch review, finding I1).** This line originally said `ScopedTransaction` "joins an open transaction safely — its `None`-token dtor no-ops". The no-op dtor is real, but "safely" was wrong: a joined scope's snapshots ride the OWNER's close, and `CommandStack::Cancel` discards pending snapshots WITHOUT reverting (`CommandStack.cpp:75-82`), so a rename that joined a gesture ending in `Cancel` applied and became permanently un-undoable. The commit site now calls `Edit::RenameWithUndo`, which opens its OWN transaction or returns `RenameResult::Deferred` having mutated nothing; the panel parks the request in `OutlinerState::pendingRename` and retries next frame. See the design doc's Section 2 for the full mechanism. Same finding, same date: the "Produces" bullet below is also stale -- no panel-local `EntityInfoDescriptor` helper shipped either. `RenameWithUndo` owns that descriptor lookup itself (`EntityOps.cpp`'s `FindEntityInfoDescriptor`), so it lives in one engine-side, headless-testable place instead of being re-derived per panel; the Step 3 code block below (which prescribes that helper) never shipped as written either.
- ~~Produces: a file-local `const Astra::ComponentDescriptor* EntityInfoDescriptor(Astra::Registry&, Astra::Entity)` helper.~~ Retracted -- see the CORRECTED note above.

- [ ] **Step 1: `renameBuf` becomes `std::string`; `BeginRename` seeds from the component**

`EditorPanels.hpp`: replace `char renameBuf[256] = {};` with `std::string renameBuf;` (kills silent truncation).

`BeginRename` (~line 419) already takes `const std::string&` — its body now assigns instead of copying into a fixed array. Its call sites change meaning: **seed from `EntityInfo::name`, not `DisplayName`** — `DisplayName` substitutes "Entity <id>" for an empty name, so renaming an empty-named entity used to seed fallback text that a no-edit commit would then write into the component. Seeding the raw (possibly empty) name makes Escape and no-edit commits true no-ops.

- [ ] **Step 2: Gate the three entry points on EntityInfo**

At each of the three `BeginRename` call sites (F2 ~562, double-click ~709, context menu ~742):

```cpp
// Rename edits an existing EntityInfo (Edit::RenameEntity refuses otherwise).
// Entities without one are runtime spawns -- no durable identity to rename.
const Arcane::EntityInfo* info = registry.GetComponent<Arcane::EntityInfo>(entity);
```

F2 and double-click: only call `BeginRename(state, entity, info->name)` when `info` is non-null. Context menu:

```cpp
if (ImGui::MenuItem("Rename", "F2", false, info != nullptr))
    BeginRename(state, row.entity, info->name);
if (info == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    ImGui::SetTooltip("Runtime entity: no EntityInfo identity to rename");
```

(`ForTooltip` resolves to `Stationary|DelayShort|AllowWhenDisabled`, so the tooltip works on the disabled item — the same flag choice the Inspector settled on.)

- [ ] **Step 3: Rewrite the inline-rename commit**

**Retracted 2026-07-27** (see the CORRECTED note under this task's Interfaces, above): the panel-local helper below never shipped. Kept for history; the shipped descriptor lookup is engine-side, in `RenameWithUndo`'s `FindEntityInfoDescriptor` (`EntityOps.cpp`).

Add the descriptor helper to the anonymous namespace (the `InspectEntity`-loop pattern proven in `CommandStackTest.cpp:41-48`; `Registry` exposes no public descriptor-by-hash accessor — verified):

```cpp
        const Astra::ComponentDescriptor* EntityInfoDescriptor(Astra::Registry& reg,
                                                               Astra::Entity e)
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
                if (ci.descriptor
                    && ci.descriptor->hash == Astra::TypeID<Arcane::EntityInfo>::Hash())
                    return ci.descriptor;
            return nullptr;
        }
```

Replace the inline-rename block (~lines 627-648). The old block's `EnterReturnsTrue` + `commit || !IsKeyPressed(Escape)` predicate dies:

```cpp
                if (state.renameTarget == row.entity)
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (state.renameFocusPending)
                    {
                        ImGui::SetKeyboardFocusHere();
                        state.renameFocusPending = false;
                    }
                    InputTextString("##rename", &state.renameBuf,
                                    ImGuiInputTextFlags_AutoSelectAll);
                    // Commit-on-deactivate; the equality guard IS the cancel
                    // path. Escape restores the activation-time text before
                    // deactivating (imgui_widgets.cpp:5301-5310), so a
                    // cancelled rename arrives here equal to its seed and
                    // RenameEntity's own unchanged-name refusal makes it a
                    // no-op. Enter and click-away deactivate too, so no
                    // EnterReturnsTrue and no global key queries.
                    if (ImGui::IsItemDeactivated())
                    {
                        const Astra::Entity e = row.entity;
                        const Arcane::EntityInfo* info =
                            registry.GetComponent<Arcane::EntityInfo>(e);
                        if (info && state.renameBuf != info->name)
                        {
                            if (binding.editMode)
                            {
                                // ORIGINAL DESIGN, not what shipped -- 2026-07-27
                                // whole-branch review (finding I1) found
                                // joining NOT safe: a Cancel on the owning
                                // gesture discards a joined snapshot WITHOUT
                                // reverting it (CommandStack.cpp:75-82). The
                                // shipped mechanism is Edit::RenameWithUndo +
                                // deferral; see the design doc's REVISED
                                // Section 2.
                                if (const auto* desc = EntityInfoDescriptor(registry, e))
                                {
                                    Arcane::ScopedTransaction txn(undo, "Rename");
                                    txn.Snapshot(e, desc);
                                    Arcane::Edit::RenameEntity(registry, e, state.renameBuf);
                                }
                            }
                            else
                            {
                                // Play mode: apply without undo bracketing --
                                // the same behaviour as Inspector field edits,
                                // whose visitor nulls its stack during Play.
                                Arcane::Edit::RenameEntity(registry, e, state.renameBuf);
                            }
                        }
                        state.renameTarget = Astra::Entity::Invalid();
                    }
                }
```

Verify `ScopedTransaction`'s constructor signature against `CommandStack.hpp` before writing (label-taking form). The `ApplyStructural(undo, binding, "Rename", ...)` call is deleted; `ApplyStructural` itself stays (create/delete/reparent/hide still use it). Note the play-mode change is deliberate and spec'd: rename during Play used to be silently refused; it now applies un-undoably like any field edit.

- [ ] **Step 4: Build, full gate BOTH seeds, exe timestamp**

Build; `.\ArcaneTests.exe "~[gpu]" --rng-seed 6` and `--rng-seed 17` — final acceptance for the arc, all green; exe-timestamp check; record timestamps and final counts.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorPanels.hpp Arcane/ArcaneEditor/src/EditorPanels.cpp
git commit -m "feat(editor): outliner rename as a component edit -- one mechanism, native cancel"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| Semantics: rename never mints; requires EntityInfo; uniqueness stated | 1 |
| Semantics: not-renameable entities disabled with tooltip | 6 |
| Mechanism: ComponentEditCommand; no-op pushes nothing; ApplyStructural("Rename") dies; play-mode parity | 2, 6 |
| Inspector: FieldKind::String, ApplyStringEdit, ComputeFieldMixed, FieldComponentCount | 3 |
| Inspector: commit-on-deactivate row, multi-select blank/fan-out, equality guard | 4 |
| Inspector: predicate split, EntityInfo visible, id view-only, no Category | 5 (id's ReadOnly annotation pre-exists on the base branch) |
| Outliner UX: std::string buffer, native Escape, seed from component, entry-point gating | 6 |
| Why the two entry points cannot drift | 2 + 6 (same command shape, pinned by 2) |
| Testing roster | 1, 2, 3 |
| Plan-time verification items | resolved in Global Constraints (ImGui reload/Escape facts, no imgui_stdlib, no public descriptor accessor, Commit-drops-unchanged citations); ComputeFieldMixed internals read and extended in 3 |

**Placeholders:** Task 3 Step 1's second test case delegates its body to the file's existing idioms with the required assertions named — deliberate (the fixture's shape is authoritative), not a gap. No TBDs.

**Type consistency:** `RenameEntity(Astra::Registry&, Astra::Entity, std::string)`, `ApplyStringEdit(const Astra::FieldInfo&, void*, const std::string&)`, `InputTextString(const char*, std::string*, ImGuiInputTextFlags)`, `IsHiddenInInspector`/`IsStructureLocked(std::string_view)`, `EntityInfoDescriptor(Astra::Registry&, Astra::Entity)` — used with the same spellings throughout.

**Three places the implementer must verify against the codebase rather than trust this plan:** the `Visit` switch's member spellings and label-rendering shape (Task 4); `ScopedTransaction`'s constructor and the `CommandStack` test-file idioms (Tasks 2, 6); the `World` fixture's entity-creation idioms in `EntityOpsTest.cpp` (Task 1).

## Desk-Verify (owed after Task 6 — nothing in the panel is gate-covered)

1. Inspector: select an entity — `Entity Info` section shows; edit `Name`, Enter commits, Ctrl+Z restores; Escape while editing reverts with NO undo entry; `Id` is dimmed and uneditable.
2. Outliner: F2 rename commits on Enter AND on click-away; Escape cancels with no undo entry; undo/redo round-trips; renaming to the same name adds no undo entry.
3. Multi-select two entities with different names: `Name` shows blank; typing a value renames BOTH in one undo step.
4. A Sandbox/Play-spawned entity (no EntityInfo): row menu's Rename is disabled with the tooltip; F2 does nothing; the Inspector shows no Entity Info section for it.
5. `Entity Info`'s header: no Remove Component; Add Component list contains no Entity Info entry.
6. During Play: rename applies immediately and is NOT in the undo stack after Stop.
7. Rename an entity while a gizmo drag is mid-flight (pathological): the drag must remain one undo entry and the rename must land (joins the open transaction).
