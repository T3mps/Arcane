# Editor Outliner: UE-shape hierarchy + EntityInfo/Hidden + multi-edit + Add Component

**Status: design.** The Hierarchy panel is a flat `Entity 42 (v1)` list; entities
have no names, no visibility toggle, no way to be created, deleted, reparented,
or given/denied components from the editor. This arc rebuilds it into an
Unreal-Outliner-shaped panel and adds the two small engine components it needs
(a UE5 Outliner screenshot is the visual reference).

Decided in brainstorm (do not re-litigate): all four interactive features are
in scope (eye, create/delete, drag-reparent, multi-select); hiding is a
`Hidden{}` marker component (not a SpriteRenderer field, not an editor-side
set); the identity component is named **EntityInfo** (not Tag — "tag
component" stays free for empty markers); multi-select is **full multi-edit**
(gizmo group transforms + Inspector fan-out + multi-outline), not list-only.

## 1. Components (engine)

```cpp
struct EntityInfo            // editor-facing identity
{
    Guid        id{};        // stable identity; generated when the component is added
    std::string name;        // display name; empty falls back to "Entity <id>"
    template<typename Ar> void Serialize(Ar& ar) { ar(id); ar(name); }
};
struct Hidden {};            // marker: render submission skips the entity
```

- Both reflected + registered in `RegisterSceneComponents` and the in-tree
  plugin rosters (Sandbox, PlaygroundGame); Aphelyon adds them at its next
  desk rebuild. Scene JSON is free (string fields + nested Guid both already
  supported); the binary path (Play snapshots, SaveBinary) uses EntityInfo's
  `Serialize` member — Astra's `HasSerializeMethod` seam exists for exactly
  this. `Hidden` serializes as presence-only (verify the zero-field JSON path
  in tests).
- `RenderSubmissionSystem` skips entities carrying `Hidden` (view exclusion
  filter if Astra has one, else a per-entity has-check — planning verifies).
  The system is plugin-compiled: in-tree plugins ride the workspace build.
- **Policy:** the EDITOR adds EntityInfo (create + first rename); runtime
  spawns are never forced to carry strings. Auto-names: `Entity`,
  `Entity_2`, ... (uniqueness scan over existing EntityInfo names at create).
- **ABI:** two appended name-keyed component types + a behavior change in a
  plugin-compiled system. Same argument as PostProcess — no layout change, no
  vtable change, expected NO bump; record in the PluginABI.hpp ledger and
  verify with the tripwires. A stale plugin simply doesn't honor `Hidden`.

## 2. Structural undo commands (engine, `Arcane/Edit/`)

The primitives every later slice uses, on the existing CommandStack +
registry-resolver seam, all headless-testable:

- **CreateEntity** (optional parent; adds EntityInfo w/ auto-name). Undo
  destroys; redo must resurrect the SAME entity id (later stack entries
  reference it).
- **DeleteEntities** (the selection set): captures every component per
  descriptor (binary serialize), parent links, and child lists. Children of a
  deleted entity splice up to its parent. Undo restores ids, components,
  links. Multi-delete handles nested selections (deepest-first).
- **Reparent** (set → new parent; null = root). Refuses cycles.
- **Add/RemoveComponent** (entity set + descriptor; remove captures bytes).
- **SetHidden** (entity + descendants, add/remove `Hidden`; one step).
- **RenameEntity** (add-EntityInfo-if-missing + set name; one step).

**Risk resolved at planning:** exact-id resurrection comes free from binary
registry restore (`Registry::Save/Load`, surfaced as
`Runtime::SnapshotRegistry/RestoreRegistry`). The six bespoke commands
collapsed into pure mutators (`Arcane/Edit/EntityOps`) + ONE whole-registry
memento (`RegistryStateCommand` via `ApplyRegistryMutation`); the CommandStack
resolver already survives the registry swap. Snapshot size per structural op
is the whole scene (editor-scale; acceptable; revisit only if profiling says
so).

## 3. The Outliner panel (editor)

Pure-core + ImGui-shell, like AssetBrowser:

- **`BuildOutlinerRows(registry, filter, sort)`** (EntityList.hpp, headless-
  tested): flat, depth-annotated rows in tree order from the relationship
  graph, plus roots-without-parents; case-insensitive substring filter over
  display names — matches AND their ancestors stay visible (non-matching
  ancestors render dimmed); per-row: entity, depth, label, hidden flag,
  child count. Label = EntityInfo.name, else `Entity <id>`.
- **AMENDED 2026-07-25 (USER DIRECTIVE, post-slice-2): there is NO type
  column.** It was built and then removed: every row IS an entity, and
  entities are *composed of* components rather than being of a type, so a
  column labelling one row "Sprite" and another "Rigid Body" asserted a
  taxonomy the ECS does not have. Removed with all related logic (the
  priority table, `OutlinerRow::type`, and sort-by-type). Do not reintroduce
  it in a later slice; component composition belongs in the Inspector.
- **`DrawOutlinerPanel`**: `ImGui::BeginTable` (RowBg striping, sortable
  Label header, frozen header row). Column 0 = eye icon
  (ICON_LC_EYE / _OFF, dimmed when off); column 1 = TreeNodeEx arrow + label.
  Footer: `N entities (M selected)`. Search box above the
  table. Renaming = F2 or slow-double-click → inline InputText in the row.
- **Interactions:** click selects (plain = replace, Ctrl = toggle, Shift =
  range over visible row order); right-click row → New Child Entity / Rename /
  Delete / Add Component...; right-click empty → New Entity; drag rows onto a
  row = reparent set, onto empty = unparent (cycle-refused); eye click =
  SetHidden on entity + descendants; Delete key deletes the selection.
  Everything routes through §2 commands.

## 4. Full multi-edit

- **SelectionContext** → ordered set + primary (last-clicked). Every current
  `.selected` consumer updates (EditorApp pick/gizmo/outline/Inspector).
  Viewport: Ctrl+click toggles the picked entity; plain click replaces;
  Alt-cycle keeps operating on the primary.
- **Gizmo:** anchors at the primary's pivot. Deltas apply to **selection
  roots only** (selected entities with no selected ancestor — prevents
  double-transform through propagation). Translate = shared delta; rotate/
  scale = true group transform about the primary's pivot (positions orbit /
  scale, rotations += / scales *=; 2D mat3). One drag = one undo step
  snapshotting every touched Transform.
- **Inspector:** shows the component-type intersection across the set
  (header: `<primary name> (+N)`); widgets display the PRIMARY's values; a
  field edit fans that field out to every selected entity's component —
  gesture = Begin → snapshot each → apply → Commit. Mixed-value dashes are a
  follow-up, not this arc.

**Follow-up BUILT 2026-07-26 (Inspector multi-select UE parity).** The two
deferred behaviours landed; plan at
`docs/superpowers/plans/2026-07-26-inspector-multiselect-ue-parity.md`. All
three rules were read out of the vendored UE 5.6 source
(`Engine/Source/Editor/DetailCustomizations/Private/ComponentTransformDetails.cpp`),
not recalled:
- `:505`/`:551`/`:628` — `.AllowSpin(SelectedObjects.Num() == 1)`: UE gives a
  multi-selection **no spinner at all**. It does not buffer a drag; the widget
  simply becomes type-and-commit. We mirror that with per-component text entry.
- `:1248` — `if (!bCommitted && SelectedObjects.Num() > 1) return;`, commented
  *"Ignore interactive changes when we have more than one selected object"*.
  Both belts, so we wear both too.
- `:1215-1225` — mixed values are **per-axis and sticky**: the first carrier
  seeds, each later one clears any axis that differs, and the
  `&& Cached<X>.IsSet()` term means a cleared axis never returns. `:1026`
  states the convention: *"unset means multiple differing values"*.
Ours lives in `Arcane::Editor::ComputeFieldMixed` (pure, headless-tested) plus
`MultiScalarRow` in the visitor. `ImGuiItemFlags_MixedValue` is Checkbox-only in
our vendored ImGui (`imgui_internal.h:984`), so Bool gets the native tri-state
and numerics blank by hand. Single-selection behaviour is unchanged.
- **Outline:** the seed pass gets a CB array of up to **64** selected ids
  (membership loop in-shader); hovered stays single. Beyond 64: first 64 +
  one-time log. SelectionOutline's signature changes — editor/tests are its
  only consumers (verified: Sandbox doesn't touch it).

## 5. Add/Remove Component UI (Inspector)

- `+ Add Component` button (bottom of the Inspector) → searchable popup over
  the ComponentRegistry's descriptors minus a curated **system-managed
  hide-list** (`WorldTransform`, `PreviousTransform`, `PhysicsBodyRef` —
  derived state, never hand-added). Adds default-constructed component to
  every selected entity lacking it (one undo step).
- Per-component header context menu → Remove Component (same hide-list
  protects system-managed ones from removal; everything else, Transform
  included, is removable — the user asked for it by name).

**Slice-4 amendments (2026-07-25, as built):**
- The hide-list is ONE predicate, `Arcane::Editor::IsSystemManagedComponent`
  (`ArcaneEditor/src/ComponentCatalog.hpp`). The Inspector's section filter, the
  Add catalog, and Remove Component all consult it, so they cannot drift.
- Unreflected components are omitted from the catalog: the Inspector can only
  render reflected types, so adding one would be an invisible edit the header
  menu could never remove.
- Tag (empty) components now DO get an Inspector header, showing
  "(tag component -- no fields)". Without one they were invisible and therefore
  unremovable. The multi-select intersection test switched from
  `GetComponentByHash` to `HasComponentByHash` for the same reason — the getter
  returns null for a tag component the entity really carries.
- A catalog row whose `missingCount` is 0 (every selected entity already has it)
  renders disabled rather than hidden.
- Removal is deferred past the `InspectEntity` loop: the archetype move dangles
  every `ci.data` pointer in the vector being iterated.
- `OutlinerBinding` was renamed `SceneEditBinding` — two panels share it now.

## 6. Slices

1. **Components + commands** — §1 + §2 + tests (EntityInfo JSON/binary
   round-trip incl. Play-snapshot path, Hidden presence round-trip +
   submission skip, every command round-trips through undo/redo, auto-name
   collisions). Pure engine; ships alone.
2. **Outliner panel** — §3 on top of S1. SelectionContext becomes set+primary
   HERE (list ops need it); every existing consumer switches to a Primary()
   accessor mechanically, keeping single-entity behavior until S3 makes it
   multi. Headless row-builder tests; panel behavior desk-verified.
3. **Full multi-edit** — §4 (gizmo group math headless-tested; outline CB
   change rides the existing [gpu] outline suites + RenderErrorCount).
4. **Add/Remove Component** — §5 + headless tests over the descriptor
   filtering + command reuse.

Estimated 3–4 sessions. Each slice lands green independently.

## Non-goals (this arc)

- Outliner folders (the parent hierarchy IS the tree; UE folders are an
  organizational overlay we don't need yet).
- Mixed-value display in multi-edit widgets; multi-rename with numbering.
- Editor-only-vs-game visibility split (`Hidden` is one serialized bit;
  UE's bHiddenEd/bHidden distinction can layer on later if wanted).
- Unreal's pin/star columns; per-type row icons (and the type column itself,
  removed 2026-07-25 -- see the amendment in section 3).
- Outlining more than 64 selected entities.
