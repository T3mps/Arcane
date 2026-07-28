# Editor Entity Identity + Rename Rethink — Design

**Date:** 2026-07-27
**Branch:** `arcane-entity-rename`, cut from `arcane-inspector-polish` @ `182722b0`
(this design depends on the Inspector polish arc: `FieldKind`, the attribute
plumbing, and the `Astra::ReadOnly` annotation on `EntityInfo::id` all live there).

## Goal

Three asks, one root cause:

1. The `EntityInfo` component is invisible in the Inspector (filtered by
   `IsSystemManagedComponent`, `ComponentCatalog.cpp:44`).
2. `EntityInfo::name` is not editable anywhere in the Inspector — `std::string`
   has no `FieldKind`, so every string field renders as a disabled
   "(unsupported)" row.
3. Entity rename in general is janky: the Outliner's F2 is the only rename
   path, and it is built on the wrong machinery.

Deliverable: `EntityInfo` visible; `name` editable from both the Inspector and
the Outliner through ONE mechanism; `id` view-only; generic string editing for
every component; the rename jank retired.

## What is actually wrong today (verified at source)

| # | Jank | Where |
|---|---|---|
| 1 | Rename is a **field edit wearing structural clothing**: it snapshots and restores the ENTIRE registry (`ApplyStructural` -> `RegistryStateCommand`) to change one string. Because whole-registry mementos are refused inside an open gesture (`RegistryStateCommand.cpp:59-62`), that miscategorization is precisely why the Inspector cannot reuse the path. | `EditorPanels.cpp:644` |
| 2 | `renameBuf` is a fixed `char[]` — silent truncation; the model is `std::string`. | `OutlinerState` |
| 3 | `commit = commit \|\| !ImGui::IsKeyPressed(ImGuiKey_Escape)` — a GLOBAL key query standing in for "was this cancelled". | `EditorPanels.cpp:640` |
| 4 | `RenameEntity` **mints a fresh Guid** when `EntityInfo` is missing — renaming creates durable identity as a side effect. | `EntityOps.cpp:187` |
| 5 | Names are uniquified at creation (`AutoEntityName`) but rename allows collisions — an unstated rule that looks like an accident. | `EntityOps.cpp:34-47` |
| 6 | `IsSystemManagedComponent` conflates "users cannot add/remove this" with "do not show this". | `ComponentCatalog.cpp:39-45` |

## What Unreal does (read from the vendored tree, not memory)

- **Identity is intrinsic, not a component.** `ActorGuid` and `ActorLabel` are
  fields on `AActor` itself (`Actor.h:1055`, `:1188`). "Removable" is not a
  concept. The label's own comment: "Never set the label directly."
- **`ActorGuid` is `NonTransactional`** (`Actor.h:1054`) — identity does not
  participate in undo.
- **Rename is a normal transacted edit**, not a world snapshot: the outliner's
  `OnLabelCommitted` wraps `FActorLabelUtilities::RenameExistingActor` in an
  `FScopedTransaction` (`ActorTreeItem.cpp:263-277`).
- **Uniqueness is a creation-time policy, not a rename-time one:**
  `SetActorLabelUnique` callers are creation-shaped (ActorFactory spawn,
  copy-paste, duplication — `EditorEngine.cpp:6154`, `ActorFactory.cpp`,
  `EditorActor.cpp`); `RenameExistingActor` does a plain `SetActorLabel`
  (`EditorEngine.cpp:6193-6205`). Duplicates are legal on rename.
- **Commit/cancel comes from the widget**, not global key state:
  `OnTextCommitted(ETextCommit::Type)` + `OnVerifyTextChanged`
  (`ActorTreeItem.cpp:60-61`).
- **Transform is optional** (actors without a root component are non-spatial) —
  which Arcane already mirrors deliberately (`ComponentCatalog.hpp:48`).

## Section 1 — Semantics

- **`EntityInfo` is identity**, the ECS equivalent of `AActor`'s intrinsic
  fields. Invariant: every editor-created or scene-loaded entity has one
  (already true at every creation path). Never addable or removable in the UI.
  Now always VISIBLE in the Inspector.
- **Rename never mints identity.** `Guid::Generate` happens only at creation.
  `Edit::RenameEntity` requires an existing `EntityInfo`; if the entity is
  invalid, lacks the component, or the name is unchanged, it returns false and
  mutates nothing. The `AddComponent` branch (`EntityOps.cpp:187-189`) is
  deleted.
- **Entities without `EntityInfo` are not renameable** (runtime plugin spawns
  only — UE's analogue, Mass entities, have no labels either). They keep the
  "Entity <id>" display; F2 and the Rename menu item disable with a tooltip
  saying why.
- **Uniqueness = UE's policy, which is already Arcane's:** unique auto-names at
  creation, duplicates allowed on rename. This becomes a STATED rule (comment
  on `RenameEntity`), not an accident.
- **Empty name is legal** and keeps falling back to "Entity <id>" in every
  display path (`DisplayName`, `EntityOps.cpp:49-55`, unchanged).
- **Transform stays removable.** Both engines agree non-spatial entities exist.

## Section 2 — Undo mechanism

Rename becomes a **component edit**: one `ComponentEditCommand` on `EntityInfo`.

- This is safe for `std::string` because `ComponentEditCommand::Snapshot` is a
  SERIALIZED blob, not a raw byte copy — it round-trips through
  `descriptor->serialize`/`deserialize` (`ComponentEditCommand.cpp:23-49`).
  Verified this session; the byte-unsafety that killed the reverted
  reset-to-default feature does not apply here.
- The Outliner's F2 commit becomes: `Snapshot(e, entityInfoDescriptor)` +
  `Edit::RenameEntity` inside a transaction — the same shape as the Inspector's
  single-shot `ApplyImmediate`. `CommandStack::Commit` already drops unchanged
  components before pushing a step, so a no-op rename produces no undo entry
  with no extra guard.
- **REVISED 2026-07-27 (whole-branch review, finding I1).** This section
  originally specified a bare `ScopedTransaction` at the commit site and
  described it as joining an open transaction *safely*. **That claim was
  false.** A joined scope contributes pending snapshots and then rides the
  OWNER's close: if the owner ends in `Cancel` — which an Inspector field
  gesture does on a plain activate-then-release with no edit — `CommandStack::Cancel`
  discards the pending snapshots **without reverting** (`CommandStack.cpp:75-82`).
  The rename would land and be permanently un-undoable.
  The commit site therefore calls the engine-side
  `Edit::RenameWithUndo(stack, reg, e, name)` (`EntityOps.hpp`), which **opens
  its own transaction or refuses**: it returns `RenameResult::Deferred`, having
  mutated nothing, whenever `stack.InTransaction()`. The panel parks the
  (entity, name) pair in `OutlinerState::pendingRename` and retries at the top
  of the next `DrawOutlinerPanel`, before rows are built; any result but
  `Deferred` consumes the slot, and starting a new rename clears it so a stale
  parked rename cannot land after the user has moved on. `RenameWithUndo` also
  owns the `EntityInfo` descriptor lookup, so the undo shape lives in one
  headless-testable place instead of per panel.
- `ApplyStructural(undo, binding, "Rename", ...)` (`EditorPanels.cpp:644`)
  dies. Rename is no longer routed through the whole-registry memento and no
  longer special-cased by `CanEditStructure`. It is still *deferred* while a
  gesture is open (above) — but deferred-and-retried, not refused-and-lost, and
  for a reason specific to the undo mechanism rather than to structural edits.
- The whole-registry memento machinery (`RegistryStateCommand`) remains for the
  operations that are genuinely structural: create, delete, reparent,
  add/remove component, hide.

## Section 3 — Inspector

**Generic string editing** (no `EntityInfo` special case anywhere):

- `FieldKind::String`, keyed on `Astra::TypeID<std::string>::Hash()` in
  `ClassifyField` — the existing idiom (`InspectorFields.cpp:30-42`).
- `ApplyStringEdit(f, instance, const std::string&)` write-back via
  `f.GetPtr<std::string>`, headless-tested like the other `Apply*Edit`s.
- `FieldComponentCount(String) == 1`. `ComputeFieldMixed` gains a typed
  `std::string` comparison (its scalar comparison cannot be reused).
- **Commit-on-deactivate, not per-keystroke:** a string row edits a scratch
  buffer and applies ONCE on Enter / deactivate-after-edit through the existing
  single-shot `ApplyImmediate` (snapshot + apply + commit). No mid-edit
  component mutation, no gesture bracketing needed, and it matches UE's
  `OnTextCommitted` model. An equality guard skips the apply when the value is
  unchanged.
- **Multi-select follows the panel's own convention** — blank when mixed,
  commit fans out to every selected entity. Deliberate divergence from UE
  (which disables the label on multi-select): duplicates are legal here, the
  fan-out rule is uniform across all field kinds, and one rule beats two.

**`EntityInfo` becomes an ordinary section:** `name` is just an editable string
field; `id` is already `Astra::ReadOnly` (annotated in the polish arc) and
renders disabled. No `Category` attributes — a single homogeneous group needs
none (the polish arc's Fix-5 lesson).

**Why the two rename entry points cannot drift:** the Inspector's generic
string path and the Outliner's `Edit::RenameEntity` both reduce to the same
mutation (`info->name = x`) bracketed by the same undo shape (one
`ComponentEditCommand` on `EntityInfo` inside a single-shot transaction), with
the same equality guard. `RenameEntity` remains the engine-level op for
programmatic and Outliner use; the Inspector needs no knowledge of it.

**The predicate split (jank 6):** `IsSystemManagedComponent` becomes two
predicates with the three call sites reassigned:

| Predicate | Members | Consumed by |
|---|---|---|
| `IsHiddenInInspector` | `WorldTransform`, `PreviousTransform`, `PhysicsBodyRef` (derived per-frame caches) | the Inspector's display gate |
| `IsStructureLocked` | the hidden set + `EntityInfo` | Add Component catalog, Remove Component menu |

`ComponentCatalog.hpp`'s per-entry WHY comment block is updated to match.

## Section 4 — Outliner rename UX

- **`std::string` buffer** (kills the truncation). Use the vendored
  `imgui_stdlib` `InputText(std::string*)` if present; otherwise a small
  resize-callback helper.
- **The global-Escape check dies via a simplification:** ImGui's `InputText`
  natively restores the original buffer on Escape before deactivating, so
  "always commit on deactivate" makes Escape a natural no-op — the unchanged
  name hits `RenameEntity`'s equality guard and produces no undo step.
  **This native-revert claim is load-bearing vendored behaviour and MUST be
  verified in `imgui_widgets.cpp` during implementation** (this project's
  recurring failure class is comments citing doc claims the implementation
  contradicts). Fallback if it does not hold exactly: keep the original string
  and compare at commit — same observable behaviour, one extra member.
- F2 / double-click entry points, focus handling (`SetKeyboardFocusHere` +
  `AutoSelectAll`), and inline placement are unchanged.
- Rename affordances (F2, context-menu Rename) disable with a tooltip when the
  entity lacks `EntityInfo`.

## Section 5 — Explicitly unchanged

`AutoEntityName`; `DisplayName` and its "Entity <id>" fallback; the Inspector's
selection header; Transform removability; multi-select intersection and
mixed-value machinery; `id` stays `Astra::ReadOnly`; the structural-memento
path for genuinely structural ops.

## Section 6 — Testing

**Headless (engine + pure editor TUs, all gate-covered):**

- `RenameEntity`'s NEW contract: false on invalid entity, false when
  `EntityInfo` is missing (and no component added — **this inverts the existing
  test** "RenameEntity adds EntityInfo when missing", `EntityOpsTest.cpp:172`,
  which must be rewritten, not appended to), false + no mutation on unchanged
  name, true + rename otherwise.
- Rename undo/redo through `CommandStack` as a `ComponentEditCommand` on
  `EntityInfo`, including an SSO-defeating long name (a fixture already exists,
  `EntityIdentityTest.cpp:105`) and undo restoring the exact prior name.
- A no-op rename inside a transaction pushes no undo step.
- `Edit::RenameWithUndo` (added by the 2026-07-27 review fix): `Renamed` pushes
  exactly one `"Rename"`-labelled entry that undo/redo round-trips; `NoChange`
  pushes none; `Deferred` is returned — and NOTHING is mutated — while another
  transaction is open, with the retry succeeding once it closes either way;
  `Invalid` for a dead entity and for one carrying no `EntityInfo`, mutating
  nothing and opening no transaction.
- `ClassifyField` maps `std::string` to `FieldKind::String`; `ApplyStringEdit`
  round-trips; `FieldComponentCount(String) == 1`; `ComputeFieldMixed` on
  strings (same / different / entity-lacking-component).

**Desk-verify (the panel is uncoverable by construction):** rename via
Outliner F2, via the Inspector name field, Escape cancels both with no undo
entry, undo/redo of each, multi-select mixed-name blanking + fan-out, rename
attempts on a plugin-spawned entity (disabled + tooltip), play-mode rename
behaves like any field edit.

## Plan-time verification items

Facts this design relies on that must be re-verified against source during
implementation, not trusted from here:

1. ImGui `InputText` Escape-revert semantics, and whether
   `IsItemDeactivatedAfterEdit` fires after an Escape revert
   (`imgui_widgets.cpp`).
2. Whether `misc/cpp/imgui_stdlib.h` is in the vendored ImGui.
3. The editor-side accessor for `EntityInfo`'s `ComponentDescriptor` (the
   Outliner needs one outside the Inspector's `InspectEntity` loop).
4. `ComputeFieldMixed`'s internals before extending it for strings.
5. That `CommandStack::Commit` drops unchanged components (asserted by the
   gesture-leak fix's test; cite the test, not the report).
