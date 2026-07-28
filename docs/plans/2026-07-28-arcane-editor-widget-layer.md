# Arcane::Editor Widget Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the editor's shared widget vocabulary and gesture-undo machinery out of
EditorPanels.cpp's anonymous namespace into three public module pairs (`EditorWidgets`,
`EditGesture`, `InspectorView`), then adopt them everywhere -- full adoption, no shims.

**Architecture:** Spec at `docs/superpowers/specs/2026-07-28-arcane-editor-widget-layer-design.md`
(READ IT FIRST -- it carries the verified starting-state inventory and the intentional
behavior changes). One transaction-converged gesture model over the existing
`Arcane::CommandStack` (NO engine changes: `Push` already joins open transactions). Pure
decision cores are headless-tested; ImGui skins stay thin and untested.

**Tech Stack:** C++23, Dear ImGui (dll-exported from Arcane.dll), Astra reflection,
Catch2 (`[editor]` tag), premake5 -> Arcane.slnx, MSVC v18 (VS 2026).

## Global Constraints

- **Sequencing:** this plan executes AFTER the shader-graph upgrade arc merges. Every
  `file:line` below was verified 2026-07-28 PRE-merge -- treat line numbers as hints and
  re-anchor by the quoted symbol/comment text. If a cited shape is gone entirely, STOP
  and re-read the spec's section 2 before improvising.
- **Branch:** create `arcane-editor-widget-layer` off the merged state before Task 1.
- **Build:** after any premake edit run `Arcane\GenerateProjects.bat`, then
  `msbuild Arcane.slnx /p:Configuration=Debug /m` using the VS 2026 (v18) MSBuild --
  NOT whatever is first on PATH (locate via vswhere if needed; prior arcs were bitten).
- **Gate:** `ArcaneTests.exe ~[gpu]` run FROM `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`.
  Capture the Catch2 seed banner (random order; reproduce failures with `--rng-seed N`).
  **A green gate does NOT prove the editor relinked** -- EditorPanels.cpp / EditorApp.cpp
  are not compiled into ArcaneTests; after editor-side edits verify ArcaneEditor.exe's
  timestamp moved.
- **Never** run `[gpu]` tests or launch the editor interactively from the harness
  (GPU-driver crash hazard under the virtual display); all interactive verification is
  the user's desk pass (Task 8 writes the checklist).
- Never a bare `Arcane::Runtime rt;` in tests (TypeContext theft).
- /MD runtime, ASCII-only comments, UTF-8 without BOM, no `/fp:fast`.
- **Comment-truth:** every comment a change contradicts gets updated in the same commit
  (SpriteDocument.hpp:38's "no compiler/undo/clock" is called out in Task 7; sweep for
  others as you touch each file).
- **Tier A is a lift:** pixel-identical behavior for every moved helper. Do not "improve"
  visuals while moving.
- Commit per task with the messages given; do not batch tasks into one commit.

## Amendments (2026-07-28 pre-execution review -- code fact-check + vendored-UE research)

Three corrections are folded into the tasks below in place; this index exists so no
task brief misses them:

1. **Teardown close** (Task 6 Step 1, Task 7 Step 4): every document that opens
   gestures adds `EditGesture::ClosePending` to its DESTRUCTOR. Documents are
   destroyed synchronously on close with no on-close hook; a stranded open
   transaction gates `InTransaction()` consumers editor-wide.
2. **imgui_internal.h** (Task 2 Step 2, Task 3 Step 1): `ImGui::GetActiveID` is
   internal-only (imgui_internal.h:3532), and so is `BeginFieldGrid`'s
   `table->LastResizedColumn` access -- both extracted .cpp files include
   `imgui_internal.h` alongside `imgui.h`, as EditorPanels.cpp does today (:23-24).
3. **Second `InTransaction()` consumer** (Task 5 Step 3): the Ctrl+Z/Ctrl+Y keybind
   gate (`EditorAppFrame.cpp:424-426`) reads it too, not just `CanEditStructure`.

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `Arcane/ArcaneEditor/src/EditGesture.hpp/.cpp` | NEW | Pure gesture decision core + ImGui bracket skin (`GestureState`, `BeginOnActivate`, `EndOnDeactivate`, `ClosePending`, `ScopeGuard`) |
| `Arcane/ArcaneEditor/src/EditorWidgets.hpp/.cpp` | NEW | Layout/widget vocabulary lifted from EditorPanels.cpp's anon namespace + new `StableTextEdit` |
| `Arcane/ArcaneEditor/src/InspectorView.hpp/.cpp` | NEW | `ImGuiFieldVisitor` + row machinery behind `DrawReflectedComponent` |
| `Arcane/ArcaneEditor/src/EditorPanels.cpp` | SHRINKS | Panels keep chrome; anon-namespace helper/visitor block moves out |
| `Arcane/ArcaneEditor/src/EditorPanels.hpp` | MODIFIED | `InspectorState` embeds `EditGesture::GestureState` |
| `Arcane/ArcaneEditor/src/ShaderEditorDocument.hpp/.cpp` | MODIFIED | Drag brackets -> EditGesture; renames -> StableTextEdit; gesture members deleted |
| `Arcane/ArcaneEditor/src/SpriteDocument.hpp/.cpp` | MODIFIED | Services gains undo; drags bracketed; `SpriteDataEditCommand` |
| `Arcane/premake5.lua` | MODIFIED | ArcaneTests source-compiles `EditGesture.cpp` + `SpriteDocument.cpp` |
| `Arcane/Tests/src/EditGestureTest.cpp` | NEW | Pure-core decision table `[editor]` |
| `Arcane/Tests/src/SpriteDocumentUndoTest.cpp` | NEW | Sprite command round-trip `[editor]` |

---

### Task 1: EditGesture pure decision core (TDD)

**Files:**
- Create: `Arcane/ArcaneEditor/src/EditGesture.hpp`, `Arcane/ArcaneEditor/src/EditGesture.cpp`
- Modify: `Arcane/premake5.lua` (ArcaneTests `files` list, after the `EditorCamera.cpp` entry ~line 599)
- Test: `Arcane/Tests/src/EditGestureTest.cpp`

**Interfaces:**
- Consumes: `Arcane::TransactionId` from `<Arcane/Edit/CommandStack.hpp>`.
- Produces (later tasks rely on these EXACT names): `Arcane::Editor::EditGesture::Slots`,
  `EndAction { None, Commit, Cancel }`, `EvaluateEnd(const Slots&, std::uint32_t lastItemId,
  bool deactivatedAfterEdit, bool deactivated)`, `ShouldCloseAbandoned(const Slots&,
  std::uint32_t activeId, bool hasPendingCommit)`, `ShouldCloseStaleOnActivate(const Slots&,
  bool hasPendingCommit)`.

- [ ] **Step 1: Write the header's pure section**

```cpp
#pragma once

// Arcane::Editor::EditGesture -- the ONE gesture bracket for cross-frame edit
// gestures (drags, text entries) against the shared CommandStack. Owns the two
// invariants every hand-rolled copy kept breaking (the 2026-07-26 audit's
// CRITICAL 1 class): only the widget that OPENED a gesture may close it, and
// every gesture has a guaranteed close path (abandonment COMMITS -- an edit
// already applied live must land on the undo stack, never strand).
//
// PURE CORE (Slots + the free functions below): decision logic over ImGui
// facts passed in as plain integers/bools -- no imgui.h include, so the
// [editor] units drive the full decision table headlessly (EditGestureTest).
// The ImGui-facing skin arrives in Task 2 and stays thin.

#include <Arcane/Edit/CommandStack.hpp>

#include <cstdint>

namespace Arcane::Editor::EditGesture
{
    // The cross-frame slots. ImGui item ids are 32-bit (ImGuiID = unsigned
    // int); the skin's TU static_asserts that so this header never needs
    // imgui.h.
    struct Slots
    {
        Arcane::TransactionId txn  = Arcane::TransactionId::None;
        std::uint32_t         item = 0;   // id of the widget that OPENED txn
    };

    enum class EndAction { None, Commit, Cancel };

    // EndOnDeactivate's verdict. Ownership guard FIRST: a row that never
    // opened the gesture still reports its own deactivation on one-frame
    // ActiveId handoffs, and without the guard its pure click would Cancel
    // the owner's live transaction -- Cancel discards WITHOUT reverting.
    // A joined gesture (txn == None, item parked) still evaluates: Commit/
    // Cancel on None no-op at the stack, and the slots must clear either way.
    [[nodiscard]] EndAction EvaluateEnd(const Slots& s, std::uint32_t lastItemId,
                                        bool deactivatedAfterEdit,
                                        bool deactivated) noexcept;

    // Abandonment test for ScopeGuard: a parked gesture whose owner no longer
    // holds ActiveId (widget vanished / window collapsed mid-gesture).
    // `hasPendingCommit` covers the builder-style JOINER edge: txn == None
    // (joined a gizmo drag) but a built command is still owed to the stack.
    [[nodiscard]] bool ShouldCloseAbandoned(const Slots& s, std::uint32_t activeId,
                                            bool hasPendingCommit) noexcept;

    // Activation-time stale check: a still-parked gesture means the previous
    // owner never got to close (click-through to a widget drawn ABOVE it in
    // submission order); close-and-commit it before opening ours.
    [[nodiscard]] bool ShouldCloseStaleOnActivate(const Slots& s,
                                                  bool hasPendingCommit) noexcept;
}
```

- [ ] **Step 2: Write STUB implementations in EditGesture.cpp** (every function returns
  `EndAction::None` / `false`) so the test TU links and the suite runs red first.

- [ ] **Step 3: Register with ArcaneTests.** In `Arcane/premake5.lua`, append to the
  ArcaneTests `files` list (after the `EditorCamera.cpp` entry, keeping the comment
  convention of its neighbors):

```lua
        -- Widget layer: EditGesture's PURE decision core (gesture ownership +
        -- close-path verdicts) source-compiles into the test exe so the
        -- [editor] units drive the full decision table headlessly -- the ImGui
        -- skin in the same TU is never called, same pattern as
        -- ShaderEditorDocument above.
        "%{wks.location}/ArcaneEditor/src/EditGesture.cpp",
```

Run `Arcane\GenerateProjects.bat`.

- [ ] **Step 4: Write the failing tests** at `Arcane/Tests/src/EditGestureTest.cpp`:

```cpp
#include "EditGesture.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace Arcane::Editor;
using Arcane::TransactionId;

TEST_CASE("EditGesture pure core: EvaluateEnd decision table", "[editor]")
{
    EditGesture::Slots s;
    s.txn  = static_cast<TransactionId>(7);
    s.item = 42;

    SECTION("owner mismatch is inert regardless of flags")
    {
        CHECK(EditGesture::EvaluateEnd(s, 99, true,  true) == EditGesture::EndAction::None);
        CHECK(EditGesture::EvaluateEnd(s, 99, false, true) == EditGesture::EndAction::None);
    }
    SECTION("owner + deactivated-after-edit commits")
    {
        CHECK(EditGesture::EvaluateEnd(s, 42, true, true) == EditGesture::EndAction::Commit);
    }
    SECTION("owner + plain deactivation cancels (a pure click never leaks a step)")
    {
        CHECK(EditGesture::EvaluateEnd(s, 42, false, true) == EditGesture::EndAction::Cancel);
    }
    SECTION("owner + still-active does nothing")
    {
        CHECK(EditGesture::EvaluateEnd(s, 42, false, false) == EditGesture::EndAction::None);
    }
    SECTION("joined gesture (txn None, item parked) still evaluates")
    {
        s.txn = TransactionId::None;
        CHECK(EditGesture::EvaluateEnd(s, 42, true, true) == EditGesture::EndAction::Commit);
    }
}

TEST_CASE("EditGesture pure core: abandonment + stale checks", "[editor]")
{
    EditGesture::Slots s;

    SECTION("cleared slots never close")
    {
        CHECK_FALSE(EditGesture::ShouldCloseAbandoned(s, 0, false));
        CHECK_FALSE(EditGesture::ShouldCloseStaleOnActivate(s, false));
    }
    SECTION("open txn + owner still holds ActiveId -> healthy, no close")
    {
        s.txn = static_cast<TransactionId>(3); s.item = 42;
        CHECK_FALSE(EditGesture::ShouldCloseAbandoned(s, 42, false));
    }
    SECTION("open txn + ActiveId moved on (or nothing active) -> abandoned")
    {
        s.txn = static_cast<TransactionId>(3); s.item = 42;
        CHECK(EditGesture::ShouldCloseAbandoned(s, 0, false));
        CHECK(EditGesture::ShouldCloseAbandoned(s, 7, false));
    }
    SECTION("builder-style joiner: txn None but a command is owed")
    {
        s.item = 42;
        CHECK(EditGesture::ShouldCloseAbandoned(s, 7, true));
        CHECK(EditGesture::ShouldCloseStaleOnActivate(s, true));
    }
    SECTION("open txn -> stale on a fresh activation")
    {
        s.txn = static_cast<TransactionId>(3); s.item = 42;
        CHECK(EditGesture::ShouldCloseStaleOnActivate(s, false));
    }
}
```

- [ ] **Step 5: Build + run, expect RED.** `ArcaneTests.exe "[editor]"` from the exe dir --
  the new sections must FAIL against the stubs (proves the tests bite).

- [ ] **Step 6: Implement the real bodies** in EditGesture.cpp:

```cpp
#include "EditGesture.hpp"

namespace Arcane::Editor::EditGesture
{
    EndAction EvaluateEnd(const Slots& s, std::uint32_t lastItemId,
                          bool deactivatedAfterEdit, bool deactivated) noexcept
    {
        if (lastItemId != s.item)  return EndAction::None;
        if (deactivatedAfterEdit)  return EndAction::Commit;
        if (deactivated)           return EndAction::Cancel;
        return EndAction::None;
    }

    bool ShouldCloseAbandoned(const Slots& s, std::uint32_t activeId,
                              bool hasPendingCommit) noexcept
    {
        if (s.txn == Arcane::TransactionId::None && !hasPendingCommit)
            return false;
        return activeId != s.item;
    }

    bool ShouldCloseStaleOnActivate(const Slots& s, bool hasPendingCommit) noexcept
    {
        return s.txn != Arcane::TransactionId::None || hasPendingCommit;
    }
}
```

- [ ] **Step 7: Build + run, expect GREEN**, then the full `~[gpu]` gate.

- [ ] **Step 8: Commit.**
  `feat(editor): EditGesture pure decision core + [editor] decision-table units`

---

### Task 2: EditGesture ImGui skin

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditGesture.hpp` (append skin section),
  `Arcane/ArcaneEditor/src/EditGesture.cpp` (append skin bodies)

**Interfaces:**
- Consumes: Task 1's pure core; `Arcane::CommandStack`; `Arcane::FunctionRef` from
  `<Arcane/Util/FunctionRef.hpp>`; imgui + imgui_internal (in the .cpp ONLY --
  `GetActiveID` is internal-only, imgui_internal.h:3532).
- Produces (later tasks rely on these EXACT names): `EditGesture::GestureState`
  (members: `Slots slots; std::function<void()> pendingCommit; std::string stringSeed;
  std::uint32_t stringSeedItem;`), `BeginOnActivate(CommandStack*, GestureState&,
  FunctionRef<std::string()>, FunctionRef<std::function<void()>()>)`,
  `EndOnDeactivate(CommandStack*, GestureState&)`,
  `ClosePending(CommandStack&, GestureState&)`, `struct ScopeGuard`.

- [ ] **Step 1: Append to EditGesture.hpp** (below the pure core, same namespace; add
  `<functional>` and `<string>` includes and the FunctionRef include):

```cpp
    // ---- ImGui-facing skin (thin; NOT unit-driven -- the pure core above is) ----

    struct GestureState
    {
        Slots slots;
        // Builder-style adopters park the command build here at open; it runs
        // EXACTLY ONCE, at close (release-commit, stale-close, or abandoned
        // close), then clears. Empty for snapshot-style gestures -- their
        // before-state rides the open transaction itself (SnapshotComponent).
        std::function<void()> pendingCommit;
        // The string row's activation-time cancel reference and its owner,
        // moved verbatim from InspectorState (see the String arm in
        // InspectorView.cpp for why the seed is latched at activation).
        std::string   stringSeed;
        std::uint32_t stringSeedItem = 0;
    };

    // Call IMMEDIATELY after submitting a widget. On that widget's activation:
    // close-and-commit any stale parked gesture, Begin(label()), park the
    // owner id, then park onOpened()'s returned command build. Both callbacks
    // run ONLY on the activation frame (zero cost otherwise). Snapshot-style
    // adopters do their SnapshotComponent fan-out INSIDE onOpened -- it runs
    // AFTER Begin, so the snapshots land in the open transaction -- and
    // return an empty std::function. Null stack = Play mode: whole bracket
    // no-ops (the Inspector's existing rule, now everyone's).
    void BeginOnActivate(Arcane::CommandStack* stack, GestureState& st,
                         Arcane::FunctionRef<std::string()> label,
                         Arcane::FunctionRef<std::function<void()>()> onOpened);

    // Owner-guarded close on the widget's deactivation; safe to call for
    // every row every frame. Commit runs pendingCommit first (the built
    // command joins the open transaction via CommandStack::Push's join rule);
    // Cancel discards it -- nothing was applied on a pure click.
    void EndOnDeactivate(Arcane::CommandStack* stack, GestureState& st);

    // Commit-close a parked gesture NOW (stale-handoff + abandonment paths).
    // Commit semantics on purpose: the live edit already applied, so closing
    // must land it on the undo stack -- Cancel would discard WITHOUT
    // reverting and strand an un-undoable edit.
    void ClosePending(Arcane::CommandStack& stack, GestureState& st);

    // The generalized GestureCloseGuard: declare as the FIRST local of every
    // panel/document draw scope that opens gestures, so it destructs LAST --
    // it closes abandoned gestures on every exit path, including early
    // returns and collapsed windows where widgets never report deactivation.
    struct ScopeGuard
    {
        Arcane::CommandStack* stack;   // null tolerated (Play mode)
        GestureState&         st;
        ~ScopeGuard();
    };
```

- [ ] **Step 2: Append the skin bodies to EditGesture.cpp:**

```cpp
#include <imgui.h>
#include <imgui_internal.h>   // GetActiveID (internal-only, imgui_internal.h:3532);
                              // EditorPanels.cpp already pairs both includes (:23-24)

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<ImGuiID, unsigned int> && sizeof(ImGuiID) == 4,
              "EditGesture's pure core mirrors ImGuiID as uint32_t");

namespace Arcane::Editor::EditGesture
{
    void ClosePending(Arcane::CommandStack& stack, GestureState& st)
    {
        if (st.pendingCommit)
        {
            st.pendingCommit();
            st.pendingCommit = nullptr;
        }
        stack.Commit(st.slots.txn);   // no-op on None (joined gesture)
        st.slots = {};
    }

    void BeginOnActivate(Arcane::CommandStack* stack, GestureState& st,
                         Arcane::FunctionRef<std::string()> label,
                         Arcane::FunctionRef<std::function<void()>()> onOpened)
    {
        if (!stack || !ImGui::IsItemActivated())
            return;
        if (ShouldCloseStaleOnActivate(st.slots, static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
        st.slots.txn  = stack->Begin(label());
        st.slots.item = ImGui::GetItemID();
        st.pendingCommit = onOpened();
    }

    void EndOnDeactivate(Arcane::CommandStack* stack, GestureState& st)
    {
        if (!stack)
            return;
        switch (EvaluateEnd(st.slots, ImGui::GetItemID(),
                            ImGui::IsItemDeactivatedAfterEdit(),
                            ImGui::IsItemDeactivated()))
        {
            case EndAction::Commit:
                if (st.pendingCommit)
                {
                    st.pendingCommit();
                    st.pendingCommit = nullptr;
                }
                stack->Commit(st.slots.txn);
                break;
            case EndAction::Cancel:
                st.pendingCommit = nullptr;
                stack->Cancel(st.slots.txn);
                break;
            case EndAction::None:
                return;
        }
        st.slots = {};
    }

    ScopeGuard::~ScopeGuard()
    {
        if (!stack)
            return;
        if (ShouldCloseAbandoned(st.slots, ImGui::GetActiveID(),
                                 static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
    }
}
```

- [ ] **Step 3: Build the workspace** (the skin must compile in BOTH ArcaneEditor and
  ArcaneTests -- the tests never call it, same as ShaderEditorDocument's Draw). Full
  `~[gpu]` gate stays green.

- [ ] **Step 4: Commit.**
  `feat(editor): EditGesture ImGui skin -- bracket, ClosePending, ScopeGuard`

---

### Task 3: EditorWidgets -- lift the vocabulary + new StableTextEdit

**Files:**
- Create: `Arcane/ArcaneEditor/src/EditorWidgets.hpp`, `Arcane/ArcaneEditor/src/EditorWidgets.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp` (delete moved code, re-point call sites)

**Interfaces:**
- Consumes: imgui; `Astra::Range` (`<Astra/Reflection/Attribute.hpp>`);
  `Arcane::FunctionRef`. NO other reflection headers -- EditorWidgets is reflection-free.
- Produces (exact names later tasks use): `namespace Arcane::Editor` free functions
  `FieldLabelCell`, `AxisDragFloatN`, `DrawAxisBar`, `InputTextString`,
  `RangedDragFloat(const char* label, float* v, float fallbackSpeed,
  const std::optional<Astra::Range>&)`, `RangedDragInt(const char* label, int* v,
  const std::optional<Astra::Range>&)`; RAII `struct FieldGrid` (explicit
  `operator bool`), RAII `struct HeaderBand`; `struct TextCommitState` +
  `StableTextEdit`.

- [ ] **Step 1: Move the free helpers verbatim.** From EditorPanels.cpp's anonymous
  namespace into `EditorWidgets.cpp` (declared in `EditorWidgets.hpp`, namespace
  `Arcane::Editor`), keeping every comment:
  - `InputTextString` (anchor: `bool InputTextString(const char* label, std::string* s`, ~:498)
  - the field-grid block: `kLabelColumnFraction`, `BeginFieldGrid`, `EndFieldGrid`
    (anchor comment: "The two-column field grid (UE's Details-panel shape", ~:1129-1272).
    NOTE (amendment 2): `BeginFieldGrid` dereferences `ImGuiTable*` for
    `table->LastResizedColumn` -- an imgui_internal.h-only type; EditorWidgets.cpp
    includes `imgui_internal.h` alongside `imgui.h`, as EditorPanels.cpp does today.
  - `FieldLabelCell` (~:1290)
  - `DrawAxisBar` (~:1351), `AxisDragFloatN` (~:1447)
  - `PushHeaderBandColors` / `PopHeaderBandColors` (~:1389-1396)
  - the ranged-drag block INCLUDING its private helpers `DragSpeedFor`,
    `ToFloatClamped`, `ToInt32Clamped`, `BindingRange` (read the block above :1108 --
    move whatever those bodies reference so EditorWidgets.cpp is self-contained; the
    private helpers stay in EditorWidgets.cpp's OWN anonymous namespace).

- [ ] **Step 2: Reshape exactly two things while moving (everything else verbatim):**
  1. `BeginFieldGrid`'s label-width authority becomes a `float& labelColWidth` parameter
     (today it reads/writes `InspectorState::labelColWidth` -- keep the width-sync
     protocol comments AND the `LastResizedColumn == 0` discriminator logic byte-for-byte;
     only the storage location changes). Callers pass `state.labelColWidth`.
  2. `RangedDragFloat`/`RangedDragInt` lose their `Astra::FieldInfo` parameter and take
     `const std::optional<Astra::Range>&` instead (the body already branches on exactly
     that -- see :1111). The `FieldInfo`-taking convenience overloads move in Task 4 to
     InspectorView; UNTIL then leave two 3-line forwarding wrappers in EditorPanels.cpp's
     anon namespace calling the new core (they are deleted in Task 4).

- [ ] **Step 3: Add the RAII types to EditorWidgets.hpp** (thin, over the moved functions):

```cpp
    // Two-column field region. Bool-convertible: ImGui::BeginTable can refuse
    // (culled/clipped host window) -- draw NO rows then, and the dtor must
    // not End what never began.
    struct FieldGrid
    {
        FieldGrid(const char* id, float& labelColWidth);
        ~FieldGrid();
        explicit operator bool() const noexcept { return m_open; }
        FieldGrid(const FieldGrid&) = delete;
        FieldGrid& operator=(const FieldGrid&) = delete;
    private:
        bool m_open = false;
    };

    struct HeaderBand
    {
        HeaderBand();    // PushHeaderBandColors
        ~HeaderBand();   // PopHeaderBandColors
        HeaderBand(const HeaderBand&) = delete;
        HeaderBand& operator=(const HeaderBand&) = delete;
    };
```

  `BeginFieldGrid`/`EndFieldGrid`/`Push`/`PopHeaderBandColors` become PRIVATE to
  EditorWidgets.cpp (anon namespace there); the RAII types are the only public form --
  no-legacy means no parallel begin/end API.

- [ ] **Step 4: Add `StableTextEdit`** (NEW -- the shader editor's stable-buffer commit
  pattern, extracted once; adopted in Task 6):

```cpp
    // One inline stable-buffer text edit: seeds from `current`, holds typed
    // text across frames while active (keyed by `key`, unique per edit site),
    // fires `commit(newText)` EXACTLY ONCE on deactivate-after-edit when the
    // text actually changed. Mutation happens only inside `commit`, so an
    // abandoned edit (window closed mid-typing) mutates nothing and needs no
    // undo coverage -- this is the single-shot cousin of EditGesture, not a
    // replacement for it.
    struct TextCommitState
    {
        std::uint64_t activeKey = 0;   // 0 = no edit in flight
        char          buf[64]   = {};
    };
    bool StableTextEdit(const char* imguiLabel, TextCommitState& st, std::uint64_t key,
                        std::string_view current, float width,
                        Arcane::FunctionRef<void(const char*)> commit);
```

  Body (EditorWidgets.cpp) -- the four shader-editor copies' exact state machine:

```cpp
    bool StableTextEdit(const char* imguiLabel, TextCommitState& st, std::uint64_t key,
                        std::string_view current, float width,
                        Arcane::FunctionRef<void(const char*)> commit)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*s",
                      static_cast<int>(current.size()), current.data());
        if (st.activeKey == key)
            std::memcpy(buf, st.buf, sizeof(buf));
        ImGui::SetNextItemWidth(width);
        ImGui::InputText(imguiLabel, buf, sizeof(buf));
        if (ImGui::IsItemActive())
        {
            st.activeKey = key;
            std::memcpy(st.buf, buf, sizeof(st.buf));
            return false;
        }
        if (st.activeKey != key)
            return false;
        const bool committed = ImGui::IsItemDeactivatedAfterEdit();
        st.activeKey = 0;
        if (committed && current != st.buf)
        {
            commit(st.buf);
            return true;
        }
        return false;
    }
```

- [ ] **Step 5: Re-point every EditorPanels.cpp call site** (`#include "EditorWidgets.hpp"`;
  grid call sites become `FieldGrid` locals honoring the bool contract; header-band
  push/pop pairs become `HeaderBand` scopes; Outliner's `InputTextString` calls resolve
  to the moved function unchanged). Delete the moved originals. EditorPanels.cpp must
  contain NO body that also exists in EditorWidgets.cpp.

- [ ] **Step 6: Build workspace; full `~[gpu]` gate; confirm ArcaneEditor.exe timestamp
  moved.** Behavior contract: pixel-identical.

- [ ] **Step 7: Commit.**
  `refactor(editor): lift the widget vocabulary into EditorWidgets + StableTextEdit`

---

### Task 4: InspectorView -- promote the field visitor

**Files:**
- Create: `Arcane/ArcaneEditor/src/InspectorView.hpp`, `Arcane/ArcaneEditor/src/InspectorView.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorPanels.cpp`, `Arcane/ArcaneEditor/src/EditorPanels.hpp`

**Interfaces:**
- Consumes: EditorWidgets (Task 3), EditGesture types (Tasks 1-2 -- machinery swap is
  Task 5, NOT here), `InspectorFields.hpp`, `InspectorMeta.hpp`.
- Produces:

```cpp
    // InspectorView.hpp
    struct ReflectedComponentArgs
    {
        Astra::Registry&                      registry;
        const Astra::Registry::ComponentInfo& component;
        std::span<const Astra::Entity>        selection;   // primary first
        Arcane::CommandStack*                 undo;        // null while Play runs
        const Arcane::Project*                project;     // may be null
        const InspectorServices*              services;    // may be null
        InspectorState&                       state;
        std::string_view                      componentDisplayName;
        std::string_view                      activeCategory;
        std::string_view                      filterQuery;
    };
    void DrawReflectedComponent(const ReflectedComponentArgs& args);
```

- [ ] **Step 1: Move `ImGuiFieldVisitor` verbatim** (struct at ~:1493 through its `Visit`
  override's end) plus everything only it uses -- `MultiScalarRow` and the string-arm
  seed logic are members and move with it; `CloseAbandonedGesture` (~:2338) and
  `GestureCloseGuard` (~:2364) move too (they die in Task 5, but they move intact first
  -- two mechanical steps beat one clever one). Also move the `FieldInfo`-taking
  `RangedDrag*` wrappers here (deleting Task 3's temporary forwarders).

- [ ] **Step 2: Extract the entry point.** In EditorPanels.cpp's `DrawInspectorPanel`,
  locate the two visitor drive sites (`ci.descriptor->visitFields(ci.data, visitor)`,
  ~:2727 and ~:2767 -- the categorized and uncategorized passes). Everything that
  configures the visitor for ONE component-and-category drive (member wiring through the
  `visitFields` call) becomes `DrawReflectedComponent`'s body in InspectorView.cpp; the
  panel keeps the component loop, headers, category enumeration, Add/Remove, search, and
  calls the new entry with both category passes.

- [ ] **Step 3: Build; full gate; editor exe timestamp check.** Pixel-identical contract
  again -- this task moves code and introduces one function boundary, nothing else.

- [ ] **Step 4: Commit.**
  `refactor(editor): promote the reflected-field visitor behind InspectorView::DrawReflectedComponent`

---

### Task 5: Inspector gestures -> EditGesture (delete the originals)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorView.cpp`, `Arcane/ArcaneEditor/src/EditorPanels.hpp`,
  `Arcane/ArcaneEditor/src/EditorPanels.cpp`, plus whatever `InspectorState` field
  references the compiler then flags (expect `EditorApp*.cpp`).

**Interfaces:**
- Consumes: the full EditGesture API (Tasks 1-2).
- Produces: `InspectorState` (EditorPanels.hpp) now reads:

```cpp
    struct InspectorState
    {
        EditGesture::GestureState gesture;   // txn+owner slots, pendingCommit,
                                             // string seed -- see EditGesture.hpp
        char  searchBuffer[128] = {};
        float labelColWidth = 0.0f;
    };
```

  (Move the four retired members' explanatory comments to EditGesture.hpp where their
  subjects now live -- do not orphan them, do not delete the knowledge.)

- [ ] **Step 1: Swap the visitor's machinery.** Inside `ImGuiFieldVisitor`:
  - `BeginGestureIfActivated(field, primaryInstance)` body becomes:

```cpp
    EditGesture::BeginOnActivate(stack, *gesture,
        [&] { return "Edit " + typeName + "." + field; },
        [&]
        {
            // Snapshot-style: one Begin + N snapshots + one Commit = one undo
            // step for the whole fan-out (CommandStack dedupes per
            // (entity, descriptor)). Runs AFTER Begin, inside the transaction.
            ForEachTarget(primaryInstance,
                          [&](Astra::Entity e, void*) { stack->SnapshotComponent(e, descriptor); });
            return std::function<void()>{};
        });
```

  - `EndGesture()` body becomes `EditGesture::EndOnDeactivate(stack, *gesture);`
  - the visitor's `gestureTxn`/`gestureItem` pointer members collapse to one
    `EditGesture::GestureState* gesture` wired from `state.gesture`; the string arm's
    `stringSeed`/`stringSeedItem` pointers re-point to `gesture->stringSeed` /
    `gesture->stringSeedItem`.
  - Keep the wrapper method names (`BeginGestureIfActivated`/`EndGesture`) -- the call
    sites in `Visit` stay untouched; only the bodies delegate. Move their block comments
    (the one-frame-handoff explanation at ~:1586-1595, the ownership-guard essay at
    ~:1747-1779) onto the EditGesture functions that now own those behaviors.
- [ ] **Step 2: Replace the guard.** `DrawInspectorPanel`'s first local becomes
  `const EditGesture::ScopeGuard gestureGuard{ &undo, state.gesture };` (keep the
  "FIRST local, destructs LAST" comment). DELETE `CloseAbandonedGesture` and
  `GestureCloseGuard` from InspectorView.cpp -- their comments move to
  `ClosePending`/`ScopeGuard` if EditGesture.hpp doesn't already carry the substance.
- [ ] **Step 3: Update `InspectorState`** per the Interfaces block; chase compiler errors
  (references to `state.gestureTxn` etc. outside the visitor -- expect
  `CanEditStructure` / structural-edit gating in EditorPanels.cpp to read
  `undo.InTransaction()` already; verify, do not assume). Amendment 3: a SECOND
  `InTransaction()` consumer exists -- the Ctrl+Z/Ctrl+Y keybind gate
  (`EditorAppFrame.cpp:424-426`, `noOpenTxn`). Both consumers must behave
  identically after the swap; their breadth is also why the teardown close
  (amendment 1) matters -- a stranded open transaction disables undo editor-wide.
- [ ] **Step 4: Build; full gate; editor exe timestamp.** The five CommandStack ownership
  regression cases (from the CRITICAL-1 fix) MUST stay green -- they pin exactly the
  semantics this task re-homes.
- [ ] **Step 5: Commit.**
  `refactor(editor): Inspector gestures ride EditGesture; hand-rolled machinery deleted`

---

### Task 6: Shader editor adoption

**Files:**
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.hpp`, `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp`

**Interfaces:**
- Consumes: EditGesture API; `StableTextEdit` (Task 3); existing `PushGraphUndo`,
  `PushPassUndo`, `ParamEditCommand`, `ActiveGraphOpt`, `CapturePassListState`,
  `BeginParamRename` (all already in this class -- read their current signatures before
  writing calls; the shader-graph arc may have touched them).
- Produces: `ShaderEditorDocument` member `EditGesture::GestureState m_gesture;` and
  `TextCommitState m_textEdit;` replacing `m_graphGestureBefore`, `m_gestureHadBefore`,
  `m_gestureBefore`, `m_nameEditNode`, `m_passNameEditIdx`, `m_nameBuf`.

- [ ] **Step 1: Document-scope guard + teardown close (amendment 1).** At the top of the
  document's `Draw` (first local):
  `const EditGesture::ScopeGuard gestureGuard{ m_services.undo, m_gesture };`
  AND in `~ShaderEditorDocument` (it exists -- today it only destroys the two
  node-editor contexts), before those teardowns:

```cpp
    if (m_services.undo)
        EditGesture::ClosePending(*m_services.undo, m_gesture);
```

  WHY: documents are DESTROYED synchronously on close with no on-close hook
  (`DocumentHost::Close` erases the unique_ptr, DocumentHost.cpp:123; `CloseAll`
  :116 on project switch). The X-button path is safe -- `requestClose` is raised
  inside `Draw` and executed after the draw loop (`DrawAll`, :137), so ScopeGuard
  has already run -- but any close that destroys the doc between gesture-park and
  its next `Draw` (hotkey close, project-switch `CloseAll`) would strand the
  transaction open, leaving `InTransaction()` true editor-wide: structural edits
  refused AND Ctrl+Z/Ctrl+Y dead (`EditorAppFrame.cpp:424-426`). Today nothing can
  strand because documents never `Begin()`; this task changes that, so this task
  closes the hole.
- [ ] **Step 2: Convert the graph-node drag lambdas** (anchor: "Gesture helpers (used by
  pin rows AND payload widgets below)", ~:3148). The label moves from `gestureEnd` to
  `gestureBegin` -- update every call site pair mechanically (they are adjacent lines;
  pattern `gestureBegin(); ... gestureEnd("Label");` becomes
  `gestureBegin("Label"); ... gestureEnd();`):

```cpp
        auto gestureBegin = [&](const char* label)
        {
            EditGesture::BeginOnActivate(m_services.undo, m_gesture,
                [&] { return std::string(label); },
                [&]
                {
                    // Whole-graph before, captured at activation; the command
                    // builds at CLOSE from this + whatever the drag did --
                    // which is why an abandoned drag now lands on the stack
                    // instead of vanishing (spec section 5, change 1).
                    return std::function<void()>(
                        [this, label = std::string(label),
                         before = ActiveGraphOpt()]() mutable
                        { PushGraphUndo(label.c_str(), std::move(before)); });
                });
        };
        auto gestureEnd = [&] { EditGesture::EndOnDeactivate(m_services.undo, m_gesture); };
```

  (Adapt `PushGraphUndo`'s exact parameter types from its live signature -- it takes the
  label and the `std::optional<Arcane::MaterialGraph>` before-state today.) Delete the
  `m_graphGestureBefore` member.
- [ ] **Step 3: Convert the params-panel drag** (anchor: "Before-state captured on widget
  activation (m_gesture*)", ~:3817; the `IsItemActivated`/`IsItemDeactivatedAfterEdit`
  pair at ~:3885-3907). Replace both blocks with, immediately after the widget switch:

```cpp
            EditGesture::BeginOnActivate(m_services.undo, m_gesture,
                [&] { return "Edit " + d.name; },
                [&]
                {
                    const bool hadBefore = m_instance->HasOverride(d.nameHash);
                    Arcane::MatParamValue before{};
                    if (hadBefore)
                        m_instance->GetParam(d.nameHash, before);
                    return std::function<void()>(
                        [this, nameHash = d.nameHash, name = d.name, hadBefore, before]
                        {
                            Arcane::MatParamValue after;
                            if (m_instance && m_instance->GetParam(nameHash, after))
                                m_services.undo->Push(std::make_unique<ParamEditCommand>(
                                    m_anchor, nameHash, "Edit " + name,
                                    hadBefore, before, /*hasAfter=*/true, after));
                        });
                });
            if (edited)
                m_instance->Set(d.nameHash, value);   // LIVE apply, unchanged
            EditGesture::EndOnDeactivate(m_services.undo, m_gesture);
```

  (Mirror `ParamEditCommand`'s live constructor signature exactly; the `if (edited)`
  live-apply line and its comment stay.) Delete `m_gestureHadBefore`/`m_gestureBefore`.
  Update the :3817 comment to name the bracket. NOTE the ordering: BeginOnActivate
  BEFORE the live `Set`, so `before` is captured pre-mutation on the activation frame --
  today's code has the same order (activation block above the `edited` block).
- [ ] **Step 4: Convert the four stable-buffer rename sites to `StableTextEdit`**,
  deleting `m_nameEditNode`/`m_passNameEditIdx`/`m_nameBuf` and adding
  `TextCommitState m_textEdit;`. Keys must be unique across site KINDS (pass indices and
  node ids could collide numerically):

```cpp
        // ShaderEditorDocument.hpp, next to m_textEdit:
        enum class TextEditKind : std::uint64_t { PassName = 1, NodeName, Swizzle, Comment };
        static constexpr std::uint64_t TextKey(TextEditKind k, std::uint64_t id) noexcept
        { return (static_cast<std::uint64_t>(k) << 56) | id; }
```

  Example conversion, the swizzle site (~:3363-3391; the other three follow the same
  shape with their own commit bodies -- pass rename keeps `CapturePassListState` +
  `PushPassUndo`, param/texture name keeps `valueEdited()` + `PushGraphUndo` +
  `BeginParamRename(old, new)`, comment title keeps its annotation-only dirty comment):

```cpp
            case Arcane::GraphNodeType::Swizzle:
            {
                // Mask edit: stable-buffer commit via StableTextEdit (one
                // shared TextCommitState -- only one InputText is active at a
                // time; keys are namespaced per site kind).
                StableTextEdit("##mask", m_textEdit, TextKey(TextEditKind::Swizzle, n.id),
                               n.swizzleMask, 70.0f,
                               [&](const char* text)
                               {
                                   std::optional<Arcane::MaterialGraph> before = ActiveGraphOpt();
                                   n.swizzleMask = text;
                                   valueEdited();
                                   PushGraphUndo("Edit Swizzle", std::move(before));
                               });
                break;
            }
```

- [ ] **Step 5: Sweep the file.** `IsItemActivated|IsItemDeactivated` must not appear in
  ShaderEditorDocument.cpp outside comments after this task (StableTextEdit and
  EditGesture own them). If the shader-graph arc added NEW sites of either pattern,
  convert them the same way -- full adoption includes code that lands between this
  plan's writing and its execution.
- [ ] **Step 6: Build; full gate (ShaderEditorDocument compiles into ArcaneTests -- its
  headless suites must stay green); editor exe timestamp.**
- [ ] **Step 7: Commit.**
  `refactor(editor): shader editor rides EditGesture + StableTextEdit; gesture members deleted`

---

### Task 7: SpriteDocument -- undo, bracketed drags (TDD)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/SpriteDocument.hpp`, `Arcane/ArcaneEditor/src/SpriteDocument.cpp`,
  `Arcane/premake5.lua` (ArcaneTests files list), the `SpriteDocument::Services`
  construction site (grep `SpriteDocument::Services` -- expect `EditorApp*.cpp`'s
  sprite factory, near the material factory that already wires `DocServices::undo`)
- Test: `Arcane/Tests/src/SpriteDocumentUndoTest.cpp`

**Interfaces:**
- Consumes: `Arcane::ICommand` (`<Arcane/Edit/Command.hpp>`: `Undo()`, `Redo()`,
  `Label()`); EditGesture; `Arcane::CommandStack`.
- Produces: `SpriteDocument::Services` gains `Arcane::CommandStack* undo = nullptr;`;
  public `void ApplySpriteData(const Arcane::SpriteAssetData&)` (the command's re-entry
  point); doc-local `SpriteDataEditCommand`.

- [ ] **Step 1: Extend Services + fix the comment.** Add the member AND rewrite the
  header comment at SpriteDocument.hpp:35-38 -- it currently reads "a sprite has no
  compiler/undo/clock", which this task falsifies. New comment states: same services
  shape as DocServices, with `undo` = the ONE shared editor CommandStack (sprite edits
  join the global history), and no compiler/clock.
- [ ] **Step 2: The command + re-entry.** Before writing it, READ how
  `ParamEditCommand` anchors its document (ShaderEditorDocument.hpp -- the `m_anchor`
  it takes) and MIRROR that mechanism exactly; a raw `SpriteDocument*` dangles when the
  document closes with commands still on the stack. Shape:

```cpp
    // SpriteDocument.cpp -- doc-local, like ParamEditCommand.
    class SpriteDataEditCommand final : public Arcane::ICommand
    {
    public:
        SpriteDataEditCommand(/* the anchor, mirroring ParamEditCommand */,
                              std::string label,
                              Arcane::SpriteAssetData before,
                              Arcane::SpriteAssetData after);
        void Undo() override;   // resolve live doc via anchor; ApplySpriteData(m_before)
        void Redo() override;   // ... ApplySpriteData(m_after)
        const char* Label() const override { return m_label.c_str(); }
    private:
        std::string             m_label;
        Arcane::SpriteAssetData m_before, m_after;
    };
```

  `ApplySpriteData(data)` on the document: `m_data = data; m_dirty = true;` and fire
  `m_services.invalidateSprite(m_data.id)` if wired (the viewport must reflect an undo
  the same way it reflects a save -- read the Services comment at hpp:42-48).
- [ ] **Step 3: TDD the round-trip.** Add SpriteDocument.cpp to the ArcaneTests files
  list (comment per the premake convention; its Draw is ImGui but never called --
  ShaderEditorDocument precedent). New `[editor]` test: construct a `SpriteDocument`
  with a null-services `Services{}` + fixture `SpriteAssetData`, drive
  `ApplySpriteData` with changed data, CHECK the document reports the new data and
  `Dirty()`; then push a `SpriteDataEditCommand` through a real `CommandStack` (safe
  headless -- no Runtime, no registry mutation for this command type; the stack ctor's
  resolve fn can return a never-used dummy via a small lambda trick ONLY if the ctor
  demands it -- read CommandStack's ctor contract first) and CHECK Undo/Redo restore
  before/after through the anchor. If anchoring headlessly proves impossible without a
  Runtime, test `ApplySpriteData` + the command's Undo/Redo against a live document
  directly (construct command with the real anchor) and note why. RED first, then green.
- [ ] **Step 4: Bracket the drags.** In `Draw` (the block at ~:86-97): add
  `EditGesture::GestureState m_gesture;` member, ScopeGuard first-local in `Draw`, and
  after EACH of the four drag widgets:

```cpp
        const auto bracket = [&](const char* label)
        {
            EditGesture::BeginOnActivate(m_services.undo, m_gesture,
                [&] { return std::string(label); },
                [&]
                {
                    return std::function<void()>(
                        [this, label = std::string(label), before = m_data]
                        {
                            if (!(before == m_data) && m_services.undo)
                                m_services.undo->Push(std::make_unique<SpriteDataEditCommand>(
                                    /*anchor*/, label, before, m_data));
                        });
                });
            EditGesture::EndOnDeactivate(m_services.undo, m_gesture);
        };
        changed |= ImGui::DragFloat("Pixels Per Meter", &m_data.ppu, ...);   // unchanged
        bracket("Edit Pixels Per Meter");
        // ... same for Source Pos / Source Size / Pivot, each with its label.
```

  If `SpriteAssetData` has no `operator==`, add a memberwise one in SpriteAsset.hpp's
  spirit (read the struct first; do NOT memcmp a struct with padding — verified: it has
  none today, holds a `std::string`, and has tail padding, so memberwise is mandatory).
  Also add the teardown close (amendment 1, same shape and rationale as Task 6 Step 1)
  to `~SpriteDocument`, creating the destructor if the class has none: a parked sprite
  gesture must not outlive the document.
- [ ] **Step 5: Wire the stack.** At the Services construction site, pass the same
  `CommandStack` the material factory passes. Keep `m_dirty |= changed` -- dirty and
  undo are separate ledgers (Save clears dirty; undo does not).
- [ ] **Step 6: Build; full gate green (new sprite units included); editor exe timestamp.**
- [ ] **Step 7: Commit.**
  `feat(editor): SpriteDocument joins the shared undo history via EditGesture`

---

### Task 8: No-legacy sweep + desk-verify checklist

**Files:**
- Modify: whatever the sweep finds
- Create: `docs/superpowers/2026-XX-XX-widget-layer-desk-verify.md` (stamp the real date)

- [ ] **Step 1: Prove no-legacy with greps** (all from `Arcane/ArcaneEditor/src/`):
  - `IsItemActivated|IsItemDeactivated` -- hits ONLY in `EditGesture.cpp` and
    `EditorWidgets.cpp` (StableTextEdit), plus comments.
  - `PushHeaderBandColors|BeginFieldGrid` -- hits ONLY in EditorWidgets.cpp internals.
  - `m_graphGestureBefore|m_gestureBefore|m_gestureHadBefore|m_nameEditNode|m_passNameEditIdx|gestureTxn|gestureItem|CloseAbandonedGesture|GestureCloseGuard` -- ZERO hits.
  - `std::function<void\(\)> pendingCommit` -- exactly one declaration (EditGesture.hpp).
  Fix any stragglers; if a hit is a comment citing the old machinery, re-point the
  comment at EditGesture.
- [ ] **Step 2: Confirm the spec's section-5 behavior changes are real in code review
  terms** -- re-read your own diff for: abandoned-drag commit path reachable in both
  documents; `InTransaction()` gating unchanged for structural edits AND the undo
  keybinds (`EditorAppFrame.cpp:424-426`); the amendment-1 teardown close present in
  BOTH document destructors; Play-mode null stack no-ops everywhere (SpriteDocument
  included: null `undo` in Services must be tolerated by every bracket call -- it is,
  `BeginOnActivate` early-outs).
- [ ] **Step 3: Write the desk-verify checklist** (the user runs it; the harness must
  not). Contents = spec section 6's six items VERBATIM plus: (7) shader editor rename
  sites still commit once on Enter/click-away and revert on Escape; (8) sprite-doc undo
  of a ppu drag updates the viewport via the invalidate hook; (9) close a document
  mid-drag via project switch (`CloseAll`): no stranded transaction -- Ctrl+Z still
  works everywhere afterwards (amendment 1's teardown close is what this exercises).
- [ ] **Step 4: Final full build (Debug), full `~[gpu]` gate, editor exe timestamp,
  commit.**
  `docs(editor): widget-layer no-legacy sweep + desk-verify checklist`

---

## Execution Handoff

Execute task-by-task with review between tasks (subagent-driven-development when a fresh
context runs it). The desk-verify checklist from Task 8 is the arc's acceptance -- the
arc is NOT closable without the user's pass, per repo convention.
