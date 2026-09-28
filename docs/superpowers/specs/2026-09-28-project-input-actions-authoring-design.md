# Project Input Actions Authoring and Rebinding Design

**Date:** 2026-09-28
**Status:** Proposed for user review
**Extends:** [Arcane Input Actions design](../../specs/2026-06-12-arcane-input-actions-design.md)

## Purpose

A game developer should be able to create and edit gameplay action maps in
Arcane Editor, then query those actions and offer runtime rebinding from game
code. The ReferenceProject should demonstrate this path with its platformer
controller. The existing engine `input.json` remains editor/host configuration:
it is not a gameplay asset, does not appear in the Asset Browser, and is not
merged into project gameplay actions.

This is a Unity-style asset and runtime-user model. Arcane's existing
`InputActions` evaluator already supports maps, actions, simple and chorded
bindings, composites, processors, interactions, and context ordering. This
design adds project asset ownership, complete editor authoring, a game-facing
runtime session, and non-destructive per-user binding overrides. The older
input design's deferred rebinding and persistence items are superseded by this
document; its evaluator semantics remain in force unless explicitly changed
below.

## Scope and Boundaries

- One project-designated gameplay Input Actions asset, containing any number
  of maps, actions, bindings, and control schemes. The editor may create other
  assets of this kind, but only the designated one is active for the project.
- One active `LocalInputUser` per running game instance for now. Its model must
  leave room for separate users and device assignment later, without exposing
  a premature local-multiplayer UI or promising simultaneous gamepad support.
- A public game-module API for typed action queries, map activation, binding
  inspection and capture, overrides, and profile persistence. Each game builds
  its own controls-screen UI using that API.
- A native editor for the complete gameplay asset, including control schemes,
  binding capture, conflict feedback, and live preview.
- Migration of the ReferenceProject platformer controller to gameplay actions.

Out of scope: an editor-shortcut settings UI; changing the editor camera or
host shortcut behavior; a built-in game controls screen; input recording or
replay; multiple simultaneous local input users; assigning individual physical
devices to users; network replication of input; and automatic migration of
external projects. A project without a designated gameplay asset remains
valid, with no game actions available.

## Ownership and Data Model

| Data | Owner and location | Edited by | Runtime use |
|---|---|---|---|
| Editor/host input defaults | `data/EngineConfig/input.json` and existing config layers | Future editor settings UI | Host/editor only |
| Gameplay action definitions | Native `.arcinput` JSON asset under project `Content/` | Input Actions editor | Game input session |
| Project selection | Input asset GUID in `.arcproj` | Project Settings | Finds active gameplay asset |
| Personal binding overrides | Versioned profile in a writable per-user app-data location, namespaced by project GUID and profile name | Game code/API | Applied over the asset without modifying it |

The `.arcinput` asset is one versioned JSON document with a top-level asset
`id`, `actionMaps`, and `controlSchemes`. A map contains actions and their
bindings, preserving the current `InputActions` evaluation schema and
semantics. Map, action, binding, and composite-part IDs are stable GUIDs;
display names and control paths are mutable. References and saved overrides
use IDs, never names or array indexes. Renaming and reordering therefore do
not invalidate game references or personal overrides. A duplicate ID or
invalid reference is a validation error. The editor creates IDs on insertion
and preserves them on edits; duplication creates fresh IDs for the copied
subtree.

The asset supports Button, 1D value, and 2D value actions. Bindings include
simple control paths, `+` chords, 1DAxis and 2DVector composites and their
named parts, processors, interactions, and optional control-scheme groups.
Control schemes identify compatible binding groups (initially keyboard/mouse
and gamepad), not separate copies of maps. Scheme filtering must be implemented
in the evaluator; merely displaying scheme metadata in the editor is not
sufficient. Existing path and processor syntax remains accepted. Asset
versioning allows future schema changes; the loader can translate the current
unversioned `actionMaps` JSON shape, but the editor writes only the native
versioned format. The engine `input.json` is not converted or imported.

The asset registry recognizes `.arcinput`, reads its embedded GUID, and treats
it as a native JSON asset. Asset Browser can create, rename, move, select, and
open it. Project Settings selects the project-wide asset by GUID, so moving or
renaming it does not break the selection. A missing GUID target is reported
clearly, never silently replaced with another asset.

## Editor Experience

The supplied `Player+Action+Map.webp` is the layout reference. The primary
view has **Action Maps** on the left and **Actions** on the right. The right
side is an expandable hierarchy: action rows contain binding rows; composite
bindings contain named part rows (for example, `Move > WASD > Up: W`, `Down:
S`, `Left: A`, `Right: D`). Device labels such as `[Keyboard]` and
`[Gamepad]` appear on binding rows. Selection, add, remove, duplicate, rename,
and reorder work at the appropriate hierarchy level. The currently selected
row's properties appear in a docked inspector beside or below this tree; they
must not replace the action/binding hierarchy. This keeps the reference image's
scanability while providing room for detailed properties.

The inspector edits action type, binding path, composite kind/part,
processors, interactions, control-scheme membership, and map behavior
(including blocking/context priority). A control picker offers keyboard,
mouse, and gamepad paths; capture can instead listen for the next eligible
control, with cancel, timeout, and explicit device filtering. Capturing a
binding in the editor changes the project asset, whereas runtime capture
changes a personal override. The editor shows human-readable display strings
alongside canonical paths.

The asset editor exposes control schemes and a live preview of the selected
map/action: current value, phase, active device, and the binding that supplied
the value. It highlights invalid paths, duplicate IDs, incompatible
composite parts, overlapping bindings in the same active scheme/context, and
unreachable bindings. Warnings may be acknowledged, but schema errors block
Save/Publish. Undo/redo includes hierarchy and property edits. Save writes
atomically, then publishes a validated compiled definition to a running
preview at a frame boundary; a failed save or compile leaves the last usable
definition active. Ordinary games need not be running to author the asset.

Project Settings gains the project-wide Input Actions asset picker. The
existing Editor Preferences placeholder and engine shortcut settings remain
outside this work. Editor camera WASD and host shortcuts continue to work
independently of the selected gameplay asset.

## Runtime Architecture and API

The host samples one fixed-size `InputSnapshot` as it does today. Host/editor
actions and gameplay actions are separate evaluator instances: gameplay asset
loading never deep-merges with `Config` category `input`, and gameplay context
changes cannot disable editor shortcuts. The host filters input according to
the active game viewport's capture/focus policy before forwarding the snapshot
to the game session. It must not let typing in editor UI trigger gameplay
actions, or suppress game controls solely because an unrelated editor widget
exists. Gamepad input follows the same focus policy as keyboard/mouse unless
the game has explicitly captured the viewport.

`LocalInputUser` owns one evaluated copy of the project action definition,
active map/context stack, active control scheme, transient interaction state,
and one personal override profile. The initial sampler provides keyboard,
mouse, and the first connected gamepad only. The public user identity and
session boundary should not be tied to ECS entities or network players; a
later device router can feed separate snapshots to separate users without
changing action assets or profile format. Project open/close and game restart
create or reset the session deliberately; hot reload of a valid asset
preserves the active maps and compatible overrides by stable ID, and resets
interaction state where the changed binding requires it.

The game-module-facing `ClientRuntime` exposes the current `LocalInputUser`
through a stable SDK interface. The proposed operations are:

- Resolve an action by stable ID or map-qualified name; typed read of Button,
  1D, or 2D value and Started/Performed/Canceled/Pressed/Released phase.
  Ambiguous unqualified names are rejected rather than depending on map order.
- Activate/deactivate maps and set a base map, preserving existing
  top-down/blocking context semantics. The designated asset names a default
  startup map; maps can also be selected by code.
- Enumerate maps, actions, and binding descriptors (IDs, paths, scheme groups,
  display strings), including the effective path after an override.
- Begin/cancel an interactive rebind for a binding ID, with eligible-device
  filter, timeout, completion/cancel result, and conflict information. A
  controls screen can alternatively apply a validated explicit path.
- Set/remove/reset individual overrides, reset a map or entire profile,
  and explicitly load/save/export/import a profile. No game UI is supplied.

The interface reports invalid IDs, type mismatches, and unavailable controls
as status/diagnostics rather than silently returning a plausible value.
Queries on a valid but inactive map return neutral values. A game can still
use the raw `InputSnapshot` for low-level cases, but the ReferenceProject
controller must use named actions.

### Frame and fixed-step semantics

Sampling and action evaluation happen once per rendered frame. Continuous
values use the most recent sample. A press, release, or interaction transition
is latched for the next fixed tick even if that render frame has no fixed
step; it is consumed by the first fixed tick and is not repeated on later
fixed steps in the same render frame. Frame/update queries see transitions
for the frame in which they occurred. If multiple opposite transitions occur
before a fixed tick, the fixed consumer receives an ordered transition queue
rather than having one edge overwrite another. Queue capacity is bounded;
overflow emits a diagnostic and preserves the latest held value. This fixes
the current lost-edge case without changing continuous-axis behavior.

## Override Profiles and Rebinding

Overrides are a sparse mapping from stable binding ID to replacement control
path, scoped to a project GUID and profile name. The canonical asset remains
unchanged. Removing an override restores the authored path. A profile has a
schema version and writes atomically to a per-user writable location, not to
project `Content/` or the engine install directory. `LocalInputUser` loads the
default profile after the project asset; games may select another profile by
name. Saving is explicit through the API, with a well-defined dirty flag, so
the game decides when to persist changes. Project switching cannot carry an
override into another project's asset accidentally.

Rebinding validates the control path against the supported device vocabulary
and action/composite role. Conflict detection reports same-scheme bindings
that could fire together, but policy belongs to the game: accept, reject, or
clear the other override. Rebind capture ignores the initiating click/key,
supports cancellation and timeout, and applies its result at a frame boundary
so a control is not half-rebound during evaluation. A stale binding ID in an
old profile is ignored with a diagnostic and retained only in the serialized
profile until the user saves a cleaned version; valid entries still load.
Malformed profile data never modifies the asset or erases the last good
in-memory profile. Display strings are derived from effective paths, so a
game UI immediately reflects overrides. This slice provides text labels, not
platform-specific controller glyph art.

## Integration and Failure Policy

`ProjectBoot::LoadInputConfig` becomes explicitly host/editor-only. Project
boot separately resolves the selected `.arcinput` GUID through the asset
registry, validates and compiles it, then creates the game session. The same
path is used by editor Play and standalone runtime. Editor project switching
discards the old project session and loads the new project asset/profile.
The ReferenceProject gains a project gameplay asset with a `Player` map,
`Move` 1D action bound to A/D and gamepad horizontal stick, and `Jump` Button
bound to W/Space and gamepad south button. `PlayerController2DSystem` reads
these actions in fixed update instead of hard-coded scancodes. Engine-config
WASD remains available for editor/host behavior. The example does not add
game-specific automated tests.

Invalid editor edits stay in the draft editor state and show diagnostics; the
last valid published mapping continues to run. A fresh runtime boot with a
configured but missing or invalid asset fails with a clear project-input
error instead of starting with silently inert controls. An unconfigured
project boots normally with an empty game session. Invalid profile entries
are nonfatal and individually ignored. Hot reload of an invalid asset leaves
the last valid mapping active and reports the error. Asset deletion or a
missing selected GUID is never treated as an empty asset.

The SDK surface follows Arcane's engine-as-SDK contract. If adding the
project asset GUID or runtime user interface changes an exported struct or
plugin-facing ABI, bump the relevant ABI version. Do not pass ownership of
engine-allocated STL objects across the module boundary. The
`InputSnapshot` remains fixed-size POD, preserving the later replay seam.

## Acceptance and Verification

1. A developer creates an Input Actions asset in the Asset Browser, selects
   it in Project Settings, and edits the hierarchy in the same visual shape
   as the supplied reference image. The project reopens with IDs and order
   intact after save, rename, and asset move.
2. The editor can author keyboard, mouse, and gamepad bindings; chords and
   1D/2D composites; processors and interactions; schemes; and context
   behavior. Preview and conflict feedback respond to edits and input.
3. A game module queries the designated asset through `LocalInputUser`,
   activates maps, captures/rebinds controls, shows effective binding labels,
   saves a profile, and loads the same overrides on the next run without
   changing the project asset.
4. A pressed/released edge survives a render frame with zero fixed steps and
   is delivered exactly once to fixed update. Repeated fixed steps do not
   retrigger it.
5. Invalid assets/profiles and project switches follow the failure policy
   above. Editor shortcuts and camera input remain independent; engine
   `input.json` never appears as a gameplay asset.
6. The ReferenceProject platformer moves and jumps from `Move`/`Jump`
   actions using its new gameplay asset, with no hard-coded scancodes in the
   controller.

Verification should include CPU tests for asset parsing/validation, stable-ID
round trips, scheme filtering, conflict detection, overrides/persistence,
capture state, context behavior, and frame/fixed edge delivery; editor model
tests for hierarchy edits and save/reload; Debug and Release engine/module
builds; and manual editor/runtime checks with keyboard and a gamepad. The
ReferenceProject controller itself remains an exploratory game example and
does not need new game-specific automated tests.

## Design References

- [Unity Input Action Assets](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.17/manual/ActionAssets.html)
- [Unity Actions Editor](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.17/manual/ActionsEditor.html)
- [Unity Project-Wide Actions](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.17/manual/ProjectWideActions.html)
- [Unity Input Bindings and Overrides](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.17/manual/ActionBindings.html)
- [Unity PlayerInput](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.17/manual/PlayerInput.html)
- User-provided UI reference: `C:\Users\Ethan Temprovich\Desktop\Player+Action+Map.webp`.
