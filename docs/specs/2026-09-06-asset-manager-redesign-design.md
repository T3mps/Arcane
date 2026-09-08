# Asset Manager Redesign — Design

**Date:** 2026-09-06
**Status:** Approved design — **Plan 1 LANDED 2026-09-06** (see §17); Plans 2–3 pending
**Design record:** https://claude.ai/code/artifact/de210519-110a-47a1-b5f8-9d82b4ec421b
(working files: `.superpowers/design/asset-manager-mockups/` — the canvas carries its
own decision record; boards titled FINAL are binding on visuals, the Live Demo board
is binding on behavior)

## 1. What this is

The Assets panel becomes **one panel with three lenses** behind a segmented switch:

- **Browse** — kind rail · folder-grouped table · preview pane (the "B+C" board)
- **Graph** — the derivation web: assets as nodes, references as edges (Plan 3)
- **Status** — a health dashboard: tiles, cook meter, attention cards, activity feed (Plan 2)

plus a **unified Create flow** (one menu, one dialog anatomy, one request path) and a
**shared interaction contract** (peek tooltip, one selection across lenses).

It replaces today's flat two-column table and retires four recorded pains:

1. Five asset-creation patterns for one conceptual action (menu dialogs, a bare
   `+ Mesh` toolbar button, per-row context items, and post materials reachable only
   by re-kinding a document).
2. Every plain texture rendered twice (the `.png` row and its auto-minted
   `.arcsprite` row).
3. No thumbnails anywhere, although every cooked artifact already carries a ≤64px
   RGBA thumb served by `Assets::PixelsFor`.
4. Kind-blind pickers (`AssetKindOf` buckets every `.arcmat` as one `Material` kind;
   subkind lives only inside the JSON).

**Not chosen, per the canvas decision record — do not implement:** Option A entirely
(folder tree, breadcrumbs, thumbnail tile grid, tile-size slider, grid/list toggle);
Options B and C as standalone panels; kind filter chips; the Kind column; the old
path+guid hover tooltip. **Note (2026-09-07):** the table's folder groups did later
grow indentation and cascading collapse (§6) — this is the "middle form" the user
ruled for, entirely inside the existing single table, and is not a reversal of
Option A's rejection. Option A's rejected shape specifically was a **separate tree
panel** with breadcrumbs; no such panel exists or is planned.

## 2. Scope and phasing

One spec, three plans. Each plan lands green (tests + gate + desk checkpoint) before
the next starts.

- **Plan 1 — shell + Browse + Create + interactions.** Replaces the old panel
  outright. Includes both engine queries (one ABI bump), the model, the thumbnail
  cache including material sphere thumbs, the unified create flow, and the
  kind+subkind-filtered pickers. Graph/Status buttons ship disabled.
- **Plan 2 — reference index + Status lens.** Includes the scene reference manifest
  (schema v4) and its fallback scan, the `AssetReferenceIndex`, the activity log,
  and the digest's `unused` count going live.
- **Plan 3 — Graph lens.** Nodes/edges over the index, focus scoping, pin-drag
  creation.

## 3. The engine seam (`ArcaneClient`, Assets facade, ABI 21 → 22, lands in Plan 1)

Two stateless queries — parse-on-call, no engine-side cache (the editor's index and
model are the caches):

### 3.1 `Assets::MaterialSurfaceFor(Guid) -> std::optional<MaterialSurface>`

(Plan-time verification corrected the type: there is no `MaterialKind` enum — the
engine's subkind type is `MaterialSurface { Fullscreen, Sprite, Mesh }`
(`MaterialSource.hpp:71`), mapped from the `.arcmat` `"kind"` string
(`"fullscreen"/"sprite"/"mesh"`) by `MaterialSurfaceForKind`.) Returns the resolved
surface. **Instance files carry no `kind` field** — only a `"parent"` guid — so the
query walks the parent chain (bounded depth, cycle-safe) to the base material's
kind. `nullopt` = not a material or unreadable. UI note: the display label for
`Fullscreen` is "post" (the mocks' vocabulary); the wire/disk vocabulary is
unchanged.

### 3.2 `Assets::ListAssetReferences(Guid) -> std::optional<std::vector<AssetRef>>`

`AssetRef { Guid target; AssetRefKind kind; }`,
`enum class AssetRefKind { References, DerivesFrom }`. Two kinds is deliberate:
`DerivesFrom` powers Browse's derived-sprite fold and the Graph's "derives" edges;
everything else is a plain reference.

Per-format extraction, one engine-side parser per format:

| Format | Extraction | RefKind |
|---|---|---|
| `.arcsprite` | its `texture` guid | **engine decides**: no sub-rect/slice data ⇒ `DerivesFrom` (the minted 1:1 wrap); sliced ⇒ `References` |
| `.arcmat` | every param with `"type": "texture"` | `References` |
| `.arcmat` (instance) | parent material (field name verified at plan time) | `DerivesFrom` |
| `.arcscene` | the `assets` manifest (3.3); pre-v4 scenes: structural scan (3.4) | `References` |
| `.arcmesh` | its `material` guid (default material, `MeshAsset.hpp:89`) — plan-time verification corrected this row: `.arcmesh` is NOT self-contained | `References` |
| textures, audio, fonts | documented leaf formats | — |
| `.json` data, `.arcdiag` | documented opaque | — |

Encoding the sprite fold decision at the parser keeps format knowledge in one place;
the editor never re-reads sprite JSON.

**Failure semantics:** `nullopt` = could-not-read (mid-save, broken file). The editor
keeps that asset's **last-known refs** (the `OnCookCompleted` last-known-good stance)
and retries on the next change event. A never-parsed asset contributes nothing to
anyone's inbound counts. Parse failures surface through the existing Problems path;
they are never silently dropped.

**Coverage guarantee:** ArcaneTests pins per-format extraction against
ReferenceProject fixtures, plus a test asserting every extension the registry
classifies maps to a known extractor or a documented leaf/opaque entry — a future
format cannot silently return empty refs.

### 3.3 Scene reference manifest (schema v3 → v4, lands in Plan 2 as the fast path)

At save time the scene serializer emits a top-level `"assets": [guids]` block — the
distinct asset guids the scene references (identity fields excluded via the existing
`IsIdentityGuidFieldName` rule, nils dropped). The serializer already visits every
reflected field; this is a byproduct, not a second pass. `ListAssetReferences` for a
v4 scene reads the manifest: exact, O(manifest), zero heuristics. The scene *loader*
never reads it — a stale hand-edited manifest can mislead the asset panel but can
never break a scene. Side benefit recorded for later: the manifest is a scene's
preload set, readable without loading the scene.

### 3.4 Structural scan (ships in Plan 1; becomes the pre-v4 fallback in Plan 2)

Scene extraction must be complete from the day `ListAssetReferences` exists — the
coverage test demands it — so the structural scan ships in Plan 1 and serves all
scenes until the manifest fast-path supersedes it for v4 scenes in Plan 2.

Scenes serialize guid references as `{hi, lo}` u64 pairs on reflected component
fields (verified in `main.arcscene`; nil = zeros). The fallback scan: any `{hi, lo}`
pair under a non-identity key, nonzero, **and resolvable in the registry**, is a
reference. The resolvability filter kills false positives. This path exists only for
scenes that have never been re-saved since v4; the next save writes the manifest.

## 4. `AssetPanelModel` (editor, pure, headless-testable)

One unit owning everything the lenses read.

- **Entry:** `{guid, name, mountPath, folder, AssetKind, subkind (materials),
  CookState ∈ {Cooked, Queued, Refused, Unknown}, derivedChildren, foldedUnder}`.
  The fold: a `DerivesFrom` 1:1 sprite gets `foldedUnder = texture` and stops being
  a peer row.
- **Derived views:** the folder-grouped row list (groups → entries → indented
  children), rail counts, `HealthCounts` (digest + Status tiles/meter), filtered
  projections (search reuses `MatchesFilter` semantics — case-insensitive substring
  over name and mount path; rail kind filter).
- **Invalidation, not per-frame rebuild:** per-guid dirty marks from the seams the
  editor already has — there is no registry watcher; the editor polls
  (`PollAssetWatch`'s ~1 Hz mtime sweep + drop discovery), and cook completion
  arrives via `CookQueue::SetOnCookComplete` → `EditorApp::OnCookCompleted`. The
  model exposes `MarkDirty(guid)`/`MarkAllDirty()` and those sites call it; full
  rebuild only on registry-scale events (project open, ScanContent). Engine queries are cached per guid and re-asked only for dirtied
  guids. Rebuild is lazy, at next draw. Today's rebuild-every-frame dies here.
- **The one shared selection** (single `Guid`) lives in the model; all lenses and
  the preview read it. Reset on project switch (today's rule kept). A
  selection-changed flag per consumer lets each lens re-center exactly once when the
  selection was changed elsewhere.
- **Testing:** inputs are injected callables (registry snapshot, kind/refs
  providers), so ArcaneTests drives fold, counts, filtering, and invalidation with
  no ImGui and no engine.

The old `AssetBrowser.*` pure helpers that other code uses (`AssetKind`,
`AssetKindOf`, `AssetKindFilterForFieldName`, `IsIdentityGuidFieldName`,
`kAssetDragType`/`AssetDragPayload`) migrate into the model header unchanged;
`InspectorView` keeps compiling.

## 5. Panel shell and chrome contracts

`AssetsPanel` keeps the panel identity ("Assets" in PanelRegistry and the Window
menu — no dock churn). Three fixed bands:

- **Toolbar:** `+ Create` (unified menu) · search well (flex, feeds the model
  filter) · per-lens slot (Graph's focus combo only, Plan 3; empty otherwise) ·
  lens strip anchored right-most. Plan 1 ships the full three-button strip with
  Graph and Status **disabled** (`TextDisabled`) — layout pinned from day one, later
  plans enable, nothing shifts.
- **Bottom bar:** left = context — `N assets · 1 selected`, becoming `X of N shown`
  whenever rail or search filters; right = the digest chip — `⚠ N refused` (amber) ·
  `N cooking` · `N unused` (dim), refused/cooking live from cook state in Plan 1,
  unused rendered `—` until Plan 2, click-through to Status once Status exists.
  No path/guid in the bar; the preview pane owns those.
- **Body:** the active lens over the shared model.

Rules: any ImGui table is authored `NoSavedSettings` (the imgui.ini veto lesson).
Narrow docks: rail fixed 180px; preview **default 165px, user-resizable via a drag
splitter between the table and the pane (session-only state, clamped to
[120px, 480px])** — below a 720px panel width the preview hides first, and the
splitter itself clamps the pane down before that hide rule has to (the table
never drops below readable width). See §17 (2026-09-07) for the follow-up that
changed the pane from a pinned 330px to this default+resizable shape. Folder
groups default expanded (derived children default collapsed, §6), **with one
recorded exception (2026-09-07, third revision, user-directed, mount-rooted):
the `diagnostics/` mount root (`diag://`) defaults COLLAPSED.** This is the
whole point of giving it its own root — crash-report `.arcdiag` files collapse
into one quiet `diagnostics/ N` band instead of polluting the `Content/` tree
the user actually works in. No other group gets this treatment; it is a
per-mount-root default, not a general rule. Panel state
(active lens, rail filter, search, group collapse, **and the preview pane's
width**) is session-only in v1.

## 6. Browse lens

- **Rail:** `All` + one row per `AssetKind` present (zero-count kinds hidden),
  counts from the model. The hover `+` appears **only on kinds with a Create
  entry** (Materials, Sprites, Meshes, Scenes) and opens the unified Create menu
  pre-scoped to that kind. Textures/Data/Audio/Font get no `+`.
- **Table:** **one depth-0 root group per populated mount** (2026-09-07, third
  revision that day, user-directed, **mount-rooted**), honest about where assets
  actually live — no synthetic bucketing of one mount's files into another's
  tree. `Content/` (`game://`, the project's primary mount) is always first,
  default-**open**, and holds the project's root-level files as its direct rows;
  everything the first two revisions described about it (root-anchored: "I want
  the entire table to have indention status, showing folder hierarchy") is
  unchanged *within* `Content/`'s own subtree. Other populated mounts appear as
  sibling depth-0 root groups in the same table, ordered lexicographically after
  `Content/` — today that is `diagnostics/` (`diag://`, crash-report `.arcdiag`
  files); a future `engine://` or `plugin://` mount would appear the same way if
  it ever registers assets. **`diagnostics/` defaults COLLAPSED** — the one
  recorded exception to groups-default-open (§5) — so crash reports collapse
  into a single quiet `diagnostics/ N` band instead of polluting the `Content/`
  tree the user actually works in. Every content directory nests inside its own
  mount root as a child group — a directory's depth is **1 + its nesting depth
  below its own mount root** (a top-level directory like `materials/` under
  `Content/` is depth 1; `textures/patterns/` is depth 2) — sorted
  lexicographically at each level within its mount, collapsible (session state).
  **Nested directories render as indented child groups inside this same table**
  (not a separate tree panel, no breadcrumbs): 20px indent per depth, and a
  group's label shows only its leaf segment (`patterns/`, not
  `textures/patterns/`; a mount root like `Content/` or `diagnostics/` keeps its
  own name — it is the one kind of group that never shortens to nothing).
  Collapse cascades downward from whichever group is closed — closing a mount
  root empties that root's own subtree (closing `Content/` empties everything
  under `Content/`; `diagnostics/` starts in exactly that state) — but mount
  roots are peers, never ancestors of each other, so closing one never touches
  another's rows. Each descendant group otherwise keeps its own open flag, so
  reopening a parent (its own mount root included) restores whatever sub-state
  its children had. Asset rows indent **one level beneath their own group's
  band** (2026-09-07, user-directed) plus their existing base offset, so they
  read as clearly nested inside that group rather than flush with its header.
  **Search reveals matches uniformly** (2026-09-07 review fix round 1, Important
  4 — this is the corrected wording; an earlier draft of this paragraph
  attributed the rule to an existing fold-child precedent that, on inspection,
  had never actually shipped that override): while a search is active, collapse
  is bypassed at **every** level — a group's own closed flag, any ancestor's up
  to and including its own mount root, and a texture's own folded-children flag
  all stop hiding a row that matches, and every group on the path down to it
  still renders (even one with zero of its own matching entries) so the tree's
  context stays visible — a search matching a `.arcdiag` file pops `diagnostics/`
  open under the same rule, its default-collapsed state notwithstanding. A
  directory that holds no files of its own but has a populated descendant
  (whether from nesting alone or because a kind filter left its own entries at
  zero) still gets a group row — its count is suppressed rather than shown as a
  bare `0` — so a subtree's chevron is always reachable to reopen it, never
  orphaned behind an ancestor that itself never renders; **every mount root is
  this bridge's unconditional case for its own subtree** — `Content/` and
  `diagnostics/` each always render, even with zero loose root files or zero
  diagnostics respectively (count suppressed the same way, never a bare `0`),
  because every directory beneath a mount needs that mount's own root row to
  hang from. Rows are Name-only: 18px thumb, name, pills (subkind,
  `boot`, `sliced`, `derived`). Derived 1:1 sprites render only as indented children
  under their texture, **default collapsed** with a count pill, keeping their own
  **extra +20px fold indent** on top of the group's own depth indent — two
  indentation meanings, kept visually distinct as today (chrome group bands vs dim
  child rows). A **refused** asset gets a small amber triangle on its row; queued
  gets no row marker.
- **Preview pane:** **compact, side-by-side header** (2026-09-07, fourth
  revision, user-directed — a live screenshot showed the sidebar scrolling
  while the space right of the thumb sat empty): the 140px thumb sits on the
  left, and to its right a metadata block stacks name + kind/subkind pills,
  then path, then guid (click-to-copy, same path hover tooltip as today) —
  same content as before, just arranged beside the thumb instead of below it,
  which is what removes the wasted width and shrinks the header's total
  height. §11.2's 140px thumb value is unchanged; this is a rearrangement, not
  a resize — the thumb keeps scaling via `min(140, avail)` when the pane is
  narrower than that. Below roughly **250px of pane width** (exact breakpoint
  is an implementer tuning value, not a pinned constant) the header falls back
  to today's stacked form — thumb above, metadata below — since the metadata
  column no longer has room to stay legible beside the thumb; the 165px
  default pane width stays on this stacked fallback, so the compact header is
  something a user sees only after dragging the pane wider, exactly the
  screenshot's situation. `Derived (N)` list (click selects the child) and the
  full-width action buttons stay below, unchanged by this revision.
- **Context menu** (every representation): kind-specific entries first (New
  Instance…, Set as Boot Scene, Create Sprite — today's exactly), then `Create ▸`
  (the full unified menu), then Show in Explorer, Copy Path, **Copy Guid** (new).
- **Open/drag:** double-click keeps today's routing verbatim (documents via
  `DocumentHost`; scenes via the `openScene` action and its unsaved-changes guard).
  Drag keeps the existing `ARCANE_ASSET` payload; child rows drag as themselves;
  **no new drop targets in this arc**.

### 6.1 Thumbnails

A new editor `ThumbnailCache` (guid → GPU texture, LRU), **subscribed to
cook-completion events from day one** (it does not join the deferred invalidation
family):

- Textures and sprites: `Assets::PixelsFor`'s ≤64px RGBA thumbs.
- **Materials: real sphere thumbs in Plan 1** — a 64px snapshot harvested from the
  existing document-preview render context on save/cook, stored in the same cache.
  (User ruling: fidelity over plumbing cost; the mocks show spheres.)
- Everything else (mesh, scene, data, audio, font, diagnostic): the Lucide kind
  icon on a well background — which is what the mocks show for those kinds.

## 7. Unified Create

**Menu (final):** `Material…` · `Material Instance…` · ─ · `Mesh…` · `Sprite…` ·
`Scene…`. Today's `Material…`/`Mesh Material…` pair collapses into one `Material…`
whose dialog carries a **Kind field (sprite / mesh / post)** — post materials get a
real creation route. The silent `+ Mesh` toolbar button dies; Mesh gets the dialog
(name + location, default cube data as today).

**Shared dialog anatomy:** Name (validated: non-empty, filesystem-safe, unique in
target directory; Create disabled until valid) · Location (combo of existing content
directories, per-kind default) · kind-specific fields (Material → subkind; Instance
→ parent picker; Sprite → texture picker; Scene → optional "set as boot"). The
Sprite dialog surfaces mint-or-reuse **visibly**: if a 1:1 derived sprite already
exists for the chosen texture, it says so and offers to open it. Failures route
through the existing `ModalErrorQueue` + Problems.

**One implementation, by refactor not parallel** (user ruling): the existing
`CreateMaterialAt` flow and its dialog become the shared dialog unit's core; the old
`newMaterial`/`newMeshMaterial` request fields become one
`CreateAssetRequest {kind, prefill}`. Entry points — Assets top menu (kept, same
submenu), panel `+ Create`, rail `+` (pre-scoped), row context `Create ▸`, Graph
pin-drag (Plan 3, pre-parented) — are thin producers of that request.
**Invariant: no creation path may bypass `CreateAssetRequest`.**

**Kind-filtered pickers** (closes the recorded kind-blind-picker follow-up, Plan 1):
dialog pickers filter by kind and subkind, showing subkind pills. The inspector's
material picker gains subkind filtering via the owning component (SpriteRenderer →
sprite, MeshRenderer → mesh), extending the existing field-name-heuristic seam until
reflection grows per-field attributes.

## 8. Interaction contract

- **Peek tooltip:** one shared helper `DrawAssetPeekTooltip(model, guid)` — rows,
  child rows, the preview's Derived list, Status cards/feed, Graph nodes. Built on
  `ImGuiHoveredFlags_ForTooltip` (delay ~0.5s, stationary behavior, drag suppression
  from ImGui's tooltip config). Content: 64px thumb, name + kind/subkind pills,
  path, cook line, guid. Structurally text-and-images only — never buttons.
- **Selection:** the model's single guid; any representation's click sets it;
  lenses re-center once on externally-changed selection.
- **Keyboard, minimal v1:** Up/Down move selection through visible rows; Enter
  opens. Rename-in-place does not exist today and stays out of scope.
- Double-click / right-click / drag: as in §6.

## 9. Plan 2 — reference index + Status lens

### 9.1 `AssetReferenceIndex` (editor)

Built in one pass on project open (`ListAssetReferences` per asset → forward map,
inverted map, inbound counts); re-walks single guids on save/watcher/create/delete
events. Last-known-good on `nullopt` (§3.2). Two disciplines adopted from UE's
registry (2026-09-06 source review of the `.example` dump):

- **Incremental rebuild order** (UE `AssetRegistry.cpp:7034-7041`): when re-walking
  one asset, first iterate its OLD outbound refs and remove it from each target's
  referencer list, then clear and re-add. Skipping the removal pass is the classic
  incremental-index corruption.
- **Tombstones for deleted/unresolvable targets** (UE keeps an empty depends-node
  so "who referenced the missing asset" survives): a referenced guid that no longer
  resolves keeps an index node holding its referencers — exactly the "dangling
  reference" list the Status lens can report.

**Unused = zero inbound references**, applied **only to kinds whose consumers the
index fully sees: Texture, Material, Sprite, Mesh.** Scenes are roots (never
unused); Data/Audio/Font are consumed by game code in ways no index observes, so
they are exempt rather than falsely accused. Unused cleanup cascades: the root of a
dead subgraph always has zero inbound refs; removing it surfaces its dependencies
next. **Known limitation, accepted:** a dead reference *cycle* never flags (each
member has inbound ≥ 1); asset cycles are near-pathological in these formats and no
machinery is built for them.

### 9.2 Status lens

Reads `HealthCounts` + the index: stat tiles (assets / cook refused / awaiting cook /
unreferenced) · cook-pipeline meter (stacked, 2px gaps, direct labels + swatches) ·
Needs-attention cards (refused wears the muted-amber acting-on frame with **Recook**
— artifact invalidation through the existing cook seam — and **Problems** — jumps to
the pane; queued shows a progress strip) · Unreferenced card with inset-well chips
and **Reveal** (switches to Browse, selects) · Activity feed · Scenes rollup card
(per scene: asset count, health; **Focus in Graph** disabled until Plan 3).

**Activity feed:** a new session-only ring buffer (`AssetActivityLog`, ~100 entries)
pushed by the same events the model consumes (source changed → queued, cooked,
refused, created, deleted). No persistence.

## 10. Plan 3 — Graph lens (design commitments; details in its plan)

- Nodes/edges straight from the index; edge labels from `AssetRefKind`
  (derives / uses); "samples" is a display label for a material's `References`
  edges, derived from the source asset's kind — not a third `AssetRefKind`.
- **Scoping model** (revised 2026-09-06 after the UE Reference Viewer review —
  its shape beats our original flat node cap): default focus = boot scene;
  **depth limit default 2** from the focus in each direction; **per-node breadth
  cap (~20)** where overflow collapses into a synthetic "+N more" node — never
  silent truncation (UE `EdGraph_ReferenceViewer.cpp:912-924`) — with edges
  sorted most-important-first (`DerivesFrom` before `References`) before the cut
  so truncation drops the least interesting links. "Everything" remains allowed
  under the same caps.
- Layout: layered left→right by dependency depth (sources left, scenes right,
  matching the mocks); computed each build, not persisted; no physics simulation.
- Rendering reuses the shader editor's canvas vocabulary (pan/zoom, grid, node
  drawing, pins, bezier edges).
- **Pin-drag v1 offers `Derive Instance…` only**, producing a pre-parented
  `CreateAssetRequest`. "Assign to selection" needs entity-slot targeting rules that
  do not exist; explicitly deferred.
- Node anatomy per the mock: chrome header + kind icon, 3px kind-color accent bar,
  kind-colored pins, body row (thumb/meta), selection = 2px `kSelection` border,
  selected node's edges brighten.

## 11. Visual fidelity

**The mocks are the redline.** The canvas's FINAL boards bind visuals; the Live Demo
binds behavior. Each plan's desk checkpoint carries an explicit **side-by-side
item**: the live panel at 960×620 next to the corresponding board render (human
comparison — web AA vs ImGui AA makes automated pixel diffs false-fail).

### 11.1 Custom widget inventory (shared `EditorWidgets` layer, not panel-local)

| Helper | Draws | Plan |
|---|---|---|
| `AssetPill` | bordered 12px label; amber variant | 1 |
| `RowWithThumb` | selectable row: 18px image · name · trailing pills · right-aligned extras (also the rail row) | 1 |
| `SegmentedStrip` | lens switch; collapsed shared 1px borders; active = `kButtonActive`; square | 1 |
| `AssetPeekTooltip` | §8 composition inside `ForTooltip` | 1 |
| `StatTile` | bordered card, `PushFont` 24px number, 13px label, optional icon | 2 |
| `MeterBar` | stacked segments, 2px surface gaps, swatch+count labels | 2 |
| `CardFrame` | bordered card; muted-amber attention variant | 2 |
| `TimelineFeed` | vertical line + dots + timestamped entries | 2 |
| graph node/pin/dashed-bezier | extends shader-editor drawlist code; dashed bezier is the one new technique | 3 |

### 11.2 Values table (lifted from the mocks, verbatim)

| Element | Value |
|---|---|
| dock tab strip / toolbar wells / bottom bar | 30px (accepted at the editor-wide ImGui tab-bar chrome's actual ~23px -- ruling 2026-09-07, §17; 30px was the web mock's own value, not a value the panel itself sets) / 24px / 24px |
| table rows / rail rows / group rows | 24px / 26px / 24px (chrome bg) |
| group nesting indent per depth / asset-row indent beneath its band / fold-child indent | 20px per level, leaf-segment group labels (§6, 2026-09-07) / one level (20px) beneath the row's own group's band (§6, 2026-09-07 follow-up) / +20px beyond that — the same 20px unit, applied once more |
| rail / preview pane widths | 180px (fixed) / 165px default, resizable [120px, 480px] (§17, 2026-09-07) |
| row thumb / tooltip thumb / preview thumb | 18px / 64px / 140px |
| tooltip width | 210px |
| pills | 12px text, 16px line, 1px `#333333` border |
| create menu / dialog widths | ~220px, rows 28px / ~380px |
| stat tile number / meter bar | 24px `PushFont` / 10–12px, 2px gaps |
| feed dots | 7px |
| graph nodes | w 180–220, header 24px, accent bar 3px, pins 9px |
| attention frame / amber | `#7a5a20` border / `#ffa61a` (theme `kAmber`) |

### 11.3 Kind-color table (data coding — exempt from the monochrome rule, like the
axis bars; these values are now pinned, no longer placeholder)

| Kind | Hex |
|---|---|
| Texture | `#b06a5b` |
| Material | `#6a9b5b` |
| Mesh | `#5b9bb0` |
| Sprite | `#9b5bb0` |
| Scene | `#b09b5b` |

Used for graph accent bars and pins only in this arc. Fonts: Inter 16px body (the
editor default), 13–14px secondary via `PushFont`, 12px pills. All chrome colors are
`EditorTheme` tokens — the mocks were drawn from them.

## 12. Testing

- **ArcaneTests:** per-format reference extraction against ReferenceProject
  fixtures; the extension-coverage test (§3.2); `MaterialKindFor`; manifest
  round-trip + fallback scan equivalence on the same scene; model fold / counts /
  filters / invalidation headless; index build + incremental update + unused rules
  (exempt kinds, last-known-good); `CreateAssetRequest` validation (name rules,
  uniqueness, subkind filtering).
- **Golden gate:** the editor-ui lane will see the new panel — a re-bless is
  expected (legitimate since Arc 2) and **must be restaged to both hosts**;
  baselines move; `--bless` against the source tree, never the staged build.
- **Desk checkpoint per plan** (house convention), including the §11 side-by-sides.

## 13. Error handling

Parse failures → last-known-good + Problems (§3.2). Create failures →
`ModalErrorQueue` + Problems. Cook refusals stay loud (F2b machinery). The digest
never renders an unknown as a zero — unknown is `—`.

## 14. Rollout and migration

- Plan 1 replaces the panel outright; `AssetBrowser.cpp/.hpp` deleted at the end of
  Plan 1 after helper migration (§4). No legacy toggle.
- One ABI bump (21 → 22) in Plan 1 for both engine queries. This stacks a Game-DLL
  rebuild obligation onto Aphelyon's existing held ABI-21 debt — recorded here, not
  a blocker.
- Scene schema v4 lands in Plan 2; pre-v4 scenes work via the fallback scan and
  upgrade on next save. Fixture population is tiny.

## 15. Out of scope / recorded follow-ups

- Thumbnail tile grid (Option A territory — rejected; could return someday as a
  Browse view mode, deliberately not designed here).
- "Assign to selection" from graph pin-drag (needs entity-slot targeting rules).
- Rename-in-place; new drag-drop targets; panel-state persistence across restarts.
- Dead-cycle detection for unused (§9.1, accepted limitation).
- A freshly-minted-sidecar warning (the forgot-to-commit-the-`.meta` hazard — a
  Status-lens candidate for later).
- Greppable guid serialization in scenes (a JSON-bridge change; separate pass if
  ever).
- Material-subkind attributes on reflection (would retire the field-name/component
  heuristics; when reflection grows attributes).
- An mtime-keyed on-disk cache of parse results (UE's `FDiskCachedAssetData`
  shape: mtime + refs + subkind per asset, invalidated by one timestamp compare) —
  trigger: cold-open parse time becomes noticeable (~5k+ assets). Until then,
  parse-on-open is milliseconds.
- Save-time reference stamping generalized beyond scenes (write outbound refs
  into artifacts/sidecars at save, UE's package-header harvest model) — same
  trigger.
- Time-sliced filtering / async text filter (UE `SAssetView` budget model) —
  UE-scale machinery; not before ~10k assets.

## 16. Decision log

| Decision | Ruling |
|---|---|
| Phasing | one spec, three plans (user) |
| "Unused" semantics | zero inbound refs; cascade accepted; exempt kinds §9.1; cycles accepted blind spot (user) |
| Reference parsing vs indexing | engine parses (owns formats), editor indexes (owns invalidation) — Approach 3 (user) |
| Scene references | save-time `assets` manifest, schema v4; structural scan as one-time fallback (user proposed reorganizing; manifest chosen over bridge changes) |
| Per-file `.meta` sidecars | kept; a central identity map was evaluated and rejected here (identity must survive external file moves; per-asset adds never merge-conflict; one bad write must not wipe every binary asset's identity) — recorded because no prior written evaluation existed |
| Create flows | refactor existing dialogs into the one shared unit; `CreateAssetRequest` bypass invariant (user: "as unified as possible") |
| Material thumbnails | real sphere thumbs in Plan 1 via document-preview harvest (user: fidelity over plumbing cost) |
| Visual fidelity | mocks are the redline; values pinned §11.2–11.3; desk side-by-side per plan (user: "as close to the web-mockup as possible") |

---

## 17. LANDED (Plan 1) — 2026-09-06

Plan 1 landed on Arcane `main` in place, **`59dd6414..2f5dc391`** (Tasks 1–15), plus
this task's gate + baselines catch-up commit. Plan file:
`docs/plans/2026-09-06-asset-manager-plan1-browse.md`. **Not pushed** — held for the
user's desk pass, per house convention.

### Scope landed

§3.1 `MaterialSurfaceFor`, §3.2 `ListAssetReferences`, §3.4 the scene structural
scan, ABI **21 → 22** (one bump, one ledger entry; `ReferenceProject.arcproj`
restamped in the same commit, per the unbroken v17–v21 precedent). §4
`AssetPanelModel`. §5 shell and chrome, including the bottom-bar digest. §6 the
Browse lens — rail, folder-grouped table, preview pane — and §6.1 thumbnails
including live material sphere thumbs. §7 the unified Create flow. §8 the
interaction contract. §11 fidelity values. The old `AssetBrowser.cpp/.hpp` are
**deleted**; their pure helpers moved text-identically into `AssetPanelModel`.

Deliberately absent, exactly as §2 phases them: §3.3 the scene reference manifest
(schema v4), §9 the reference index and Status lens, §10 the Graph lens. The Graph
and Status buttons ship **disabled**.

### Measured close

Three-config build of `Arcane.slnx`, **0 warnings / 0 errors** in Debug, Release and
Dist. Suite counts DERIVED — each pasted from its own run's final line,
`ArcaneTests.exe "~[gpu]"` run FROM the exe directory:

| Configuration | `~[gpu]` | seed |
|---|---|---|
| Debug | 54270 assertions / 1462 cases | 829257050 |
| Release | 54270 / 1462 | 1685340276 |
| Dist | 54202 / 1456 | 3577943350 |

The constant Dist gap (68 assertions / 6 cases, from pre-existing
`#if !defined(ARCANE_DIST)` guards) holds. One **unfiltered** Debug run:
**116544 assertions / 1495 cases**, seed 3082311851 — the difference from `~[gpu]`
is exactly the 33 `[gpu]` cases / 62274 assertions, unchanged by this plan.
`scripts/automation-baselines.json` re-derived to match: **+288 assertions / +33
cases** in every configuration, and the raw `TEST_CASE` count in `ArcaneTests/src`
rose 1475 → 1508 (+33), all inside `~[gpu]`.

**Golden gate**, Debug, both hosts × both backends: `gatePassed: true`, four lanes,
zero red — `ArcaneRuntime/dx12` PassedOnFallback (its documented steady state: there
is no `dx12/runtime-scene.png`, only `vulkan/`), the other three Passed, all four at
`diffCount=0`.

**Gate self-test**, `golden-gate.ps1 -SelfTest`, Debug: **PASSED** — all four lanes
launched and caught the deliberately broken scene by `exitReason=compare-failed`, and
the tree (source plus both staged copies) restored clean afterwards. So the green
above comes from a gate observed *failing* on this tree, not one that merely never
fails.

**The editor-ui re-bless** — the first since 2026-08-30 (`97abd074`) — was expected
and legitimate: the lane diffed against a panel that no longer exists. Before
blessing, the diff artifact was read, and the differing pixels (57428, identically
on **both** backends) fall **entirely inside the Assets panel band** — menu bar,
Outliner, Viewport, Inspector and the Console/Problems tabs show none. `--bless` was
pointed at the **source** tree (never the staged copy, which the next host build
would silently overwrite), then restaged md5-identically to **both** hosts. The
vulkan lane then passed `diffCount=0` against the dx12-blessed **shared** reference,
which is what re-proves editor-ui is backend-invariant rather than assuming it.

### Deviations and rulings recorded during execution

1. **`RowWithThumb` gained a height parameter** (default 24, rail passes 26). §11.2
   pins table rows at 24 *and* rail rows at 26, while §11.1 makes `RowWithThumb` the
   widget behind both — one fixed-24 signature could not serve both.
2. **The "sliced" datum lives on the model entry.** `AssetPanelEntry` gained a
   rebuild-time flag (kind == Sprite, not folded, texture ref of kind `References`);
   §4's entry struct omitted the datum the §6 pill needs.
3. **Cook FAILURE and refusal seams dirty-mark too**, not only the cooked-success
   loop. §5's digest, §6's amber row marker and §13's refusals-stay-loud all bind
   refused state live in Plan 1, so a success-only dirty-mark list was a plan defect,
   not a scope choice.
4. **The visible-material thumb `Request` push lives inside the `resolveAssetThumb`
   seam** (material branch, cache miss → `Request(guid)`), not a new panel-services
   callback. "Visible" is defined as what the clipper resolved this frame; the panel
   contract is unchanged.
5. **Create producers bridge `AssetKind` → `CreateAssetKind`** through the one
   `CreateAssetRequest`. The rail's `+` is gated by `RailKindCreatable`, so rails for
   kinds with no create flow (e.g. Texture, Data) never offer one — the panel's
   browse vocabulary and the create vocabulary are deliberately not the same enum.
6. **A third, unlisted create consumer was retired** — `createInstanceOf` →
   `ShowSaveFileDialog` → `instanceNew` — beyond the consumers §7 named. The
   `CreateAssetRequest` bypass invariant required it: one surviving bypass would have
   made "one request path" false.
7. **The sprite mint-or-reuse notice gates on exactly one match** (`size() == 1`),
   not merely a non-empty result. With 2+ derived sprites an "Open existing"
   affordance would have to guess which, and the editor's never-guess principle
   forbids it.
8. **Keyboard-nav scrolling is gated OFF-SCREEN-ONLY** — a *behavioral* departure.
   The plan prescribed `SetScrollHereY` whenever the selection stamp differs; as
   shipped, the panel scrolls only when the newly selected row is actually off
   screen, so selecting an already-visible row no longer yanks the list under the
   pointer. Adjudicated sound on its merits during Task 10's re-review rather than
   reverted to the prescribed origin-gating.
9. **The refused marker is a thumb-corner badge**, not a prepended glyph — a *visual*
   departure. The plan specified prepending `ICON_LC_TRIANGLE_ALERT` to the row; as
   shipped it occupies its own slot as a 10px badge overlaid on the thumbnail's
   corner, so it cannot displace the name column or desync the row's fixed anchor.
   Its legibility over real thumbnail pixels is a desk item precisely because no
   refused fixture exists to test it headlessly.

### Final-review fix wave (post-Task 16)

A whole-branch review of `59dd6414..218a7ca6` returned one Critical and five
Importants; all six were fixed in one wave, `4bafd062` (engine) and `b61f24c9`
(editor) plus this documentation commit.

**The two engine queries are genuinely parse-on-call as of this wave** — §3's
"no engine-side cache (the editor's index and model are the caches)" was
*specified* in Plan 1 but not *implemented*: both queries routed their JSON
through the facade's cached loader, which keys on the canonical path, never
consults the mtime, and memoizes failures for the process lifetime (nothing
evicts it). Every per-guid re-ask therefore returned the first parse — subkind
pills, `isInstance`, fold/sliced state and the material-picker filter frozen for
the session while thumbnails updated around them — and a file caught mid-save
was latched broken forever, which made §3.2's "retries on the next change event"
unimplementable above the facade. They now read and parse the resolved file on
every call, through a helper that touches neither side of the cache; the cache's
other consumers keep today's parse-once semantics deliberately. Four regression
cases in `AssetReferencesTest.cpp` pin it, each rewriting a file in place and
re-asking through the same `Assets` instance; three of the four were proven RED
against the cached path before the fix.

The editor half: the panel model was never dirtied by the editor's **own**
material saves (`onAssetSaved` re-baselines the watcher mtime, so nothing else
could notice them), nor by crash-report registration — both now `MarkAllDirty`,
which is also what `PollAssetWatch`'s external-material-edit branch now does,
since an instance's surface resolves *through* the edited material's parent
chain and a per-guid mark leaves every descendant stale. And **create failures
now reach Problems** as §7/§13 bind, under an `assets:create` key accumulated
per project beside the existing report and cook accumulators; the modal is
unchanged and the row carries the same message.

**Scoping §7's bypass invariant, for the record.** "No creation path may bypass
`CreateAssetRequest`" governs the *dialog-backed* create paths — the ones §7
enumerates as producers. Two one-click mints stay deliberately outside it,
matching §6's "today's exactly": the row/preview-pane **Create Sprite** quick
action (mint-or-reuse for the selected texture, no name or location to choose)
and the Inspector's **texture-drop auto-mint**. Both are drag/drop-scale
gestures with nothing for a dialog to ask; routing them through the request
would add a modal to a one-click affordance. Everything that names a file goes
through the request, and Task 12 proved that invariant by call-site census.

### Recorded follow-ups (new, on top of §15's list)

- **A pre-existing `DrawModals` re-arm defect**, found during Task 12 and
  deliberately **not** fixed here: the error modal and the Unsaved-Scene modal each
  re-arm every frame and close each other, so under `AlwaysAutoResize` both can end
  up permanently invisible. It predates this plan; the create dialog is gated behind
  an empty error queue to route around it.
- Keyboard: **`KeypadEnter` is not bound** alongside `Enter`; **`Enter` does not
  submit the create dialog**.
- The rail `+` is painted outline-only (no filled body, no pressed state).
- Context `Create ▸` does not prefill per row (the `Create Sprite` quick action is
  the prefilled path).
- `ConsumeBrowserActions` / `browserActions` keep "Browser" in their names now that
  `AssetBrowser.*` is gone.

### Owed, and deliberately held

The ABI 21 → 22 bump stacks a second Game-module rebuild obligation onto Aphelyon's
already-held ABI-21 debt (§14 recorded it as recorded-not-blocking). Gacha `main`
stays at `5923da65`.

### Post-landing user-directed changes — 2026-09-07

The preview pane changed from a pinned 330px to a **165px default, user-resizable
via a drag splitter** (session-only state, clamped [120px, 480px]; the splitter
itself yields to the existing <720px hide rule) — §5 and §11.2 above are the
edited arbiter; the redline mocks still show 330px and were **not** re-rendered.

The automated mock-vs-editor Browse comparison (`.superpowers/sdd/2026-09-06-
asset-manager-plan1/compare/COMPARISON.md`) surfaced five small deviations; the
desk pass ruled on each:

- **Dock tab strip ~23px accepted as-is** against §11.2's pinned 30px (see the
  parenthetical added there). It is not the panel's own metric — it is ImGui's
  editor-wide dock tab-bar chrome, shared by every panel — so this is a ruling to
  leave it, not a fix.
- **A "Name" column header band was added**, chrome-toned like the folder-group
  rows (`DrawGroupRow`'s own idiom, reused rather than ImGui's
  `TableSetupColumn`/`TableHeadersRow` mechanism, whose row height is
  font/CellPadding-driven rather than the pinned 24px this panel assumes
  everywhere else), frozen at the table's top edge via
  `TableSetupScrollFreeze(0, 1)`. The mock drew one; the shipped panel had none.
- **Group-row counts moved inline**, immediately after the group name (dim,
  small gap) — the mock's own placement — replacing the right-aligned reading of
  §11.1's "right-aligned extras" that `DrawGroupRow` had followed literally.
  `RowWithThumb`'s own trailing-pill convention (asset/child/rail rows) is
  unchanged; this was `DrawGroupRow` only.

### 2026-09-07 user-directed: in-table nested folder groups (design pass)

**In-table nested folder groups** (middle form between the flat groups and Option
A's rejected tree panel): the table's folder groups gained indentation and
cascading collapse for nested directories, entirely inside the existing single
table — no separate tree panel, no breadcrumbs (§1, §6, §11.2 amended above). The
`OptionBC.dc.html` FINAL board (Browse lens) was amended to match — a
`textures/patterns/` depth-1 example group (`tiles_stone.png`, `noise_blue.png`)
was added by regrouping two existing texture rows rather than inventing net-new
sample data, keeping the added-row footprint to one nested group header so the
960×620 framing stays unclipped — and `renders/OptionBC-Browse-FINAL.png` was
re-rendered against it; the README binding table records the amendment. This is a
**design-only pass**: the live panel does not yet implement nested groups; a
follow-up implementation task lands the behavior against this spec text and the
amended mock.

### 2026-09-07 implementation + review fix round 1: in-table nested folder groups

The follow-up implementation task landed against the design pass above
(`AssetPanelModel`/`AssetsPanel` — see `.superpowers/sdd/2026-09-06-asset-manager-
plan1/followup-treeview-impl-report.md` for the full record), then a review pass
returned 1 Critical + 4 Important findings, fixed in one round on top of it. The §6
paragraph above is the ALREADY-CORRECTED text (this entry records what changed and
why, not a second copy of the rule):

- **Critical 1 — ancestor bridge is now unconditional, not search-gated.** The
  first cut only synthesized a zero-own-count ancestor group row while a search was
  active. A kind filter alone (no search) routinely produces the identical shape —
  an ancestor with zero own entries under the active filter, a descendant with some
  — and left it genuinely unreachable: no bridge row meant no chevron to reopen a
  stale-closed ancestor, while the rail still counted the (invisible) descendant
  asset. Bridging now happens unconditionally; `searchActive` still governs
  whether COLLAPSE itself is bypassed (that part was already correct), just no
  longer whether the bridge ROW exists at all.
- **Important 4 (controller ruling) — search now also overrides FOLD collapse.**
  Before this round, a matching derived sprite under a collapsed texture stayed
  hidden during a search that correctly revealed matches under a collapsed folder
  — an inconsistency, since both are "collapse" in the same sense. Search now
  reveals a matching fold child regardless of the texture's own `childrenOpen`
  flag, uniformly with group collapse. §6's own wording is corrected in place
  (above) to state this as one uniform rule rather than citing a fold-child
  precedent that, on inspection, had never itself shipped a search override before
  this round — that citation was aspirational, not historical, and is retracted.
- **Important 2 (+ its DrawAssetRow twin, taken as a rider) — honest chevrons under
  search.** With content now rendered regardless of the real open/collapsed flag,
  painting that same stale flag on the chevron glyph made the toggle look inert (a
  right-pointing "collapsed" arrow sitting directly above visibly-expanded rows).
  The chevron (group rows, and — by the same bug class — the texture-row fold
  expander) now paints an EFFECTIVE open state (`realFlag || searchActive`)
  display-only; clicking still flips the real, persisted flag, which simply has no
  visible effect until the search box clears.
- **Rider 8 — bridge rows suppress a literal `0`.** A zero-own-count bridge row
  (Critical 1) no longer paints a bare `0` next to its label, which read as "this
  group is empty" directly contradicting the visible rows beneath it. A real,
  populated group's count is unaffected, including a genuine `1`.
- **Important 5 — behavior change, flat projects included (recorded, not new to
  this round).** `showChildren`'s `searchActive || GroupOpenOrDefault(...)` form
  never special-cased depth — it applied to EVERY group, top-level (depth 0,
  "flat") groups included, from the original nested-groups implementation
  onward. That implementation's own report discussed it only in nested terms and
  never called out that a genuinely flat, unnested project is affected too:
  searching while a top-level group is collapsed shows its matching rows, where
  the panel's ORIGINAL (pre-nesting) Plan 1 behavior left the group's header
  visible but its contents hidden, exactly as if the search had not run. No test
  ever pinned this either way. This review is what surfaces and records it —
  Important 4, landing in this same round, is a DIFFERENT, genuinely new
  extension of the identical principle to fold (derived-child) collapse, not the
  source of the group-level behavior described here. Test (ii) in the impl
  report's fix-round addendum is this case's regression pin.

### 2026-09-07 user-directed, second revision: root-anchored folder tree (design pass)

The user's own ruling: "I want the entire table to have indention status,
showing folder hierarchy. i think that is best now." This supersedes the first
revision's shape (top-level directories as depth-0 peers of `Content/`) with a
strictly **root-anchored** tree: `Content/` becomes the real depth-0 root of the
whole table — the root files stay its direct rows, and every top-level directory
becomes an indented depth-1 child of it (a directory's depth is now `1 + its
nesting below Content/`, so `materials/` is depth 1 and `textures/patterns/`,
already one level under `textures/` from the first revision, is now depth 2).
§6 above is the ALREADY-AMENDED text. Collapse cascades from `Content/` over
everything, since it is now every directory's ancestor; the existing unconditional
ancestor bridge (this same section, review fix round 1) is what keeps `Content/`
itself always rendering, count-suppressed, even in a project with zero loose root
files — `Content/` is simply the bridge's most-unconditional case, not a new
mechanism. Labels are unchanged (`Content/` keeps its name; nested directories
already showed leaf segments as of the first revision). §11.2's 20px/level value
is unchanged; only which depth each existing group sits at moved by one level
(`patterns/` by two, since it nests below a directory that itself moved).

The `OptionBC.dc.html` mock was amended a second time the same day: every
existing group and its rows below `Content/` (`materials/`, `meshes/`, `scenes/`,
`sprites/`, `textures/`, and the nested `patterns/`) shifted 20px deeper —
`Content/` and its own `sample.asset.json` row are unchanged, still depth 0. Row
count is identical to the first revision (only indent values changed), so the
960×620 framing held with no reflow or clipping, verified by re-rendering and
viewing the PNG. This is a **design-only pass**: the live panel implements the
first revision's (non-root-anchored) shape as of the review-fix-round-1 landing
above; a follow-up implementation task is owed to move it to root-anchored.

**Correction, recorded after the fact:** root-anchoring *did* land in code
shortly after this design pass, in `5be0302b` — `GroupParentOf` now resolves
every top-level directory's parent to `Content/` (was `""`), plus
`GroupDepthOf`'s matching `1 + nesting` formula; cascading collapse, the
unconditional ancestor bridge, its suppressed-zero count, search-overrides-
collapse, and the panel's `groupDepth * kGroupIndent` math all fell out of the
existing machinery unchanged once `Content/` became every directory's ancestor.
So the "owed" line above is stale as of that commit — recorded here rather than
edited away, so the design-pass record still shows what was true at the moment
it was written.

Not pushed, per house convention.

### 2026-09-07 user-directed, third revision: mount-rooted asset tree (design pass)

The user's own ruling, approved by the coordinator: the table becomes
**mount-rooted** — one depth-0 root group per mount that holds registered
assets, honest about locations, rather than one universal root. This is layered
on top of the second revision's root-anchoring, not a reversal of it:
`Content/` (`game://`) keeps everything the second revision gave it (real
depth-0 root, root files as direct rows, every directory below it nested and
indented, default-**open**) — the change is that `Content/` is no longer
*the* root, it is *a* root, first among peers. §5 and §6 above are the
ALREADY-AMENDED text (this entry records what changed and why):

- **`diagnostics/` (`diag://`) is a new sibling depth-0 root**, appearing after
  the entire `Content/` subtree, ordered lexicographically among any other
  populated mounts (`Content/` itself always sorts first, by convention, not
  alphabetically — it is the primary mount). Depth within `diagnostics/` follows
  the same `1 + nesting below its own mount root` formula §6 already established
  for `Content/`, just anchored at a different root.
- **`diagnostics/` defaults COLLAPSED** — recorded in §5 as the one exception to
  groups-default-open. This is the entire point of the mount-rooted shape: crash
  reports (`.arcdiag` files) were previously either invisible to the tree
  entirely or, worse, would have needed synthetic bucketing into `Content/` to
  show up at all (dishonest about where they actually live, on a different
  mount). A collapsed sibling root gives them exactly one quiet row —
  `diagnostics/ N` — until someone deliberately opens it.
- **Future mounts follow the identical pattern.** `engine://` or `plugin://`
  would each get their own sibling depth-0 root, default-open (only
  `diagnostics/` is pinned collapsed; a future mount's default is a decision for
  whoever adds it, not inherited from this ruling), the moment they register at
  least one asset. A mount that registers nothing never gets a row — the
  ancestor bridge's unconditional rendering applies to a mount root with a
  populated descendant, not to every theoretically-possible mount.
- **No more synthetic bucketing.** Before this revision, the only honest way to
  represent a `diag://` asset in a `Content/`-only tree would have been to fake
  it as a `Content/` subdirectory or drop it from the panel; mount-rooting
  removes that dishonesty at the root of the table, not just for this one mount.

The `OptionBC.dc.html` mock was amended a third time the same day: one row
added — a collapsed `diagnostics/ 3` root band (right-pointing chevron, dim
count, depth 0, same base indent as `Content/`) placed immediately after
`Content/`'s entire subtree. To hold the fixed 976×640 frame with no clipping
(the frame lesson from the first revision, re-applied), the `patterns/` demo
group was trimmed from two rows to one (`noise_blue.png` dropped, `tiles_stone.png`
kept, count pill `2` → `1`) — a net-zero row-count change, not a reduction in
what the shape demonstrates. Verified by re-rendering and viewing the PNG; the
new row and its collapsed-chevron affordance read clearly, and no earlier row
was clipped.

This is a **design-only pass**: the live panel does not yet have a
`diagnostics/` mount root or any per-mount-root default-collapsed behavior — a
further follow-up implementation task is owed, on top of the still-current
root-anchoring implementation (`5be0302b`) this revision builds on.

**Correction, recorded after the fact:** mount-rooting *did* land in code
shortly after this design pass, in `4a9f3cb0` — one depth-0 root group per
populated mount, `diagnostics/` a sibling of `Content/` ordered after it,
default-collapsed, with an explicit keying fix so a real `game://diagnostics/`
directory can never alias the `diag://` mount's own synthetic root. A further
commit, `c0ca6edc`, then changed asset-row indentation per direct user
follow-up ("for the rows to be indented starting at their icons, so the row is
farther indented than it already is"): asset and fold-child rows now sit one
full 20px level beneath their own group's band rather than flush with it (a
row under a band at indent X now starts at X+20; fold children at X+40). **The
`OptionBC.dc.html` mock was not updated for this row-indent refinement** — it
still draws asset rows flush with their group band's own indent, matching
every revision through this one, not the live panel's current X+20 shape. This
is flagged rather than silently fixed here because it is a geometry change
outside this pass's scope (compact preview header only) and, per this
document's own pattern, geometry changes to the redline have each gotten their
own numbered design pass and explicit sign-off; it should get the same
treatment rather than be folded into an unrelated revision's commit.

### 2026-09-07 user-directed, fourth revision: compact preview header (design pass)

The user, with a screenshot of the live panel: the preview sidebar needed
scrolling while the space to the right of the thumb sat wasted. §6's preview-
pane bullet above is the ALREADY-AMENDED text. Pinned shape: the header goes
side-by-side (140px thumb left, metadata block right — name, kind/subkind
pills, then path, then guid, same content and click-to-copy/tooltip behavior
as before); `Derived (N)` and the action buttons stay below, unchanged; the
thumb keeps its existing `min(140, avail)` scaling; below roughly 250px of
pane width (implementer-tunable, not pinned) the header falls back to the
prior stacked form, so the 165px default pane is unaffected — this only pays
off once the user drags the pane wider, exactly the reported screenshot's
situation. §11.2's 140px thumb value is unchanged; this revision is
arrangement, not sizing.

The `OptionBC.dc.html` mock was reworked at the board's 330px pane width: the
thumb (previously 160px, centered above the metadata in its own row — a
pre-existing minor deviation from §11.2's pinned 140px that this revision
happened to correct as a side effect of rebuilding the header) moved to a
140px flex-none box on the left of a new row, with the name/pill row and the
path/guid block stacked in a `flex: 1` column beside it. Because the combined
metadata column (name row + path/guid block, roughly 80px tall) is shorter
than the 140px thumb, the row's total height is thumb-bound at 140px — down
from the old stacked total of roughly 250px (160px thumb + gaps + name row +
path/guid block) — a net vertical savings of roughly 100px, freeing the exact
kind of headroom the reported live-panel bug needed. This **deletes** rows
rather than adding them, so there was no frame-overflow risk this round;
verified by re-rendering and viewing the PNG anyway, per house discipline.

One defect caught and fixed during the same pass, before considering it done:
the first render showed the `path` value (`textures/uv_marker.png`) clipped
hard against the pane's right edge, since the compact metadata column is only
about 154px wide at 330px pane width and nothing constrained the value span to
that width. Fixed with `min-width: 0` on the containing flex rows/columns plus
`overflow: hidden; text-overflow: ellipsis; white-space: nowrap` on the path
value span, so a too-long path now truncates with an ellipsis (matching the
guid row's own existing `…` convention) instead of spilling past the pane
boundary. Re-rendered and re-viewed to confirm the fix.

The mock's preview pane never drew a `cook` line (only name/pills, path, and
guid) in any prior revision — §6's own text has always listed one (`path,
guid, cook line`), but the fixture asset in the mock is a healthy, already-
cooked texture with nothing to show there. This revision did not add one: the
brief asked to rearrange existing content, not introduce new content, so the
absence is unchanged from every earlier board, not a new gap.

This is a **design-only pass**: the live panel's preview pane still stacks
thumb-above-metadata at every pane width as of `c0ca6edc`; a follow-up
implementation task is owed to add the side-by-side header and its
narrow-width fallback.

---

## 18. LANDED (Plan 2) — 2026-09-07

Plan 2 landed on Arcane `main` in place, **`d12a9722..66344350`** (Tasks 1–8), plus
this task's re-bless (`860ab115`) and gate + baselines catch-up commit. Plan file:
`docs/plans/2026-09-07-asset-manager-plan2-status.md`. **Not pushed** — held for the
user's desk pass, per house convention.

### Scope landed

§2's Plan-2 line in full. §3.3 the **scene reference manifest**: the save-time
top-level `"assets"` block, identity fields excluded through the shared
`IsIdentityGuidFieldName` rule (promoted to one header so the emitter and the scan
cannot drift), nils dropped, distinct and sorted — emitted as a byproduct of the
writer's existing visit via a writer sink, not a second pass, and **always** emitted
even when empty. The scene *loader* never reads it. The version gates widen to accept
`{3, 4}` at exactly two sites; v2 is still refused. ABI **22 → 23**, with
`ReferenceProject.arcproj` restamped in the same Task-1 commit.

§3.4 the **structural scan demoted to the pre-v4 fallback**: `ListAssetReferences`
takes the manifest fast path for a v4 scene that has one, and falls back to the scan
for pre-v4 scenes *and* for a v4 scene whose manifest is absent.

§9.1 the **`AssetReferenceIndex`**: forward and inverted maps, inbound counts, UE's
remove-before-re-add incremental discipline (the removal pass iterates the OLD
outbound before the overwrite), tombstones for deleted and unresolvable targets with
GC on both letting-go paths, last-known-good on `nullopt` per §3.2. **Unused = zero
inbound**, over eligible kinds exactly `{Texture, Material, Sprite, Mesh}`. The index
lives inside `AssetPanelModel`, fed by the model's own `refsFor` asks — one ask per
rebuilt guid, held in a local and handed to both the entry build and `Update`, so
Plan 1's call-count pins stay green unmodified.

§9.2 the **Status lens, complete and ENABLED**: stat tiles (assets / cook refused /
awaiting cook / unreferenced), the stacked cook-pipeline meter, Needs-attention cards
(refused in the muted-amber acting-on frame with **Recook** and **Problems**; queued
with its progress strip), the Unreferenced card with inset-well chips and **Reveal**,
the Activity feed, and the Scenes rollup. **Focus in Graph** ships disabled, as §9.2
binds. §11.1's Plan-2 widget rows — `StatTile`, `MeterSegment`/`MeterBar`,
`BeginCardFrame`/`EndCardFrame`, `TimelineEntry`/`TimelineFeed` — live in the shared
`EditorWidgets` layer, not panel-local. The **`AssetActivityLog`** is the session-only
~100-entry ring §9.2 specifies, pushed by ten event seams, every push riding *beside*
an unchanged dirty mark rather than replacing one. §13's "the digest never renders an
unknown as a zero" is now satisfied by the count being **real** rather than suppressed.

Deliberately absent, exactly as §2 phases it: **§10 the Graph lens** (Plan 3). The
Graph button still ships **disabled**.

### Measured close

Three-config **rebuild** of `Arcane.slnx`, **0 warnings / 0 errors** in Debug, Release
and Dist. Suite counts DERIVED — each pasted from its own run's final line,
`ArcaneTests.exe "~[gpu]"` run FROM the exe directory:

| Configuration | `~[gpu]` | seed |
|---|---|---|
| Debug | 54908 assertions / 1510 cases | 4183101686 |
| Release | 54908 / 1510 | 2841737635 |
| Dist | 54840 / 1504 | 354211894 |

The constant Dist gap (68 assertions / 6 cases, from the pre-existing
`#if !defined(ARCANE_DIST)` guards) still measures exactly 68/6. One **unfiltered**
Debug run: **117182 assertions / 1543 cases**, seed 2374230885, all passing — the
difference from `~[gpu]` is 62274 assertions / **33** `[gpu]` cases, and that 33 is
stated from measurement (1543 − 1510), unchanged by this plan.

`scripts/automation-baselines.json` re-derived. Its figures were stale **twice** over —
measured at Plan 1's close (`2f5dc391`) and never re-derived when the 2026-09-07
follow-up waves landed — so the note now attributes the +638 assertions / +48 cases in
two separately-booked waves: **(a)** the 09-07 follow-ups, pre-Plan-2 (`+274`/`+20`:
`AssetPanelModelTest` 14 → 30, `AssetReferencesTest` 13 → 17), and **(b)** Plan 2
(`+364`/`+28`: the new `AssetReferenceIndexTest` (9) and `AssetActivityLogTest` (3),
plus `AssetReferencesTest` +5, `AssetPanelModelTest` +6, `SceneAssetTest` +4,
`SceneJsonTest` +1). The raw `TEST_CASE` count in `ArcaneTests/src` rose 1495 → 1515 →
1543, and `~[gpu]` cases rose by exactly that same +48, so all 48 fall inside the
filter and neither wave added GPU coverage. The identity `raw TEST_CASE − 33 = ~[gpu]
cases` holds at all three points, which is what lets both waves' case halves be
attributed from git alone. Verified against the file's real consumer:
`check-baselines.ps1` reports `+0` on both metrics, exit 0.

**Golden gate**, Debug, both hosts × both backends: `gatePassed: true`, four lanes,
zero red — `ArcaneRuntime/dx12` PassedOnFallback (its documented steady state), the
other three Passed, all four at `diffCount=0`.

**Gate self-test**, `golden-gate.ps1 -SelfTest`, Debug: **PASSED** — `selfTest: true`,
`gatePassed: false`, all four lanes caught the deliberately broken scene by
`exitReason=compare-failed`, and the tree restored clean afterwards (`git status
--porcelain -- ReferenceProject` empty). So the green above comes from a gate observed
*failing* on this tree.

**The editor-ui re-bless** was expected and legitimate: the lane diffed against a
Browse-only toolbar, from a golden blessed *this same morning* at `d12a9722`. Before
blessing, the diff artifact was read. The differing pixels (2411, and the two
backends' diff PNGs are **byte-identical**, md5 `dd5e03a3…`) fall in exactly two
clusters, both **inside the Assets panel band**: the lens strip's Status button
(`x[763..809] y[570..593]`, now enabled rather than disabled-dim) and the bottom-bar
digest (`x[586..808] y[697..711]`, now `0 unused` rather than `— unused`). **Zero**
marked pixels fall outside `x[240..810] y[543..720]` — verified programmatically, not
only by eye. `--bless` was pointed at the **source** tree, `exitReason=compare-blessed`,
then restaged to **both** hosts with all 13 `Verify/` files md5-verified identical
across source and both staged copies. The vulkan lane then passed `diffCount=0` against
the dx12-blessed **shared** reference, which re-proves editor-ui backend-invariant.

### Deviations and rulings recorded during execution

1. **ABI 22 → 23 — a SECOND bump, deviating from §14.** §14 says "One ABI bump
   (21 → 22) in Plan 1". Plan 2's scene-format move takes a second. The bump is
   *documentary* (no vtable change; the writer's sink member is a layout change, but
   the header-only argument carries it), and it is mandatory under the v16
   scene-format-move precedent plus the standing cheap-bumps rule. §14's wording is
   left as written; this is the correction. Cost: one more Game-DLL rebuild
   obligation on Aphelyon's held pile — recorded, not blocking.
2. **The manifest is a strict SUPERSET of the structural scan, not its equal.** The
   scan applies §3.4's resolvability filter; the manifest path deliberately does
   **not**, so it reports references to targets that no longer resolve. That
   asymmetry is load-bearing, not an oversight: §9.1's tombstones exist precisely to
   answer "who referenced the missing asset", and a resolvability filter on the fast
   path would starve them. The equivalence test (§12) therefore pins the two against
   a scene whose targets all resolve; a `ReflectionJson.hpp` comment claiming the two
   simply "agree" overstates it and is corrected by this paragraph.
3. **The queued card gained an "N of M cooked" caption** (dim, 13px, beneath the
   progress strip — there was no room beside it). `arccook` is ONE whole-project
   coalescing run, so the global fraction genuinely *is* every queued asset's honest
   pipeline position; the defect the reviewer found was unlabeled semantics, not a
   fabricated per-asset number. The board draws the strip without the caption, so the
   caption is board-over-brief and recorded here as such.
4. **The refused card keeps the board's whole line.** Authority is the BOARD WHOLE
   (§11's mocks-are-redline): the card keeps `cook refused — <detail>` **and** adds
   the dim `· details in Problems` tail, rather than trading one for the other.
5. **Reveal expands more than it selects.** §9.2 says Reveal "switches to Browse,
   selects". As shipped it also clears the search and kind filter, flips the lens,
   **expands every ancestor group of the revealed guid including its mount root**,
   and **opens the revealed row's own fold** if it is a derived child. Without those
   two expansions a collapsed group or a collapsed texture fold hides the row after
   the clears — an explicit Reveal click that visibly does nothing. Accepted cost:
   groups the user had deliberately closed pop open on that click.
6. **The index piggybacks the model's ACTUAL invalidation granularity.** §9.1 says
   the index "re-walks single guids on save/watcher/create/delete events", but most
   of those seams were deliberately widened to `MarkAllDirty` during Plan 1's final
   fix wave (parent-chain correctness). The index therefore does a full re-feed on
   `MarkAllDirty` and a per-guid walk on `MarkDirty`. Ruled **spec-spirit-compliant**
   rather than a deviation: the full re-feed costs exactly what Plan 1's full rebuild
   already cost, and narrowing the seams back would reintroduce the staleness bug
   that widened them.
7. **`TimelineFeed` gained a return value.** Task 6 shipped it `void`, which could not
   serve §8's per-row tooltip and click contract. Task 8 extended it to return
   `TimelineFeedResult { hoveredIndex, clickedIndex }`; drawing is unchanged and Task 8
   is the sole caller.
8. **Dangling references stay data-only — no UI was invented.** The index exposes
   `DanglingTargets()` and §9.1 calls the tombstone list "exactly the 'dangling
   reference' list the Status lens can report", but §9.2's card inventory does not
   name a dangling card. None was added. The data is there for Plan 3 or a later
   Status revision to surface deliberately, rather than a card designed at the desk
   during implementation.

### Degradation and known blind spots

**§13 degradation, recorded and parked.** If the model's `refsFor` provider is absent,
every asset reads as zero-inbound and the unused count would *over*-report. That state
is **unreachable in production**: `MakeAssetPanelProviders`
(`EditorAppProject.cpp:753-778`) always assigns the lambda. A per-guid facade
`nullopt` is a different and accepted case — §3.2's one-rebuild-flicker class, covered
by last-known-good.

**Known desk-only branches.** The refused attention card and the cards' empty states
are **never rendered by any automated run**: `ReferenceProject` has zero refused
artifacts, so no headless capture and no golden exercises those paths — they are
inspection-verified only. Breaking a texture cook at the desk is the checklist item
that closes this. This is the same accepted risk Plan 1 recorded for its refused
thumb-corner badge, and the same reason Tasks 7/8 shipped with no headless tests:
`AssetsPanel.cpp` is not compiled into `ArcaneTests`, so the lens's logic is covered
through the Task 3/4/5 pure units and the render/desk comparisons instead.

### A consequence of the ABI bump, found at close

The ABI 22 → 23 restamp made `ReferenceProject/Binaries/ReferenceGame.dll` — built at
14:08, before the bump at 14:37 — a stale **ABI-22** module. The engine's plugin gate
refused it (`Plugin.cpp`'s `AbiMismatch`), which sank `plugin_load`. Two consequences,
both found and fixed here rather than at the desk:

- The two `[witness][gpu]` host scenarios (W1 bounds-spent, W3 missing-reference) went
  red in the unfiltered run — the *only* suite that launches a real host, and so the
  only one that could see this. Rebuilding `ReferenceProject.slnx` for Debug turned
  both green (40 assertions / 2 cases). `Binaries/` is untracked, so the repair is a
  local build-artifact fix with no commit.
- **The "Open Project Failed" modal that Tasks 7 and 8 recorded in their headless
  captures as "pre-existing, environment-level" was this, and is now resolved.**
  `EditorApp.cpp:986-993` pushes exactly that modal, with a banner naming the required
  ABI, when `plugin_load` fails (the stage is Optional for the editor, so it boots on
  and surfaces the failure as a modal instead of aborting). It does not appear in this
  task's captures. `golden-gate.ps1` never saw it because the script's own step one is
  a `ReferenceProject.slnx` rebuild for the target configuration — the single-slot
  `Binaries/` precondition its header documents.

### Dated corrections to §17 — 2026-09-07

Recorded here rather than by editing §17, so its record still shows what was true when
written:

- §17's **"Owed, and deliberately held"** says the ABI 21 → 22 bump "stacks a *second*
  Game-module rebuild obligation" onto Aphelyon. With ABI 23 it is now a **third**.
  Gacha `main` still stays at `5923da65`; still recorded, still not blocking, still not
  to be nagged about.
- §17's **"Measured close"** figures (54270/1462, 54202/1456, unfiltered 116544/1495)
  are superseded by this section's table. They were correct at `2f5dc391` and are left
  as the Plan-1 record.
- §17's fourth-revision **compact preview header** is still genuinely owed — Plan 2 did
  not touch the preview pane, and the "follow-up implementation task is owed" line
  there remains accurate, unlike the second and third revisions' own owed-lines which
  their corrections already retired.
