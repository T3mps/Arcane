# Inspector filters: which source kinds an instance follows

**Status:** Approved 2026-09-29 (brainstorm with the user; decisions in section 8)
**Amends:** `docs/superpowers/specs/2026-09-28-inspector-ownership-design.md` s3.3 (pin and
instances) and s3.5 (the Asset Browser's preview pane goes ENTIRELY; the "keeps the thumbnail
and the Open button" option is dropped).
**Scheduled:** editor mini-arc 2, as ONE piece with the asset page and the preview-pane removal
(s6 explains why they cannot land apart). Mini-arc 3 (the Material tab retirement) then adds
the shader kind to a feature that already exists.

## 1. The problem

An Inspector instance today does one of two things: it FOLLOWS (shows the page of whichever
source selected last, `InspectorHost::Current()`) or it is PINNED (holds one source + key).
There is no middle: "keep showing whatever is selected in the scene while I work in an action
map" needs a pin that the user re-pins on every scene click.

The request: each instance carries a FILTER -- a set of source kinds it follows, chosen from a
dropdown of checkboxes. All kinds ticked (the default) is today's behaviour; "Scene" alone is a
scene inspector; "everything but Scene" is equally expressible.

Example: click entity A, then entity B, then open an action map and select a binding. An
instance filtered to Scene shows B; an instance on All moves to the binding.

Prior art: Houdini's pane link groups (panes follow a group or are pinned). Unreal's Details
panels only follow or lock.

## 2. Kinds

- `InspectorSource` gains `[[nodiscard]] virtual std::string_view Kind() const = 0;` -- a
  stable id: `"scene"`, `"input-actions"`, `"assets"` (this arc), `"shader"` (mini-arc 3).
- The kinds form a fixed editor-side CATALOG (id + display name, e.g. `"input-actions"` ->
  "Input Actions"), independent of which sources are open: a filter must be choosable and
  restorable before any source of a kind exists.

## 3. Model (`InspectorHost`)

- **Per-instance filter.** `Instance` gains `std::vector<std::string> excluded;` -- the
  UNTICKED kinds. Empty = All (the default for every instance). The filter is stored as
  exclusions on purpose: a kind added to the catalog later is SHOWN by every existing filter
  ("everything but Scene" keeps meaning that when Shader arrives); see decision 8.3.
- **At least one kind stays ticked.** The dropdown refuses to untick the last ticked kind
  (the checkbox draws disabled with a tooltip). An instance that can show nothing reads as
  broken.
- **Selection order.** The host keeps a monotonic counter; every selection EVENT
  (`NotifySelected`, behind the existing `SelectionEdge` filter -- a clear is never an event)
  stamps its source with the next value. `RemoveSource` drops the stamp; `ReleaseAll` clears
  them all.
- **Routing.** `SourceFor(id)`:
  1. pinned -> the pinned source (unchanged);
  2. no exclusions -> `Current()` (unchanged);
  3. otherwise -> among registered sources whose kind is NOT excluded, the one with the
     highest stamp; if none has a stamp yet, the most recently added matching source; if no
     matching source is registered, null.
- **No matching source.** The instance draws one line naming what it waits for: "No Input
  Actions document open" for a single kind, "Nothing to show for this filter" otherwise.
- **Deselect is per source.** What an instance shows is always its routed source's LIVE
  selection. A clear in source X changes only the instances routed to X: clicking empty space
  in the viewport blanks a Scene inspector ("Nothing selected") and leaves an Input Actions
  inspector on its binding; an All inspector is unaffected unless it is currently routed to X
  (a clear is not an event, so it moves no stamp and never moves `Current()`). A filtered
  instance never keeps a stale page after its own source clears -- keeping a page is what pin
  is for.
- **Pin is per instance.** `CanPin` and `SetPinned` take the instance id and capture what THAT
  instance shows (`SourceFor(id)` + its key + name), not `Current()`. Without this, pinning a
  Scene inspector just after clicking a binding would pin the binding. Unpinning returns the
  instance to following its filter.

## 4. History

- One shared history (depth 32, prune-on-invalidate, never persisted) -- unchanged.
- A filtered instance's back/forward step only through entries whose source's kind passes its
  filter. `CanGoBack/CanGoForward/GoBack/GoForward/BackEntry/ForwardEntry` and the history
  dropdown take the instance's filter; no exclusions = today's exact behaviour.
- Landing on an entry restores it in its source (unchanged `TryLand`), so it IS a selection:
  the cursor moves in the shared list and every instance whose filter admits that source
  follows.

## 5. UI

- **Header:** a filter dropdown between the back/forward arrows and the breadcrumb:
  `<- -> [All but Assets v]  Scene > Player  (pin)`. It opens a list of checkboxes, one per
  catalog kind. Available on every instance, including id 0.
- **Label** (dropdown face and window-title suffix):
  - every kind ticked: "All" on the dropdown, no title suffix;
  - one kind ticked: its name ("Scene");
  - one kind unticked: "All but <name>" ("All but Assets");
  - otherwise: the ticked names joined with ", ", ellipsized to the dropdown width (full list
    in the tooltip).
- **Title:** "Inspector - All but Assets", "Inspector 2 - Assets"; the `###` window id is
  unchanged so the dock slot and `[Window]` entry survive a filter change.
- A filtered window must be obviously filtered: an inspector that ignores some clicks
  otherwise reads as broken.

## 6. The Asset Browser's preview pane goes; two inspectors by default

- The Asset Browser's in-panel preview pane (`kAssetsPreviewPaneDefaultWidth`, the
  table<->preview splitter) is REMOVED entirely. The Asset Browser becomes an ordinary
  `InspectorSource` (kind `"assets"`); its selection contributes the asset page (identity,
  cook, derived rows, the thumbnail and the Open button -- everything the pane showed).
- **Default layout (`BuildDefaultLayout`) gets two inspectors:**
  - "Inspector" (id 0) in the right node, filter **All but Assets**;
  - "Inspector 2" filter **Assets only**, docked in a split to the RIGHT of the Asset
    Browser's node (where the pane was), so it sits where users already look.
  This keeps today's behaviour -- browsing assets never replaces the entity page -- while the
  asset details become a full Inspector (history, pin, its own filter).
- **Why this cannot land apart from the filters:** once the Asset Browser is a source, an
  unfiltered Inspector jumps to every asset click and throws away the page the user was
  editing. The pane is what prevents that today; the filtered default layout is its
  replacement. Asset source + pane removal + filters + default layout ship together.
- **Layouts saved before this feature are upgraded ONCE.** A `[EditorInspector][Instances]`
  section with no `Filters=` line was written before this feature. On that first load the
  host applies the new default: instance 0 gets "All but Assets", and an "Assets only"
  instance (lowest free id) is docked into a split to the right of the Asset Browser's dock
  node (if the Asset Browser is floating or closed, the instance opens floating beside where
  the browser last was / at the default floating position). The next save writes `Filters=`,
  so the upgrade never repeats. A user who deliberately set everything back to All keeps it:
  an empty `Filters=` line is a real answer, not a missing one.

## 7. Persistence

- Filters are LAYOUT (like the instance list), persisted in `[EditorInspector][Instances]`
  as one more line: `Filters=<id>:<kind>+<kind>,<id>:<kind>` listing each instance's
  EXCLUDED kinds (instances absent from the line are All; the line itself is always written).
  Kinds not in the catalog at load are dropped from the exclusion set.
- The existing ClearAllFn resets filters with the list (a windowed project switch reads the
  incoming project's layout; a missing `Filters=` there triggers the one-time upgrade too).
  `ReleaseAll` (project switch) keeps filters and releases sources, pins, history and the
  selection stamps exactly as today.
- Pins stay unpersisted.

## 8. Decisions taken in the brainstorm (2026-09-29)

1. **Filter by source KIND**, not by a specific source (a specific document dies on close and
   cannot persist) and not Scene-vs-Documents buckets (cannot tell a shader inspector from an
   input-actions one).
2. **A multi-select filter (checkbox dropdown)**, not a single target: "everything but X" is a
   first-class filter. (Supersedes the single-target draft of this spec, same day.)
3. **Persist the unticked kinds**, so a new kind appears in existing filters. The user's rule:
   the tool stays honest -- a filter that silently narrows when the catalog grows is the kind
   of surprise that makes a tool infuriating.
4. **Back/forward = the shared history, filtered**; landing re-selects in the source. Not a
   per-instance history (a second list to keep in sync), not hidden arrows.
5. **An emptied source shows "Nothing selected"**, never the last page (an unmarked auto-pin
   whose edits land on an entity selected nowhere). Deselect stays per source/editor: a scene
   clear never blanks an instance filtered to another kind (s3).
6. **Header dropdown + title suffix**; any instance, including the main one.
7. **The preview pane is removed entirely**; the default layout carries two inspectors (All but
   Assets / Assets only). Filters move into mini-arc 2 with the asset page, because the pane
   cannot go before the filters exist.
8. **Old layouts are upgraded once** to the two-inspector default (s6), rather than left on a
   single All inspector with a "Reset Layout" hint.

## 9. Verification

- Unit (`EditorInspectorHostTest`; the existing fake sources gain a kind):
  - routing by filter; no exclusions unchanged (every existing test passes untouched);
  - two sources admitted by one filter: the higher stamp wins; closing it falls back to the
    other; "All but X" follows Y and Z but never X;
  - no admitted source -> `SourceFor` null;
  - **a clear in source X never changes what an instance filtered away from X shows** (and a
    clear never moves an All instance off a different source);
  - the last ticked kind cannot be unticked (the setter refuses an all-excluded set);
  - `SetPinned` on a filtered instance captures that instance's source/key, not `Current()`;
  - filtered back/forward skip non-matching entries; landing makes the source current;
  - ini: `Filters=` round-trip; an unknown kind dropped; a kind added to the catalog after the
    save is admitted; a missing `Filters=` line triggers the one-time upgrade and an empty
    one does not; ClearAllFn resets filters.
- Goldens: `editor-ui` changes (two inspectors, no preview pane) -- re-bless per the golden
  procedure; a new asset-selected golden (`--select` of an asset in the Browser, the Assets
  inspector on the asset page, the main Inspector unchanged).
- Witness: the report's inspector block gains `instances[].excluded` (kind ids; `[]` = All).
- Desk: the s1 example with a Scene and an All instance; an empty-space viewport click with a
  Scene + an Input Actions instance; an asset click on the default layout (the entity page
  stays); a pre-feature ReferenceProject/Aphelyon layout upgrading once and not again; change
  a docked instance's filter and confirm the dock slot holds.
