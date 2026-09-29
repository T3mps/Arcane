# Inspector targets: an instance follows one source kind

**Status:** Approved 2026-09-29 (brainstorm with the user; decisions in section 6)
**Amends:** `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md` s3.3 (pin and instances)
**Scheduled:** with the Material tab retirement (editor mini-arc 3), which adds the second document kind this is tested against.

## 1. The problem

An Inspector instance today does one of two things: it FOLLOWS (shows the page of whichever
source selected last, `InspectorHost::Current()`) or it is PINNED (holds one source + key).
There is no middle: "keep showing whatever is selected in the scene while I work in an action
map" needs a pin that the user re-pins on every scene click. The request: let an instance
target a source kind -- a Scene inspector keeps following scene selections and ignores every
other source.

Example: click entity A, then entity B, then open an action map and select a binding. An
instance targeted at Scene shows B; an instance targeted at All moves to the binding.

Prior art: Houdini's pane link groups (panes follow a group or are pinned). Unreal's Details
panels only follow or lock.

## 2. Model (`InspectorHost`)

- **Source kinds.** `InspectorSource` gains `[[nodiscard]] virtual std::string_view Kind() const = 0;`
  a stable id (`"scene"`, `"input-actions"`, later `"shader"`, `"asset"`). The kinds form a
  fixed editor-side CATALOG (id + display name, e.g. `"input-actions"` -> "Input Actions"),
  independent of which sources are open: a target must be choosable and restorable before
  any source of that kind exists.
- **Per-instance target.** `Instance` gains `std::string target;` -- empty = All (today's
  behaviour, the default for every instance including id 0).
- **Last source per kind.** On every selection EVENT (`NotifySelected`, behind the existing
  `SelectionEdge` filter -- a clear is never an event) the host records the source as the
  last-selecting source of its kind. `RemoveSource` drops it from that map; `ReleaseAll`
  clears the map.
- **Routing.** `SourceFor(id)`:
  1. pinned -> the pinned source (unchanged);
  2. target empty -> `Current()` (unchanged);
  3. otherwise -> the last-selecting source of the target kind; if none has selected yet, the
     most recently added registered source of that kind; if none is registered, null.
- **No source of the kind.** A targeted instance with no registered source of its kind draws
  one line: "No <display name> document open" (the scene is always registered, so a Scene
  target never hits this).
- **Deselect is per source.** What an instance shows is always its routed source's LIVE
  selection. A clear in source X therefore changes only instances routed to X: clicking empty
  space in the viewport blanks a Scene inspector ("Nothing selected") and leaves an Input
  Actions inspector on its binding; an All inspector is unaffected unless it is currently
  routed to X (a clear is not an event, so it never moves `Current()`). A targeted instance
  never keeps a stale page after its own source clears -- keeping a page is what pin is for.
- **Pin is per instance.** `CanPin` and `SetPinned` take the instance id and capture what
  THAT instance shows (`SourceFor(id)` + its key + name), not `Current()`. Without this,
  pinning a Scene inspector just after clicking a binding would pin the binding. Unpinning a
  targeted instance returns it to following its target.

## 3. History

- One shared history (depth 32, prune-on-invalidate, never persisted) -- unchanged.
- A targeted instance's back/forward step only through entries whose source's `Kind()`
  matches the target. `CanGoBack/CanGoForward/GoBack/GoForward/BackEntry/ForwardEntry` take
  an optional kind filter; All = no filter (today's exact behaviour).
- Landing on an entry restores it in its source (unchanged `TryLand`), so it IS a selection:
  the cursor moves in the shared list and All instances follow. The history dropdown of a
  targeted instance lists only matching entries.

## 4. UI

- **Header:** a target combo between the back/forward arrows and the breadcrumb:
  `<- -> [Scene v]  Scene > Player  (pin)`. Items: "All" + every catalog kind. Available on
  every instance, including id 0.
- **Title:** a targeted instance shows its target ("Inspector 2 - Scene", "Inspector - Input
  Actions"); the `###` window id is unchanged so the dock slot and `[Window]` entry survive a
  re-target.
- A targeted window must be obviously targeted: a Scene inspector that ignores action-map
  clicks otherwise reads as broken.

## 5. Persistence

- Targets are LAYOUT (like the instance list), persisted in the existing
  `[EditorInspector][Instances]` section as one more line: `Targets=<id>:<kind>,...`
  (instances absent from the line are All). A kind not in the catalog at load -> All.
- The existing ClearAllFn resets targets with the list (a windowed project switch reads the
  incoming project's layout). `ReleaseAll` (project switch) keeps targets and releases
  sources, pins, history and the per-kind map exactly as today.
- Pins stay unpersisted.

## 6. Decisions taken in the brainstorm (2026-09-29)

1. **Target granularity = source KIND**, not a specific source (a specific document dies on
   close and cannot persist) and not Scene-vs-Documents buckets (cannot tell a shader
   inspector from an input-actions one after the Material tab migrates).
2. **Back/forward = filtered view of the shared history**; landing re-selects in the source,
   so All instances follow. Not a per-instance history (a second list to keep in sync), not
   hidden arrows (the Scene inspector is where "back to the previous entity" matters most).
3. **An emptied source shows "Nothing selected"**, never the last page (that would be an
   unmarked auto-pin whose edits land on an entity selected nowhere). Deselect stays per
   source/editor: a scene clear never blanks an instance targeted elsewhere (s2).
4. **Header combo + title suffix**; any instance, including the main one.
5. **Scheduled with the Material tab retirement** (mini-arc 3): it brings the shader kind, so
   the routing is exercised with three kinds.

## 7. Verification

- Unit (`EditorInspectorHostTest`; the existing fake sources gain a kind):
  - routing by kind; All unchanged (every existing test passes untouched);
  - two sources of one kind: the last-selecting wins; closing it falls back to the other;
  - no source of the kind -> `SourceFor` null;
  - **a clear in source X never changes what an instance targeted at Y shows** (and a clear
    never moves an All instance off a different source);
  - `SetPinned` on a targeted instance captures that instance's source/key, not `Current()`;
  - filtered back/forward skip non-matching entries; landing makes the source current;
  - ini round-trip of `Targets=`; an unknown kind -> All; ClearAllFn resets targets.
- Witness: the report's inspector block gains `instances[].target` (kind id, `""` = All).
- Desk: the section 1 example (A, B, action-map binding) with a Scene and an All instance;
  an empty-space click in the viewport with a Scene + an Input Actions instance; re-target a
  docked instance and confirm the dock slot holds.
