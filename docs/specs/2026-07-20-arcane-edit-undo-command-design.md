# Editor Undo/Redo Command Foundation — Arcane engine

**Date:** 2026-07-20
**Status:** Design — approved (scope + coverage + boundary + transaction grouping decided); awaiting spec review.
**Layer:** Arcane engine (`ARCANE_API` `Arcane/Edit/` command module), consumed by Grimoire.
The first half of SPEC #2 (gizmos + unified undo/redo); **gizmos are a separate follow-up spec** that
reuses this stack.

---

## 1. Why

Grimoire can select an entity (GPU hit-proxy pick) and edit its components in the Inspector, but there
is **no undo**: a mis-drag on a `DragFloat`, a wrong toggle, an accidental value — all are permanent.
The authored-transform-sync spec (2026-07-18) named the next authoring step "gizmos + unified undo/redo"
and prescribed a **generic reflection-based 'set component before→after' command so gizmo and inspector
edits share one stack** (referencing the old `Tools/Editor/src/UndoManager.hpp`).

The user chose to build the **undo/redo foundation first**, then gizmos on top. So this spec delivers a
generic, engine-level command/undo-history capability and wires **every Grimoire Inspector edit** through
it. Gizmo drags (next spec) push the same command type into the same stack — they plug in, no retrofit.

---

## 2. Grounding (verified against the tree)

- **Inspector edit flow** (`Arcane/Grimoire/src/EditorPanels.cpp`): `DrawInspectorPanel` iterates the
  selected entity's components via `registry.InspectEntity(entity)` → `ComponentInfo{descriptor, data,
  meta}`, and for each calls `descriptor->visitFields(data, visitor)`. The visitor renders one ImGui
  widget per reflected field and applies edits live via `Grimoire::ApplyFloatEdit(FieldInfo&, void*
  instance, value)` (and `ApplyBoolEdit` / `DragFloat2` / `DragFloat3` siblings).
- **Generic snapshot/restore seam** (`ThirdParty/Astra/include/Astra/Component/Component.hpp`): the
  component `descriptor` exposes, alongside `visitFields`, function pointers
  `serialize(BinaryWriter&, void* instance)` and `deserialize(BinaryReader&, void* instance)` (the same
  seam `Registry::Save` uses). This is a **whole-component** binary snapshot/restore — the exact
  mechanism a reflection-based before→after command needs.
- **Paused reconcile** (SPEC #1, `PhysicsSystem` PASS 3.5): while paused, the system statelessly compares
  each entity's `LocalTransform` pos/rot/scale against its physics body every frame and pushes any
  divergence into the body (SetPosition/SetAngle; scale → fixture rebuild). So **a restored
  `LocalTransform` is reflected to the body next frame with zero extra plumbing** (see §9, "PostEditUndo").
- **Grimoire seams**: `GrimoireApp` owns the `SelectionContext` + the `PlaySession` (Edit|Play), samples
  input each frame (`InputSnapshot`), and draws the Sim toolbar (`DrawSimTimeToolbar`). These are the
  wiring points for the stack, the keybinds, and the Undo/Redo toolbar affordances.
- **Prior command model**: `Tools/Editor/src/UndoManager.hpp` — a Gesture pattern (`Begin` → `Track` →
  `Commit`) plus an Immediate pattern (`Push`), stack depth 100, with a nav token per op. This design
  ports that shape to the reflection world (see §3, §6).

---

## 3. Prior art (research)

**Unreal Engine's transaction system** is essentially this design. An edit is wrapped in a *transaction*:
`BeginTransaction` → each object calls `Modify()` (which **serializes its state via reflection** into the
transaction as a "before" snapshot) → mutate → `EndTransaction` (pushed onto a size-capped transaction
buffer). `FScopedTransaction` is the RAII wrapper. Two properties we adopt: **one transaction groups
unlimited objects into a single undo step**, and `Modify()` is **idempotent within a transaction** (first
touch snapshots; later touches don't). After undo Unreal fires `PostEditUndo` so objects rebuild derived
state.

**Modern apps** split into **Memento/snapshot** (store state, restore — generic, encapsulation-preserving,
memory-heavier → cap the stack) vs **Command** (explicit do/undo, delta-based, memory-lean, but per-op
inverse logic; Figma/TLDraw at scale). For a **reflection/serialization-rich editor** the memento model is
the standard choice; universal practices are a depth cap, **coalescing rapid edits into one step**,
transaction grouping, redo-invalidation on new edits, and clearing history on context switches.

**Mapping to this design:** whole-component `descriptor->serialize` = Unreal's `Modify()` snapshot;
`CommandStack` = the transaction buffer; our "one op per drag gesture" = the recommended coalescing; our
polling paused-reconcile satisfies Unreal's `PostEditUndo` for free. The **one refinement research drove**
is transaction grouping: the stack's undo unit is a **transaction of 1..N `ComponentEditCommand`s**, so a
gizmo multi-select move (next spec) is one undo step. A single Inspector field edit is a trivial 1-command
transaction.

Sources: Unreal [FTransaction](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FTransaction)
/ [ITransaction](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/ITransaction)
/ [Lewicki, "Implement Undo/Redo in UE"](https://unreal.robertlewicki.games/p/implement-undo-and-redo-in-unreal-engine);
[Memento for undo/redo](https://curatepartners.com/tech-skills-tools-platforms/mastering-the-memento-pattern-powering-undo-redo-and-state-restoration-in-software/);
[Designing Undo/Redo](https://medium.com/@kharavela.jain/designing-undo-redo-features-7c57b5902779);
[Command-based undo](https://dev.to/npbee/command-based-undo-for-js-apps-34d6).

---

## 4. Architecture

A new **`Arcane/Edit/`** module (`ARCANE_API`), consumed by Grimoire. Engine owns the generic capability;
Grimoire owns the editor glue (input, Inspector bracketing, toolbar) — mirrors `PickBuffer` (Arcane) +
Grimoire-consumer.

```
Grimoire Inspector field edit (ImGui gesture)
      │  IsItemActivated  → stack.Begin(label); stack.SnapshotComponent(reg, e, compId)   [before]
      │  (live edit applies via ApplyFloatEdit, unchanged)
      │  IsItemDeactivatedAfterEdit → stack.Commit()                                        [after → push]
      ▼
Arcane::CommandStack ── undo/redo deques of Transactions (depth-capped 100)
      │  Undo(): pop transaction → each ComponentEditCommand.Undo() (reverse) → push to redo
      │  Redo(): pop transaction → each ComponentEditCommand.Redo() (forward) → push to undo
      ▼
Arcane::ComponentEditCommand.Undo/Redo → descriptor->deserialize(before|after) into the LIVE component
      │  (re-resolved by (Entity, ComponentId) each time — robust to archetype moves / deletion)
      ▼
restored LocalTransform → SPEC #1 paused reconcile pushes it to the physics body next frame
```

**Engine types (`Arcane/Edit/`, all `ARCANE_API`):**

| Type | File | Responsibility |
|---|---|---|
| `Arcane::ICommand` | `Command.hpp` | `Undo()` / `Redo()` / `Label()`. The forward edit already happened live, so a command only reverses/replays. |
| `Arcane::ComponentEditCommand` | `ComponentEditCommand.{hpp,cpp}` | One component's before/after blobs, keyed by `(Entity, ComponentId)`. Undo/Redo re-resolve the live instance + descriptor from the registry and `deserialize` the relevant blob. |
| `Arcane::CommandStack` | `CommandStack.{hpp,cpp}` | Undo + redo history of **Transactions**; `Begin/SnapshotComponent/Commit/Cancel`, `Undo/Redo`, `CanUndo/CanRedo`, `Clear`. Depth cap (default 100; oldest dropped). Pushing clears redo. |
| `Arcane::ScopedTransaction` | `CommandStack.hpp` | RAII wrapper (`Begin` in ctor, `Commit` in dtor unless `Cancel`ed) for single-scope edits (gizmo drag-commit, programmatic multi-edit). |

**Grimoire glue:** `GrimoireApp` owns one `Arcane::CommandStack m_undo`; `EditorPanels.cpp` brackets
Inspector gestures; the input block maps `Ctrl+Z`/`Ctrl+Shift+Z`/`Ctrl+Y`; the Sim toolbar shows
enabled-state Undo/Redo buttons.

---

## 5. The reflection command — `ComponentEditCommand`

A command stores `{Entity entity; Astra::ComponentId componentId; std::vector<std::byte> before, after;
std::string label;}`. It does **not** cache the raw component pointer (archetype moves invalidate it).

- **Snapshot** (before, and after at commit): resolve the live instance + descriptor for
  `(entity, componentId)` via the registry's component-inspection seam (`InspectEntity` today; the exact
  Astra getter is pinned at plan time), then `descriptor->serialize(BinaryWriter{blob}, instance)`.
- **`Undo()`**: re-resolve the live instance; if gone (entity/component removed) → **no-op**; else
  `descriptor->deserialize(BinaryReader{before}, instance)`.
- **`Redo()`**: same with `after`.
- **Changed?**: `before != after` (byte compare). A no-op edit is dropped at commit (§6), never pushed.

Whole-component (not field-level) is deliberate: it matches the serialize seam, unchanged fields restore
as no-ops, and components are tiny so the memory cost is negligible (no delta storage — §11).

---

## 6. Transaction model (Unreal `Modify()` + `FScopedTransaction`, ported)

The stack's undo/redo unit is a **Transaction**: an ordered list of `ComponentEditCommand`s plus a label.
A single Inspector field edit is a 1-command transaction; a future gizmo multi-select move is an N-command
transaction — **one undo step** either way.

- `Begin(label)` — open a transaction. No-op-guarded if one is already open (ImGui gestures are exclusive,
  so at most one is open at a time).
- `SnapshotComponent(registry, entity, componentId)` — the `Modify()` analog: if this `(entity,
  componentId)` is **not yet** in the open transaction, capture its `before` blob now (**idempotent** —
  later touches of the same component in the same transaction don't re-snapshot). MUST be called before
  the live edit mutates the component (see §7 ordering).
- `Commit()` — for each snapshotted component, capture its `after` blob; drop unchanged commands; if any
  survive, push the transaction onto the undo deque and **clear the redo deque**; if none changed, discard
  (no history entry). Closes the open transaction.
- `Cancel()` — discard the open transaction **without** pushing (does not auto-revert live state; ImGui's
  own Escape-to-cancel restores widget values). For programmatic aborts.
- `Undo()` — pop the top undo transaction; call `Undo()` on its commands in **reverse** order; move it to
  the redo deque. `Redo()` — mirror (pop redo, `Redo()` forward, push to undo).

`ScopedTransaction` is the RAII form for single-scope call sites (`Begin` in ctor; `Commit` in dtor unless
`Cancel`ed). The Inspector uses the **explicit** `Begin`/`Commit` because its gesture spans frames
(mouse-down frame → mouse-up frame), which an RAII scope can't hold.

---

## 7. Inspector edit capture (one undo op per gesture)

The visitor gains gesture bracketing around the existing live-apply, using ImGui item state. Per field
widget, in order:

1. `bool changed = ImGui::DragFloat(...)` (or sibling) — ImGui updates its internal state.
2. `if (ImGui::IsItemActivated())` → `m_undo.Begin("Edit <Type>.<field>")`;
   `m_undo.SnapshotComponent(reg, entity, componentId)`. **Before `ApplyEdit`**, so the snapshot reads the
   pre-edit component value (correct even in a same-frame click+drag, since `ApplyEdit` hasn't run yet).
3. `if (changed) Grimoire::ApplyFloatEdit(f, instance, v);` — unchanged live apply.
4. `if (ImGui::IsItemDeactivatedAfterEdit())` → `m_undo.Commit()`.

A whole drag → one `Begin`…`Commit` → one transaction with one `ComponentEditCommand`. `DragFloat2`/
`DragFloat3` edit a whole `vec2`/`vec3` field in one gesture → still one command (whole-component
snapshot). A click that doesn't change the value → `Commit()` finds `before == after` → discards.

`DrawInspectorPanel` (and the visitor) gain a `CommandStack&` + the selected `Entity`; the visitor already
has the `ComponentInfo` (descriptor + componentId) it needs to key the snapshot.

---

## 8. Keybinds + UX

- Grimoire input block: `Ctrl+Z` → `m_undo.Undo()`, `Ctrl+Shift+Z` **or** `Ctrl+Y` → `m_undo.Redo()`.
  Gated on **Edit mode** (not Play) and suppressed while an ImGui text field has keyboard focus
  (`WantCaptureKeyboard` for a text input) so typing "z" in a `text_input` isn't an undo.
- Sim toolbar: Undo / Redo buttons, enabled from `CanUndo()` / `CanRedo()`, tooltip showing the top
  transaction's label. (Reuses `DrawSimTimeToolbar`.)

---

## 9. Edge cases / decisions

- **Play/Stop**: undo/redo is **Edit-mode only**. Entering **Play clears the stack** (`m_undo.Clear()`):
  Play snapshots the registry and Stop restores it, so edit history across that boundary is meaningless.
- **Deleted entity**: `ComponentEditCommand::Undo/Redo` re-resolve the instance and **no-op** if the
  entity or component is gone. (A later spec may prune such transactions; v1 just skips.)
- **Redo invalidation**: any `Commit()` clears the redo deque (standard).
- **Depth cap**: 100 transactions; the oldest is dropped when exceeded (`UndoManager` parity).
- **No-op skip**: `before == after` commands are dropped; an all-no-op transaction is not pushed.
- **"PostEditUndo"**: restoring a component is a plain `deserialize` into the live instance. Derived-state
  systems that **poll** the component pick the change up automatically — the SPEC #1 transform reconcile
  is stateless/polling, so a restored `LocalTransform` (incl. scale → fixture rebuild) reflects to the
  body next frame. This gives Unreal's `PostEditUndo` behavior with no notification plumbing. Components
  with no polling consumer (e.g. a future one needing an explicit rebuild) are out of scope here; undo has
  **parity** with the forward Inspector edit (both are plain component writes).

---

## 10. Testing

- **Headless Arcane unit tests** (`Arcane/Tests/src/CommandStackTest.cpp`, tag `[edit]`; cross-DLL
  `TypeContext` pin like the pick tests, since `ComponentEditCommand` resolves component ids in Arcane.dll):
  - `CommandStack`: push/undo/redo, `CanUndo/CanRedo`, depth-cap eviction, redo-cleared-on-new-commit,
    empty-transaction discard.
  - `ComponentEditCommand` round-trip: snapshot a reflected component (e.g. `LocalTransform`), mutate,
    `Undo()` restores the before value byte-exact, `Redo()` restores after. Deleted-entity → no-op.
  - **Transaction grouping**: a transaction over two components → one `Undo()` reverts both; idempotent
    `SnapshotComponent` (two touches of one component in a transaction → one command).
- **Headless Grimoire-path test**: drive `Begin → SnapshotComponent → (mutate) → Commit → Undo/Redo`
  against a registry **without** ImGui, proving the capture→command→restore path end-to-end.
- **Interactive** (desk): edit a field in the Inspector, `Ctrl+Z` reverts it (and, for a transform, the
  body follows), `Ctrl+Y` re-applies; toolbar buttons enable/disable correctly; Play clears history.

---

## 11. Non-goals / deferred

- **Gizmos** — the next spec; they push the same `ComponentEditCommand`/transaction into this stack.
- **Field-level granularity / delta storage** — whole-component blobs; components are tiny.
- **Cross-gesture coalescing / merge** (e.g. debouncing rapid keystrokes into one op) — one op per gesture
  is enough; merge is a later refinement.
- **Add/remove-component and create/destroy-entity commands** — this spec covers **value edits** of
  existing components; structural edits are a later command type on the same stack.
- **JSON / human-readable history, persistent (on-disk) undo, multiplayer / selective undo** — out of scope.
- **Auto-revert on `Cancel`** — Cancel discards the record only (ImGui handles widget-value cancel).

---

## 12. Impl-time verification points (shape unaffected; resolve while planning)

1. **Component resolution API** — the exact Astra call to get a component's `{descriptor, void* instance}`
   by `(Entity, ComponentId)` for snapshot/restore (today via `Registry::InspectEntity`; confirm a direct
   getter + the `ComponentId` type the Inspector already has on `ComponentInfo`).
2. **`BinaryWriter`/`BinaryReader` construction** — how to build them over a `std::vector<std::byte>` blob
   for a single-component serialize/deserialize (the `Registry::Save` path is the reference).
3. **ImGui gesture signals** — confirm `IsItemActivated` fires before the first value change for the
   `DragFloat*` widgets used, and that `IsItemDeactivatedAfterEdit` is the right commit signal for each
   (including `text_input` / `Checkbox`, whose gesture shape differs from a drag).
4. **Keybind suppression** — the precise `WantCaptureKeyboard` / active-text-item check so `Ctrl+Z` while
   editing a `text_input` does ImGui's in-field undo, not a stack undo.
