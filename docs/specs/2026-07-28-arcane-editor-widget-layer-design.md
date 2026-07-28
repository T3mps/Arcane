# Arcane::Editor Widget Layer -- Design

**Date:** 2026-07-28
**Status:** Approved (scope, adoption, naming, and sprite-undo decisions all user-confirmed)
**Executes:** AFTER the in-flight shader-graph upgrade arc merges (same agent continues into
this arc). Line numbers cited below were verified 2026-07-28 pre-merge; the shader-graph arc
may shift ShaderEditorDocument.cpp numbers, but every cited *shape* holds.

## 1. What this is

A shared editor-vocabulary module inside the ArcaneEditor project. Not a namespace change --
everything in the editor is already `namespace Arcane::Editor` -- but a **module boundary with
owned invariants**: three file pairs whose public API every panel and document consumes
instead of hand-rolling ImGui idioms.

The two invariants the layer owns outright, because they are the recurring bug class
(2026-07-26 audit CRITICAL 1; the `!readOnly` drop-gate; ID push/pop renesting):

1. **Every cross-frame edit gesture has an owner and a guaranteed close path.**
2. **Read-only gating covers drops, not just input** (`BeginDisabled` does not gate
   `BeginDragDropTarget`; the layer's asset-ref widget does).

## 2. Verified starting state (2026-07-28)

What exists and where -- confirmed against source, not the review doc's claims:

- **Inspector (EditorPanels.cpp): complete, hardened gesture machinery**, all trapped as
  members of the anonymous-namespace `ImGuiFieldVisitor` or free functions beside it:
  `BeginGestureIfActivated` (:1582), `EndGesture` with owner-id guard (:1744),
  `CloseAbandonedGesture` (:2338), RAII `GestureCloseGuard` declared first-local /
  destructs-last (:2364, :2377), `ApplyImmediate` (:1647) and `ApplyGuidImmediate` (:1795)
  over `std::optional<ScopedTransaction>` + `ForEachTarget` multi-select fan-out.
- **Vocabulary helpers, same anonymous namespace:** `BeginFieldGrid`/`EndFieldGrid`
  (:1140/:1272, owns the label-width sync protocol), `FieldLabelCell` (:1290),
  `AxisDragFloatN` (:1447) + `DrawAxisBar` (:1351), `PushHeaderBandColors` (:1389),
  `RangedDragFloat`/`RangedDragInt` (:1108/:1118), `InputTextString` (:498).
- **Shader editor (ShaderEditorDocument.cpp): two independent hand-rolled DRAG-bracket
  implementations**, neither owner-guarded nor abandonment-safe: the
  `gestureBegin`/`gestureEnd` lambda pair (:3152-3163, whole-graph before-state in
  `m_graphGestureBefore`) and the `m_gestureHadBefore`/`m_gestureBefore` member pair
  (:3885-3907, param drags). Both mutate LIVE during the drag, so an abandoned drag is a
  silent un-undoable edit (the pre-token gizmo bug class). Both also park before-state in
  ONE shared member slot with no owner id, so a one-frame ActiveId handoff between two
  widgets (the exact class audit CRITICAL 1 closed in the Inspector) can overwrite A's
  before-state with B's before A's push runs -- derived by reading, not reproduced, but
  it is the same mechanism. Separately, **four stable-buffer rename sites** (:1416-1436
  pass name, :3104-3124 comment title, :3258-3282 param/texture name, :3370-3390 swizzle
  mask) share `m_nameEditNode`/`m_passNameEditIdx`/`m_nameBuf`: mutation happens ONLY at
  commit, so they are undo-correct today -- their duplication is four copies of the same
  ~20-line buffer state machine, a vocabulary problem, not a correctness one.
  `SetParamWithUndo` (:3786) is neither -- it is the single-shot path ("single-step undo,
  no gesture bracketing needed", :3727) and it stays.
- **SpriteDocument.cpp: no undo at all.** Four `DragFloat` rows set `m_dirty` only
  (:86-97). Its `Services` struct deliberately omits undo (SpriteDocument.hpp:38-39).
- **Outliner: already canonical on undo.** Structural edits route through
  `ApplyStructural` -> `RegistryStateCommand`; renames park on transaction collision. Its
  gap is vocabulary/styling only.
- **AssetBrowser: drag SOURCE only** (154 lines); the drop targets live in the Inspector
  AssetRef arm and the shader editor's texture params. Nothing to adopt.
- **CommandStack (Arcane/Edit/CommandStack.hpp): already unifies both undo models.**
  `Begin` returns a `TransactionId` owner token (:72); `Push` of a generic `ICommand`
  **joins the open transaction** when one is open (:82-88, `m_pendingGeneric`) --
  "material param edits, and later graph edits, share the ONE undo history through it."
  The engine API needs NO changes for this arc.
- **Astra attributes AngleFormat/ColorFormat/DragSpeed/Multiline/Precision exist
  (Attribute.hpp) but nothing in the editor reads them.** InspectorMeta consumes
  Category/Tooltip/Range/ReadOnly/Hidden only. Wiring the rest is the deferred Tier D arc.

## 3. Module design

Three file pairs, flat in `Arcane/ArcaneEditor/src/` (existing convention). Pure cores
source-compile into ArcaneTests per the established `InspectorFields.cpp` pattern; ImGui
skins do not.

### 3.1 `EditorWidgets.hpp/.cpp` -- layout & widget vocabulary (Tier A)

Lifted from EditorPanels.cpp's anonymous namespace, minimally reshaped:

- `FieldGrid` -- RAII over `BeginFieldGrid`/`EndFieldGrid`. Bool-convertible ("began")
  because `ImGui::BeginTable` can refuse (culled host window): callers draw no rows and
  the dtor must NOT call `EndTable` in that case. Owns the width-sync protocol; the shared
  label-column width becomes a caller-owned `float&` parameter (today it is
  `InspectorState::labelColWidth`). The `LastResizedColumn == 0` discriminator and the
  `ImGuiTableFlags_NoSavedSettings` rule move with it, verbatim -- both were hard-won.
- `FieldLabelCell` -- unchanged signature.
- `HeaderBand` -- RAII over `PushHeaderBandColors`/`PopHeaderBandColors`.
- `AxisDragFloatN` + `DrawAxisBar` -- unchanged.
- `RangedDragFloat`/`RangedDragInt` -- **reshaped reflection-free**: the core overloads
  take `const std::optional<Astra::Range>&` instead of `Astra::FieldInfo`. The
  `FieldInfo`-taking convenience overloads move to InspectorView (the reflection
  consumer), calling `InspectorMeta::RangeOfField` themselves. `EditorWidgets` includes
  no reflection headers.
- `InputTextString` -- unchanged (the std::string <-> ImGui::InputText adapter).
- `StableTextEdit` + `TextCommitState` -- NEW: the shader editor's stable-buffer inline
  text-commit pattern (seed from current value, hold typed text while active keyed by a
  caller id, fire a commit callback exactly once on deactivate-after-edit-with-change),
  extracted once so its four hand-rolled copies collapse to calls.

Behavioral contract: pixel-identical output to today's helpers. This tier is a lift, not a
redesign.

### 3.2 `EditGesture.hpp/.cpp` -- the gesture bracket (Tier B)

**One model for every cross-frame gesture, converging on CommandStack transactions** (the
stack already supports it; see 2. above):

- `GestureState` -- the per-panel/per-document persistent slots, generalized from
  `InspectorState`'s: `TransactionId txn`, `ImGuiID item` (the owner widget), plus the
  string-edit cancel-reference slots (`std::string seed`, `ImGuiID seedItem`) since the
  Escape-revert semantics they encode are InputText-generic, not Inspector-specific.
- **Bracket protocol**, called once after submitting a widget:
  - *Activation* (`IsItemActivated`): commit any still-parked stale token first (the
    intra-frame ActiveId-handoff fix from the audit), then `Begin`; park token + item id;
    run the adopter's before-capture. Before-capture is one of:
    (a) **snapshot-style** -- `SnapshotComponent` calls (Inspector), or
    (b) **builder-style** -- capture a before-value AND register a deferred
    `build(before, current) -> std::unique_ptr<ICommand>` (shader editor, sprite doc).
  - *Deactivation-after-edit*: ownership-guarded. Builder-style adopters build + `Push`
    (joins the open transaction), then `Commit(token)`. Snapshot-style just `Commit`.
  - *Pure-click deactivation*: `Cancel(token)` -- a no-op click never leaks an undo step.
  - *Abandonment* (ActiveId moved on while our gesture is parked; widget no longer
    submitted): `GestureScopeGuard` -- RAII at panel/document draw scope, the generalized
    `GestureCloseGuard` -- closes it. **Commit semantics, not Cancel** (matching today's
    `CloseAbandonedGesture`): builder-style adopters' registered builder runs against the
    current value, so an abandoned shader-editor drag now yields an undo record instead of
    a silent un-undoable edit. This is an intentional behavior fix.
- **Pure decision core, headless-tested**: the state machine (inputs: activated /
  deactivated-after-edit / deactivated-plain / active-id-elsewhere / owner-id-match;
  outputs: open / commit / cancel / close-abandoned / ignore) is a pure function over a
  small struct of ImGui facts. The thin skin gathers the facts (`IsItemActivated`,
  `GetItemID`, `GetActiveID`, ...) and applies the verdict. The core compiles into
  ArcaneTests; the skin does not.

Free consequence of homogenizing: `CommandStack::InTransaction()` now refuses structural
mementos mid-drag in EVERY document, not just the Inspector.

Known limitation carried forward unchanged: the **intra-group gesture loss** (tab .x -> .y
inside one multi-scalar row closes .y's fresh gesture empty; filed, deliberately unfixed;
root = the one-shared-slot design). This arc must not make it worse; fixing it is its own
arc and would slot into `GestureState` later.

### 3.3 `InspectorView.hpp/.cpp` -- reflection-driven rows (Tier C)

`ImGuiFieldVisitor` moves here whole, out of the anonymous namespace, with its coupled
row machinery: `MultiScalarRow`, the mixed-mask display, the string-arm seed logic, the
AssetRef arm (drop target + pick popup + `!readOnly` gating + sprite-mint hook), and the
`FieldInfo`-taking `RangedDrag*` overloads. Public entry point: draw one component's
reflected fields for a selection --

    void DrawReflectedComponent(<registry, component info, selection span,
                                CommandStack* (null = Play, bracketing no-ops),
                                const Project*, const InspectorServices*,
                                InspectorState&, active category, filter query>);

`DrawInspectorPanel` keeps only chrome: the window, component headers, category loop,
search box, Add/Remove Component. `InspectorState` keeps `labelColWidth` + search buffer
and embeds a `GestureState` (replacing its four hand-rolled slots).

Pure collaborators (`InspectorFields`, `InspectorMeta`) are untouched.

## 4. Adoption map -- full adoption, no shims, no legacy paths

| Surface | Converts to | Deleted |
|---|---|---|
| Inspector (EditorPanels.cpp) | All three modules; panel keeps chrome only | The entire anon-namespace helper + visitor block (~1,200 lines move out) |
| ShaderEditorDocument | `EditGesture` bracket at BOTH drag implementations (fixes abandonment + the shared-slot handoff overwrite); the four stable-buffer rename sites -> `StableTextEdit`; `GestureScopeGuard` at document draw scope. Its params-panel layout is NOT field-grid-shaped and keeps its own look | `gestureBegin`/`gestureEnd` capture bodies; `m_graphGestureBefore`, `m_gestureHadBefore`/`m_gestureBefore` members; `m_nameEditNode`/`m_passNameEditIdx`/`m_nameBuf` and their four state machines |
| SpriteDocument | `EditorWidgets` ranged drags + `EditGesture` builder-style bracket; new doc-local `SpriteDataEditCommand : ICommand` (before/after `SpriteAssetData`) | The bare `changed |= DragFloat...; m_dirty = true` block |
| Outliner | `InputTextString` from the module; `HeaderBand`/styling where applicable | -- (there is ONE `InputTextString` in the anon namespace, shared by outliner + inspector call sites; it moves, call sites re-point) |
| AssetBrowser | Nothing (drag source only) | -- |

**SpriteDocument undo plumbing** (user-approved scope addition): `SpriteDocument::Services`
gains `Arcane::CommandStack* undo = nullptr;` wired at the document-construction site the
same way ShaderEditorDocument's `DocServices` already is. The deliberate design comment at
SpriteDocument.hpp:38 ("a sprite has no compiler/undo/clock") is UPDATED, not contradicted
-- comment-truth is a standing review rule in this repo. Undo joins the one shared editor
stack; sprite edits become Ctrl+Z-able like everything else.

`SetParamWithUndo` stays as-is: a doc-local convenience over the canonical `Push`, not a
duplicated invariant.

## 5. Intentional behavior changes

1. Abandoned shader-editor drags produce undo records (were silently un-undoable).
2. Shader-editor drags open transactions -> structural edits refused mid-drag
   (matches the Inspector's existing rule).
3. SpriteDocument edits become undoable (new capability).
4. Everything else: pixel- and behavior-identical. Tier A is a lift.

## 6. Testing

- **New `[editor]` headless units** (source-compiled into ArcaneTests per the
  InspectorFields premake pattern): the `EditGesture` pure decision core (full
  decision-table coverage: open/commit/cancel/close-abandoned/owner-mismatch/stale-token),
  and the width-sync decision function if extracted pure.
- **Existing suites stay green untouched**: all `[editor]` units, the CommandStack
  ownership regression cases (5, from the CRITICAL-1 fix), gate run from the exe dir.
- **Desk-verify checklist** (owed at arc close, per repo convention):
  1. Multi-select `position.x` type-then-gizmo-drag = one undo step (the CRITICAL repro).
  2. Shader editor: drag a param, close the window mid-drag, Ctrl+Z restores (new).
  3. Sprite doc: drag ppu, Ctrl+Z restores; Save-dirty still works (new).
  4. Field-grid label-width drag still syncs across grids and never fights imgui.ini.
  5. Intra-group tab .x -> .y: the KNOWN loss reproduces identically (not worse).
  6. AssetRef drop on a ReadOnly field still refused; clear button still gated.

## 7. Out of scope (parked, in priority order)

- **Tier D**: consuming AngleFormat/ColorFormat/DragSpeed/Multiline/Precision -- the
  natural follow-up arc, now cheap on top of InspectorView.
- **Intra-group gesture loss** redesign (multi-slot GestureState).
- **Graph-canvas shared framework** (canvas/schema/undo for node editors) -- separate
  standing directive, separate arc; this arc's bracket work reduces its surface.
- Any engine-side (Arcane.dll / CommandStack) API change -- explicitly none needed.
