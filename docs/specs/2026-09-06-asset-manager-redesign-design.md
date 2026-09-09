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
9. **`Recook` is a five-step sequence, not a cook call**
   (`EditorAppFrame.cpp:2421-2437`): `InvalidateArtifact(guid)` → erase the guid's row
   from `m_cookDiagnostics` → `PublishCookDiagnostics()` (which is *how* the Problems
   row clears — the publication-group contract republishes the whole set, so an erased
   row actually disappears) → `m_cookQueue->NoteChanged()` (non-blocking; it only
   submits a background `CookProject` pass) → `m_assetModel.MarkDirty(guid)`. An
   activity entry is pushed **beside** the dirty mark, never instead of it, with kind
   `SourceChanged` and detail `"recook requested"` — the honest kind, since the user
   asked for exactly what a source edit asks for.
10. **`Problems` surfaces the pane and does nothing more**
    (`EditorAppFrame.cpp:2438-2448`) — un-hide, then `SelectDockTab("Problems")`, the
    same two-step Edit ▸ Rename uses for the Outliner (a panel merely visible but
    buried behind a sibling tab is not surfaced). **No pre-filtering to the clicked
    asset**: §9.2 says "jumps to the pane", so none was invented.
11. **`AssetActivityKind::Deleted` has no live producer.** The kind exists and the feed
    renders it (`AssetsPanel.cpp:2072`), but no seam pushes it — a repo-wide search
    finds that label switch as its only use. Deliberate, and **user-visible**: nobody
    should read the feed's silence on deletions as a bug. The desk checklist says to
    expect it.

### Degradation and known blind spots

**§13 degradation, recorded and parked.** If the model's `refsFor` provider is absent,
every asset reads as zero-inbound and the unused count would *over*-report. That state
is **unreachable in production**: `MakeAssetPanelProviders`
(`EditorAppProject.cpp:818-843` as of this close, with the `p.refsFor` assignment at
`:825` — unconditional, no branch that can leave it empty) always assigns the lambda.
A per-guid facade `nullopt` is a different and accepted case — §3.2's
one-rebuild-flicker class, covered by last-known-good.

**Known desk-only branches.** The refused attention card and the cards' empty states
are **never rendered by any automated run**: `ReferenceProject` has zero refused
artifacts, so no headless capture and no golden exercises those paths — they are
inspection-verified only. Breaking a texture cook at the desk is the checklist item
that closes this. This is the same accepted risk Plan 1 recorded for its refused
thumb-corner badge, and the same reason Tasks 7/8 shipped with no headless tests:
`AssetsPanel.cpp` is not compiled into `ArcaneTests`, so the lens's logic is covered
through the Task 3/4/5 pure units and the render/desk comparisons instead.

**A measurement asymmetry, recorded so it is never mistaken for a defect.** The
`diag://` mount is **not mounted under a headless compare/report run**:
`OpenOptionsFor` sets `mountDiagnostics = !(headless && (compare || report))`
(`ProjectBoot.hpp:361-367`). So a gate capture, a `--bless`, and the golden itself see
`Content/` only — the blessed `editor-ui.png` reads `8 assets` — while an **interactive**
session mounts `diag://` and shows the accumulated crash/hang reports as their own
`diagnostics/` root, with correspondingly larger totals in the tiles and the digest.
This is what makes the golden **stable as diagnostics accumulate**, and it is why the
Status lens captures taken during Tasks 7/8 (`--screenshot`, no compare) show 38 assets
against the golden's 8. Both numbers are right; they are different populations.

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

### Desk-pass fix — "2 awaiting cook", permanently — 2026-09-08

Found by the user at the desk on the Plan-2 tree: the Status lens read **2 awaiting
cook** on `ReferenceProject` and never moved. Not a lens defect — the number was the
honest render of a wrong answer from the host.

**Root cause.** `EditorApp::IsCookPending` (`EditorAppProject.cpp`) answered *pending*
whenever `m_cookDiagnostics` held **no row** for the guid. That map records failures
and refusals only, and a cook **success ERASES** a row — so a healthy asset never has
one, and nothing could ever move a cooking-kind asset (`Texture`/`Sprite`, per
`CookStateOf`) to `Cooked`. `ReferenceProject` has exactly two such assets
(`uv_marker.png`, `uv_marker.arcsprite`), hence the constant 2. The same presumption
also parked queued cards forever and put every asset **back** into `Queued` immediately
after a successful re-cook.

**The fix.** On an absent row, ask the artifact store instead of presuming:
`CookSession::ResolveCurrentArtifactPath(projectDir, guid)` — the read-only helper that
recomputes *today's* cook key from the source's current bytes + settings + importer
version and answers only if a file already exists at that key. Artifact resolves →
`Cooked`; does not → `Queued`; no project in hand → the old presume-pending default
survives as the fallback. Row-present branches are unchanged (transient
`ArtifactMissing` → pending, permanent → `Refused` via `HasPermanentCookDiag`, which
`CookStateOf` checks first). Deliberately **not** routed through
`AssetsFacade().ArtifactFor()`: the facade's Missing branch consults `QuietlyPending` →
the installed `SetCookPendingProbe`, an editor closure in the same object — the
cook-pending seam would be asking itself. The local `CookSession` is also **not**
`CookQueue`'s own session, which is worker-thread-only by that class's contract.

**Sprites derive through their texture.** Verified in the pipeline, not assumed:
`CookSession::EnumerateTextureSources` takes `.png` **with a `.meta` sidecar** and
nothing else, and `ReferenceProject/Intermediate/Artifacts` holds exactly **one**
`.arcart` for its one `.png` (`arccook` on the same tree reports
`cooked=0 upToDate=1 failed=0`). A `.arcsprite` has no artifact of its own — it renders
through its texture's — so a sprite's cook state is resolved through
`FirstTextureRefOf(guid)`, the editor's single guid → outgoing-refs path. A sprite whose
texture ref cannot resolve keeps the presume-pending default.

**Cost, recorded in §15's trigger style.** The oracle enumerates `Content/` and hashes
the matching source's bytes per ask. `IsCookPending` **takes the asset's `kind` and gates
on it**, so an ask is paid only for a **cooking kind** (`Texture`/`Sprite`) — once per such
entry per model **rebuild** (invalidation-driven, not per-frame), plus the render oracle's
already-throttled `PendingCook` re-poll. Every other kind is answered `false` for free.
Trigger: if a cold rebuild over a large `Content/` ever shows in a frame trace, memoize
per `(guid, source mtime)` — never by making the answer less honest.

**Testing, honestly.** `CookStateOf` is pure and **unchanged**; its Plan-1 pins stay
green untouched. `IsCookPending` lives on `EditorApp`, which is not compiled into
`ArcaneTests` — **no headless pin is possible for it**, the same blind spot this section
already records for `AssetsPanel.cpp`. What *was* strengthened is the oracle's own
contract, in the existing `[pipeline]` source-edit test: between a source edit and its
recook `ResolveCurrentArtifactPath` must answer `nullopt` (the pre-edit artifact is
still on disk at that instant), and must resolve to the **new** key once the cook lands
— the exact `Queued → Cooked` transition the badge now reads. No new infrastructure.

Comments that documented the old default were corrected in the same commit rather than
left to lie: `OnArtifactRefused`'s `permanent=false` note, `HasPermanentCookDiag`'s,
`IsCookPending`'s and `MakeAssetPanelProviders`' declarations, `CookStateOf`'s kind
gate, the `SetCookPendingProbe` install site, and both Recook sites
(`EditorAppFrame.cpp`, `AssetsPanel.hpp`) whose "erasing the row flips the card to
Queued" rationale now names the artifact-store re-derivation instead of the
presumption.

#### Review round 1 — three fixes before the bless

The ruled design was implemented faithfully, but the review found three things the first
cut got wrong. All three landed in a second commit; the design is unchanged.

1. **The oracle ran for EVERY kind.** `cookStateFor` evaluated `IsCookPending(g)` eagerly
   and `CookStateOf`'s kind gate discarded the answer for a non-cooking guid *after* the
   work was done — and for a guid no `.png` carries, that work is the worst case: walk all
   of `Content/`, stat a `.meta` per `.png`, JSON-parse each one hunting a guid that can
   never match. A `MarkAllDirty` rebuild was therefore O(assets × content-tree-walk) file
   I/O: invisible on `ReferenceProject`, a multi-second hitch on a real project. **Fixed**
   by making `kind` a required argument of `IsCookPending`, gated before the store ask
   (`Texture`/`Sprite` only; everything else answers `false` for free). The caller already
   had the kind, so this also removed the duplicate registry `Resolve` the first cut did
   for its sprite test. The render oracle passes `AssetKind::Texture` — the only kind
   `NriTextureCache` resolves, since a sprite is re-targeted to its texture before it can
   reach that cache. The cost paragraph above is written against the gated behaviour; the
   ungated version of that sentence was false when written.
2. **A sprite stayed Queued after its own texture cooked.** `OnCookCompleted` marks
   `result.cookedGuids` dirty — those are **textures** — and nothing dirtied the dependent
   sprite (the model's cascade fires for *removed* guids only). On an uncooked tree, or
   after a Recook, the texture flipped to Cooked while its sprite stuck in Queued until
   some unrelated `MarkAllDirty` happened by: the original symptom, displaced one hop down
   the dependency edge. **Fixed** by `m_assetModel.MarkAllDirty()` on a non-empty
   `cookedGuids` batch — the same subsumption trade the material-save seam already makes
   and justifies (a derived answer invalidates more than its own guid, and at this scale a
   whole-model rebuild is cheaper than the dependency walk that would narrow it). Bounded
   by the batch: a pass that cooked nothing marks nothing, so the steady state of trivial
   watcher passes costs no rebuilds. The per-guid diagnostic-row erasure loop is untouched.
3. **A sprite now costs two `refsFor` parses per rebuild** — the model's own ask, plus
   `cookStateFor` → `FirstTextureRefOf` — and `refsFor` is parse-on-call. With the kind
   gate this affects **sprites only**, so it is accepted rather than memoized, and recorded
   honestly at the sprite branch with the same memoize-per-`(guid, mtime)` trigger. The
   model's "exactly ONE `p.refsFor` ask per rebuilt guid" comment was corrected to scope
   that invariant to **the model's own** asks: a host's composed provider may legitimately
   add one, and that is the host's cost to account for, not a break of the rule. The
   call-count test is unchanged and still green — it pins the model, which is what the
   invariant is about.

## 19. LANDED (Plan 3) — 2026-09-08

Plan 3 landed on Arcane `main` in place, **`0dabb8a9..d7a17aab`** (the plan commit
plus Tasks 1–6), plus this task's re-bless (`27135ad0`) and the gate + baselines
catch-up commit. Plan file:
`docs/plans/2026-09-08-asset-manager-plan3-graph.md`. **Not pushed** — held for the
user's desk pass, per house convention. **No ABI bump** (23 stands): Plan 3 is
editor-side, and the riders touched engine code only as a behavior bugfix and a
header-only predicate share.

### Scope landed

§10 the **Graph lens**, complete and ENABLED — the lens mask goes `0b111` and the
one-panel/three-lens design of §2 is closed.

**`AssetGraphViewModel`** (new, editor-pure, headless-testable) projects
`AssetPanelModel::Entries()` + `RefIndex()` into nodes/edges/layers. Deliberately
model-type-free: `GraphBuildInput` takes the entries map and the index **by
pointer**, so its unit tests never construct an `AssetPanelModel`. Scoping is
§10's revised UE-shaped model: default focus = boot scene, BFS depth ≤ 2 from the
focus in **both** directions, per-node breadth cap ~20 with a synthetic "+N more"
overflow node per truncated side, `DerivesFrom` sorted before `References` so the
cut drops the least interesting links, and "everything" = no depth cut with the
breadth caps still applied. Layout is layered left→right by dependency depth
(layer = longest **outbound** path to a leaf, so sources sit left and scenes
right), computed every build, never persisted, no physics.

**The canvas is `ax::NodeEditor`** — spec §10's "reuses the shader editor's canvas
vocabulary" IS that library, and pin-drag creation is its shipped, desk-debugged
gesture. All `ed::` usage is lens-local in `AssetsPanel.cpp`; the graph-framework
extraction stays deferred (standing user directive, 2026-07-24). The shader
editor's idioms carried over: `GraphGridPhase` for the grid, the hand-drawn header
band, `DrawPinDot`-style pins, and the two-layer transparent-`ed::Link` +
hand-drawn-bezier trick that per-kind edge color, mid-edge labels, selected-node
edge brightening and tombstone-edge styling all depend on. The **dashed in-flight
bezier** is the arc's one new draw technique (`CubicBezierAt` + an on/off
arc-length phase accumulator); `ed::Flow`'s marching dots were not used.

**Interactions** (§8 on graph surfaces): a two-way selection bridge on the Graph
lens's **own** selection stamp, the shared peek tooltip with a Graph-only
edge-summary addition, edge brightening on the selected node's own edges,
double-click opens, and a right-click context menu at parity with the row menus.
**Focus UI**: a `focus:` combo in the toolbar and the bottom bar's "N of M assets ·
focus: X". **Focus in Graph** on the Status lens's Scenes rollup — the only
reserved digest→Graph deep-link — is now live rather than disabled.

**Pin-drag `Derive Instance…`** ships as §10 binds and as the Demo board's vignette
draws it: dragging off a **material's** dependents pin opens a ghost menu whose one
entry produces a **pre-parented** `CreateAssetRequest` — the FIRST real producer of
the `createPrefillParent` limb, routed through the one unified-create request §7
requires. "Assign to selection" stays deferred (§15).

A **riders** task (Task 1) paid recorded debt alongside: the EditorLock **zombie**
fix (a desk-found, twice-blocking defect — a lock held by an exited process now
reads stale, from the `exited` FILETIME `GetProcessTimes` already returned, even
while an open handle keeps the pid reserved), the `{hi,lo}` guid-shape predicate
share, the index self-reference characterization test, the boot-guid parse hoist
(`BootSceneGuid(project)` — Plan 3 would have been its third copy), the redundant
premake test-TU line, and the `kStatusPillLineHeight` duplicate.

### Measured close

Three-config **rebuild** of `Arcane.slnx` (`/t:Rebuild`, exit 0 in each), **0
warnings / 0 errors** in Debug, Release and Dist. **What that claim rests on, said
plainly because it is weaker than the phrase sounds:** the builds ran at `/v:m`,
which suppresses MSBuild's `N Warning(s) / N Error(s)` summary block, so "0 warnings"
comes from a case-insensitive scan of all three complete logs finding zero `warning`
lines. That is sound — minimal verbosity still emits every warning and error line —
but it is a log scan, not a summary block, and earlier records in this document that
state the same phrase should not be read as having meant more than this either.

Suite counts DERIVED — each pasted from its own run's final line,
`ArcaneTests.exe "~[gpu]"` run FROM the exe directory:

| Configuration | `~[gpu]` | seed |
|---|---|---|
| Debug | 55294 assertions / 1522 cases | 455517866 |
| Release | 55294 / 1522 | 1081318073 |
| Dist | 55226 / 1516 | 1920384428 |

The constant Dist gap (the pre-existing `#if !defined(ARCANE_DIST)` guards in
`HostConfigTest.cpp` and `NriDiagnosticsTest.cpp`) still measures exactly **68
assertions / 6 cases**: 55294 − 55226 = 68, 1522 − 1516 = 6. One **unfiltered**
Debug run: **117568 assertions / 1555 cases**, seed 3158019304, all passing — so
`[gpu]` contributes 117568 − 55294 = 62274 assertions and 1555 − 1522 = **33**
cases, both stated from measurement. That unfiltered run is also the only suite
that launches a real host, and its two `[witness][gpu]` scenarios were green
first time — the independent confirmation that `ReferenceProject/Binaries/` is not
stale, which is the failure Plan 2's ABI restamp produced at its own close and
which no bump this plan could reproduce.

**One anomalous run at this close, recorded rather than discarded.** The *first*
attempt at the Debug `~[gpu]` figure came back **55293 / 1522 with one case failed**
(seed 3877650429). It is not the measurement in the table above, and the reason is
procedural rather than a product finding: two `ArcaneTests.exe` invocations had been
chained in one shell command, the first piped into a head-style filter that closed
the pipe and killed it mid-suite, leaving shared `%TEMP%` fixture state behind
(`arcane_material_asset_test`, `arcane-diag-test`, …) for the second — which was
itself running while the Release/Dist rebuild saturated the machine. **The failing
case's name was lost with the truncated output, so that mechanism is *attributed*,
not demonstrated.** Against it: five clean serial runs afterwards — three `~[gpu]`
configurations, one unfiltered, one `-r json` — all green, each under a different
Catch2 random-order seed, so the greens are not one lucky ordering. This is recorded
here for the same reason §18 recorded its own close-time surprise: a close that hides
its one red run is not a close anyone can audit. If it recurs outside that abuse it is
a real flake and should be hunted; nothing here claims it cannot.

`scripts/automation-baselines.json` re-derived: **+383 assertions / +12 cases** in
every configuration, booked in **two separate components** so the riders' coverage
is not credited to the lens.

- **(a) The riders** (Task 1, `0dabb8a9..161bbad0`): **+12 / +2** — `ProjectTest`
  21 → 22 (the EditorLock zombie case, 6 assertions) and `AssetReferenceIndexTest`
  9 → 10 (the self-reference case, 6 assertions).
- **(b) The Graph lens** (Tasks 2–6, `161bbad0..d7a17aab`): **+371 / +10** — two
  NEW suites, `AssetGraphViewModelTest` (9 cases / 303 assertions) and
  `AssetsGraphCanvasTest` (**1** case / 68 assertions — one `[graphcanvas]` case
  that grew across Tasks 3/4/5/6 by SECTIONs rather than by cases).

The split rests on two independent grounds, either sufficient alone: **(i)** this
close's own per-case JSON — the 12 new cases sum to exactly 383, and
54911 + 12 + 371 = 55294 reproduces the measured total to the assertion; **(ii)** a
run taken AT the split point — Task 1's own `~[gpu]` Debug run at `161bbad0`
measured 54923 / 1512 (seed 2641303502), and 54911 + 12 = 54923. Nothing was added
to a pre-existing case: `git diff --stat 0a02c101..d7a17aab -- ArcaneTests/src`
touches exactly four files for 1248 insertions and **zero** deletions, and the
per-case sum accounts for the whole rise. Raw `TEST_CASE` count in `ArcaneTests/src`
rose 1543 → 1555 (+12) and `~[gpu]` cases rose by exactly the same +12, so all 12
fall inside the filter and Plan 3 added no GPU coverage; the identity
`raw TEST_CASE − 33 = ~[gpu] cases` holds at both points. Verified against the
file's real consumer: `check-baselines.ps1` reports **+0 on both metrics, exit 0**,
for all three configurations — not Debug alone.

**Golden gate**, Debug, both hosts × both backends, asserted from
`golden-gate-summary.json` only: `gatePassed: true`, four lanes, zero red —
`ArcaneRuntime/dx12` `PassedOnFallback` (its documented steady state), the other
three `Passed`, all four at `diffCount=0`.

**Gate self-test**, `golden-gate.ps1 -SelfTest`, Debug: **PASSED** — `selfTest:
true`, `gatePassed: false`, all four lanes caught the deliberately broken scene by
`exitReason=compare-failed` (runtime lanes 11799 differing pixels, editor lanes
4080), and the tree restored clean afterwards (`git status --porcelain --
ReferenceProject` empty). So the green above comes from a gate observed *failing*
on this tree. An ordinary gate run *after* the self-test then came back
`gatePassed: true` on all four lanes again — which is what proves the restore
actually restored, rather than leaving the green resting on a run taken before the
mutation.

**The editor-ui re-bless** was expected and legitimate: the lane diffed against a
Browse+Status toolbar in which the Graph button was still disabled-dim. Before
blessing, the diff artifact was read — and copied into the SDD workspace first,
since the gate deletes per-lane artifacts on its next run. The differing pixels
(**1104**, and the two backends' diff PNGs are **byte-identical**, md5
`e60393e9…`) form exactly **one solid rectangle**, `x[717..762] y[570..593]` — the
lens strip's Graph button, immediately left of the Status button Plan 2 measured
at `x[763..809] y[570..593]`, in the same y band. 46 × 24 = 1104 = the reported
`diffCount` exactly, which is what proves the marked set **is** that button and
that **zero** marked pixels fall anywhere else — verified programmatically over the
diff PNG, not only by eye. Nothing from Tasks 5/6 leaks in, and that is the
expected result rather than a lucky one: the canvas/node tone overrides, the focus
combo, the legend and the pin-drag affordances all render only *under* the Graph
lens, which the golden scene does not show. `--bless` was pointed at the **source**
tree, dx12, `exitReason=compare-blessed`, then restaged to **both** hosts with all
13 `Verify/` files md5-verified identical across source and both staged copies. The
vulkan lane then passed `diffCount=0` against the dx12-blessed **shared**
reference, which re-proves editor-ui backend-invariant rather than assuming it.

### Deviations and rulings recorded during execution

1. **Pin direction: the plan's parenthetical is corrected.** The plan wrote the
   node's pins the other way round. As shipped, the **LEFT pin is the node's
   OUTBOUND side** (its references / "what I use") and the **RIGHT pin is its
   INBOUND side** (its referencers / "who uses me"). This is geometrically forced,
   not a preference: `layer` is the longest *outbound* path to a leaf, so a target
   always sits in a lower column than its referencer, and the library's curve
   convention has a link leave its start pin along `(+1,0)` and arrive at its end
   pin along `(−1,0)` — so a wire must start at the left node's **right** pin and
   end at the right node's **left** pin. Hence right pins are `ed::PinKind::Output`
   and left pins are `Input`. The board agrees: `uv_marker.png`, a pure target,
   carries a right-hand pin only; `main.arcscene`, a pure referencer, a left-hand
   pin only. **Vocabulary hazard, recorded so it is never re-litigated by words
   alone:** "outbound pins right / inbound left" and "left pin = outbound refs"
   describe the SAME geometry in different frames (wire-flow direction vs
   node-reference direction). State it geometrically. The consequence that matters:
   drag-to-derive off a material creates a new **dependent**, so it comes off the
   material's **RIGHT** pin — confirmed on both boards before Task 6 wired it.
2. **The overflow-node contract, pinned harder than the plan pinned it.** A review
   found that edges dropped by the *veto* path (culled endpoint) had no overflow
   accounting, which left orphan nodes and let "+N more" double-represent nodes that
   were visible anyway. Ruled: **every undrawn candidate edge counts into BOTH
   endpoints' per-direction overflow** (or into the present side only, when the
   other endpoint is culled), for either reason a candidate can fail — the node's own
   breadth cap or the veto. "+N more" therefore means **"N undrawn connections on
   this node's side, in this direction"**, documented in the header
   (`AssetGraphViewModel.hpp`), never "N hidden nodes". §10 forbids silent
   truncation; suppressing the overflow node would reintroduce it, and one-sided
   accounting would leave unexplained orphans. Accepted cost: overflow-node clutter
   in dense graphs.
3. **An overflow node carries `AssetKind::Other`, ALWAYS** — never the anchor's own
   kind. Kind drives the accent color and icon, and a "+5 more" standing for sprites
   must not wear the anchor's texture hue. Part of the same contract, not a minor.
4. **`realNodeCount` (ruling 12's N) excludes overflow nodes AND tombstones.** The
   plan's ruling named only the synthetic overflow nodes; the implementer extended
   it and the extension was adopted — ruling 12's N is "real **asset** nodes", and a
   tombstone is not an asset (nor is it inside M = `health.total`). Note the header
   carries two different "real"s on purpose and says so: `edges` are between
   non-overflow nodes and DO include tombstones (ruling 11 wants a dangling
   reference's edge to have pixels), while `realNodeCount` excludes both.
5. **Overflow nodes are INERT to per-node interactions in v1.** Their guid aliases
   the anchor's, so a select/tooltip/open bridge would silently act on the anchor;
   and expanding them is deliberately-unspecified canvas territory that was not
   invented here. **Tombstones** are naturally excluded from the entry-based context
   menu and double-click (there is no entry), and the peek tooltip is inert on them
   too — the ruling's second option, taken because the helper cannot honestly render
   a peek from index data alone. Both are affordance-shaped things that do nothing;
   the desk checklist says to expect it.
6. **§11.1's "shared `EditorWidgets` layer" yields for the Plan-3 widget row.** The
   graph node / pin / dashed-bezier helpers live **lens-local in `AssetsPanel.cpp`**,
   not in `EditorWidgets`. `CanvasPopupScope.hpp:16-19` is an explicit refusal to
   couple shared widget headers to `imgui_node_editor.h`, and that no-coupling
   precedent is the stronger authority; hoisting these helpers would drag the node
   editor into every widget consumer's include graph for one caller. §11.1's table
   row is left as written and this paragraph is the correction. Cost if wrong: a
   later second graph consumer re-hoists them.
7. **The canvas surface takes the BOARD's tone, not the plan's pin.** The plan
   pinned `Theme::kPanel` from the shader editor's `kCanvasColor` precedent; the
   board draws `#121212`. §11's mocks-are-redline is the higher authority — the plan
   itself appointed the render comparison as arbiter — so the canvas is
   **`Theme::kWell`** (`#121212`, an exact token match, no literal).
8. **…and so do the node tones, lens-locally.** Darkening only the canvas broke the
   board's node-over-canvas *relationships* (+17/+27 where the board steps +7/+12),
   and redline authority covers relationships, not one surface. Measured from
   `OptionD.dc.html`'s own CSS rather than sampled from an AA-contaminated render:
   node body `#1e1e1e` → **`Theme::kPanel`**, title band `#191919` →
   **`Theme::kChrome`**, border `#0d0d0d` → **`Theme::kBorder`**. Four board colours,
   four exact tokens, zero literals; canvas → band → body now steps +7/+12 exactly as
   the board does. **The shader editor's shared canvas constants are untouched** —
   that canvas has its own board and its own review history and no such ruling.
   Accepted and written into the code comment rather than left to be discovered: the
   editor's two canvases no longer read as identically-toned material.
9. **Where the brief's strings and the board's strings disagreed, the BOARD won.**
   The toolbar combo previews `focus: <name>` (the brief had no prefix) and the
   bottom bar reads `N of M assets · focus: X` (the brief's format string was missing
   the word "assets"). Both transcribed from `OptionD.dc.html`, both checked against
   the §5/§18 unified bottom-bar contract for contradiction — there is none. One
   board detail is **not** expressible and was raised rather than faked: the board
   renders the `focus:` prefix in a dimmer tone than the value, which `BeginCombo`'s
   single-string preview cannot do; the combo ships single-tone.
10. **The bottom bar never fabricates N.** Review finding: the Status lens's **Focus
    in Graph** button flips `state.lens` from *inside* the already-dispatched Status
    body, so `DrawGraphLens` does not run that frame at all — yet the bar, which runs
    after the body, already reads the new lens and would have paired the previous
    build's `realNodeCount` (often 0, if the lens was never opened) with the new focus
    name. N is now printed only when `AssetsGraphProjectionIsCurrent(state, model)`
    holds; otherwise the bar prints an **em dash**, per §13's "the digest never renders
    an unknown as a zero". Self-corrects on the following frame. The predicate is
    exported from the header rather than spelled inline at the call site, so the canvas
    test asks the panel's own question instead of a mirror of it. **Coverage limit,
    disclosed rather than glossed:** the gate is a three-conjunct predicate, and only
    two of the three were negative-controlled. The third — the `graphBuilt` conjunct —
    is **defensive-only and unwitnessed**: no test drives a state that falsifies it
    while the other two hold, so nothing proves it is load-bearing rather than dead.
11. **`CreateKindForAssetKind` was the wrong bridge for the pin-drag entry — a brief
    defect, correctly deviated from.** The brief said to map the source asset's kind
    through `CreateKindForAssetKind`; that maps `Material` → `CreateAssetKind::Material`,
    which would have opened an unparented **Create Material** dialog instead of deriving
    an instance. The entry creates the thing that DERIVES from the source, so it names
    `CreateAssetKind::MaterialInstance` outright and sets `createPrefillParent` to the
    source guid. (The bridge itself is correct and stays in use at the rail's `+`, where
    the caller *does* want "the thing the source IS".)
12. **A material's right pin is drawn always, not only when something depends on it.**
    A pure-source material with zero dependents would otherwise have no pin to drag
    from, and the gesture is the lens's headline behavior. Board-supported: the Demo
    vignette shows exactly that lone right-hand pin with its amber glow.
13. **The ghost menu's entry is DISABLED, never hidden**, when the drag did not come
    off a material's dependents pin — a menu that flashes up and vanishes reads as a
    bug; "cannot derive from this" reads as an answer. A tombstone source fails the
    same test by construction (no entry).
14. **Graph nodes are NOT `ARCANE_ASSET` drag sources in v1** (plan ruling 3, shipped
    as ruled). The canvas owns four drag gestures already — pan, box-select, node
    reposition, pin-drag-create — and an ImGui drag-drop source stacked on nodes fights
    all four. §8's drag clause is satisfied by every other representation of an asset.
    Right-click and double-click DO apply to nodes; they do not conflict.
15. **Node repositioning is transient by design** (plan ruling 2). `ed::Config::SettingsFile`
    is `nullptr` — the library would otherwise write a settings json beside the exe, a
    silent new artifact — and positions are re-written from the computed layout on every
    rebuild. Dragging a node therefore **snaps back** on the next model rebuild. Recorded
    as intended behavior so nobody files it.
16. **Tombstone nodes render** (plan ruling 11, shipped): a `RefIndex()` node with
    `exists == false` and a non-empty inbound set draws as a ghost — dim body, amber
    border accent, short-guid name, "missing" pill. This is what finally gives §9.1's
    dangling-reference story pixels, in the Graph lens's own vocabulary rather than as a
    Status card nobody designed.
17. **The legend is a Task-6 rider, and its copy departs from the board twice.** The
    board's bottom-left legend was transcribed board-exact in geometry (18×2 swatches,
    6-on/5-off dash, `#5c5c5c`/`#4a4a4a`/`#ffa61a`, 12px inset) but two strings were
    ruled to follow the lens instead: **"used by" → "uses"** (spec §10 and ruling 9 pin
    `References` → "uses", and that is what this lens's own mid-edge labels say — a
    legend contradicting its own graph is worse than no legend), and **"drag a pin =
    create" → "drag a material pin = derive"** (every non-material pin refuses the
    gesture, and "derive" is the word the entry itself uses). **Standing mismatch, left
    for the desk pass:** the board's first two swatches are two GREYS, while the graph
    draws edges in per-kind color. They were kept as transcribed on the reading that
    they say "a line", not "this color means this" — but the user arbitrates.
18. **Edge display labels** (plan ruling 9, shipped): `DerivesFrom` → "derives";
    `References` → "uses", except a **material-kind source's** `References` edges →
    "samples", which is a display label derived from the source asset's kind, not a
    third `AssetRefKind`. Edge color keys off `e.from` (the graph-semantic source).
19. **Column pitch 300, not the brief's ~260.** Board-measured actual pitches were
    290 and 330; the brief's figure was a tuning start, and the render comparison —
    which the plan appointed as the arbiter — accepted 300.
20. **Ruling 14 resolved by verification, not by guess.** The in-canvas peek tooltip is
    drawn **after `ed::End()` + `SetCurrentEditor(nullptr)`**, keyed off a
    `GetHoveredNode()` captured post-`Begin`, with `forceShow` and a dwell. Verified
    against the vendored library's own source rather than asserted.
21. **The `ed::` context never leaks across a project switch.** It is destroyed beside
    the model's `ResetForProjectSwitch` seam, and both destroy seams were confirmed to be
    the only two. Task 6's gesture stash (`graphWireGuid` / `graphWireDerivable`) is
    cleared there for the same reason: it names an asset of the outgoing project.
    **Mechanism deviation:** the plan said the `ed::DestroyEditor` calls "live app-side";
    what shipped is a panel-owned `DestroyAssetsPanelCanvas(state)`, called app-side at
    both seams. Ruling-1-conformant — all `ed::` usage stays inside `AssetsPanel.cpp`.
22. **The Graph lens gets its OWN selection stamp** (`seenSelectionStampGraph`, plan
    ruling 4). The existing shared field has Browse as its only consumer; a second
    consumer on it would swallow the other's pending scroll/center. Browse's field and
    idiom are untouched.
23. **Kind colors are a panel-local table** (plan ruling 5), in `AssetsPanel.cpp`'s
    anonymous namespace: `EditorTheme.hpp` rules domain color-coding out of the theme,
    and the `kPillAmberBorder` precedent covers spec-pinned hexes with no token.
24. **`AssetsPanel.cpp` is now compiled into `ArcaneTests`.** The link closed with that
    one TU alone — the preflight's worry about dragging half the editor in did not
    materialize, since `DocumentHost`/`EditorWidgets`/`EditorFonts`/`AssetPanelModel`/
    `AssetReferenceIndex`/`AssetActivityLog` were already there and `CreateAssetDialog`'s
    half of the surface is header-only. Precedent for compiling a DRAW TU into the tests
    is `ShaderEditorDocument.cpp`, and the reason is the same: the node-editor canvas only
    runs inside a live ImGui frame, so no pure unit can stand in for it. This retires
    §18's "`AssetsPanel.cpp` is not compiled into `ArcaneTests`" — see the dated
    corrections below.

### Verification technique, and what it could not reach

The lens is verified on three legs. **(1)** The pure view model, nine `[editor]` cases.
**(2)** A device-less `[graphcanvas]` case that drives the REAL `DrawAssetsPanel` through
four ImGui frames with the Graph lens forced on — the harness class that caught the shader
editor's frame-2 `EndCreate` abort. **(3)** Headless `--screenshot` render comparisons
against `OptionD-Graph-FINAL.png`, structural per §11.2, never a pixel diff.

Two **negative controls** worth recording because they proved live folklore rather than
repeating it: making `EndCreate` conditional **aborts** the suite on frame 2
(`imgui_node_editor.cpp:4756`, exit 3) — the unconditional-`EndCreate` rule is now
observed, not inherited; and disabling the rebuild guard reproduces the exact
renumbering failure it exists to prevent. A separate lesson was captured when the FIRST
determinism witness passed its own control: `SkipItems` guts submission without firing
the region guard, so that harness now carries two witnesses.

**What no machine run reached.** The pin-drag gesture was never executed end-to-end:
there is no headless drag injection, so the consumer limb is traced structurally and the
identical `BeginCreateAsset` arm is exercised by the shipped New Instance action, but the
one hop from "menu entry clicked" to "dialog opens pre-parented and the new instance
lands selected" is **desk-checklist territory** and is listed there. Likewise the
**capture framing**: the Graph captures in the SDD workspace were taken with a temporary
lens default and a temporary *staged* `verify-layout.ini` edit, both reverted and
re-proven clean — no committed Graph-capture layout seed was added, because that is a
tracked-file decision the user owns.

And **one conjunct of the honest-N gate is unwitnessed**: `AssetsGraphProjectionIsCurrent`
tests three conditions, two of which were negative-controlled by disabling them and
observing the failure. The `graphBuilt` conjunct was not — no test reaches a state that
falsifies it while the other two hold — so it ships as defence in depth with nothing
proving it is load-bearing rather than dead code. Disclosed at the time and repeated here
rather than left in a review thread (deviation 10).

### A repo-wide discovery, recorded where it will be found

**Catch2 assertion `file:line` is unreliable in `ArcaneTests` TUs that include
`<windows.h>`.** Task 1's first RED transcript reported assertion failures at lines 1141
and 1142 of a 568-line file, which read as a fabricated or hand-edited transcript. It was
neither: MSVC's **traditional preprocessor** mangles `__LINE__` in those TUs, and the
effect is pre-existing and file-wide — `ProjectTest.cpp` and `DiagnosticsTest.cpp` are
affected including cases nobody touched, while `AssetReferenceIndexTest.cpp` is clean. A
regenerated RED and its GREEN print the **same** impossible numbers, failing then passing,
which is what rules out different-file-state. `/Zc:preprocessor` is the usual remedy. This
degrades every future failure transcript out of those files; it is a rider or engine-backlog
candidate, **not** this plan's scope. Recorded here because a spec addendum is where the
next person looks.

### Dated corrections to §15 — 2026-09-08

Recorded here rather than by editing §15, so its record still shows what was true when
written:

- §15's out-of-scope list still stands entirely. **Three** of its items are the ones
  Plan 3 could plausibly have moved, and all three stay out — but they stay out for a
  new reason, which is the correction worth recording. They are no longer merely
  *unscheduled*: the lens that would have carried them shipped, and each was
  re-confirmed against it. **"Assign to selection" from graph pin-drag** remains out
  because the entity-slot targeting rules still do not exist, and §10's "pin-drag v1
  offers `Derive Instance…` only" is exactly what shipped. **Panel-state persistence
  across restarts** and **new drag-drop targets** are re-confirmed by rulings 15 and 14
  above — the canvas deliberately persists nothing (`SettingsFile = nullptr`), and graph
  nodes are deliberately not drag sources. None of the three is "settled"; they are
  confirmed-out with a live implementation now standing behind the confirmation.
- **§9.1's dangling-reference story now has UI.** §18's deviation 8 recorded that
  dangling references stayed data-only because §9.2's card inventory did not name a
  dangling card and none was invented. Plan 3's ruling 11 gives them their natural home:
  **tombstone ghost nodes in the Graph lens**. `DanglingTargets()` is no longer
  data-with-no-pixels.
- **§10's scoping model shipped as specified**, including the parts that were revised
  after the UE Reference Viewer review: depth limit 2 both directions, per-node breadth
  cap with "+N more" instead of silent truncation, `DerivesFrom`-first sorting before the
  cut, and "everything" under the same breadth caps. The one thing §10 did not say, and
  which this arc had to rule, is what "+N more" *counts* — see deviation 2.

### Dated corrections to §17 and §18 — 2026-09-08

- §18's **"Known desk-only branches"** says "`AssetsPanel.cpp` is not compiled into
  `ArcaneTests`, so the lens's logic is covered through the Task 3/4/5 pure units and
  the render/desk comparisons instead". **The first clause is no longer true**: it is
  compiled in as of `33640bb9` (deviation 24). The *rest* of that paragraph still holds
  — the refused attention card and the cards' empty states remain inspection-only,
  because `ReferenceProject` has zero refused artifacts.
- §18's **measurement asymmetry** paragraph (the `diag://` mount is absent under a
  headless compare/report run, so the golden reads `8 assets` while an interactive
  session shows more) is unchanged and applies to this plan's captures for the same
  reason. It is also why the Graph lens's fixture graph is small: see the desk checklist's
  note on depth/breadth caps.
- §17's **"Owed, and deliberately held"** ABI line is unchanged by this plan — Plan 3
  took **no** bump, so Aphelyon's obligation stays at the third one Plan 2 recorded, and
  Gacha `main` still stays at `5923da65`. Still recorded, still not blocking, still not to
  be nagged about.
- §17's fourth-revision **compact preview header** is still genuinely owed — Plan 3 did
  not touch the preview pane.

### Deferred, and going to the desk pass

Deliberately not decided at the desk during implementation; the checklist carries each:

- **Grid tone and pill colours** are the last Graph surfaces off the board (board: a
  single `#242424` dot grid where this lens keeps a minor/major grid; `.pill` is
  `#9a9a9a` on `#333333`). Both were held rather than retinted: pills are a Plan-1
  shared widget already desk-passed across all lenses, and the grid is shared canvas
  language with no §11.2 pin that the shader editor draws from the same phase.
- **The tombstone ghost's two amber strengths.** The ghost wash dims the node's accent
  bar but not its border, so one node carries amber at two intensities. Raised at Task 3
  and explicitly held for the Task 5/7 render comparison rather than tuned in place —
  which of the two is meant to read as "missing" is a design call, not a defect report.
- **The node body's detail line.** The board shows a real detail line for non-material
  kinds where this lens draws a kind pill. The content is unspecified, and inventing it
  would be desk-designing.
- **The legend's two grey swatches** (deviation 17) and **"New Instance…" vs "Derive
  Instance…"** — one action wearing two names, both brief-mandated.
- **The edge summary is two lines**, where the brief said "line".
- **Dwell survives a lens switch**, so returning to the Graph lens can show an instant
  peek rather than a fresh dwell.

### Minors deferred with citations

Each was raised in review, ruled non-blocking, and carried here **complete** — this
list is the whole carry-forward, because the arc's working ledger lives in a
gitignored SDD workspace and does not survive the push. None blocks.

**Correctness / robustness**

- Negative `breadthCap` → `resize(~2^64)`, UB-adjacent; a `std::max(0, ·)` clamp at
  `Build` entry closes it.
- `ComputeLayer` recursion is unbounded (deepest chain actually tested: 7).
- Duplicate refs to one target with **differing kinds** give a nondeterministic edge
  kind/label and consume the cap budget twice; dedup candidates in `ProcessNode`.
- `ScenesByName` sorts by **stem** while displaying **fileName** — the two orders
  coincide only while every scene shares one extension. A latent ordering defect, not
  a cosmetic one; either switch the key or comment the assumption.
- Mid-codepoint **UTF-8 truncation** is possible at the 128-byte `snprintf` boundary
  in the bottom bar (theoretical at present name lengths).
- `DrawGraphEdgeSummary` **silently emits nothing** when its `Find` is null. Harmless
  for today's only caller, which always supplies one; latent for any future non-graph
  caller, which would inherit a silent drop rather than a diagnosable failure.
- `graphCanvas` is an owning raw `void*` on a **copyable** state struct with no RAII
  or copy guard — a latent leak/alias class. Both live seams are covered today.
- `Project.cpp`'s `lpExitTime` is documented-undefined for a still-running process;
  a one-line `GetExitCodeProcess(STILL_ACTIVE)` hardening would close it (the kernel
  zeroes it in practice, and the live-lock direction is pinned by tests).

**Comments that overstate what the code does** (each is a wording fix, but each one
misleads the next reader in a specific way)

- The dirty trigger **over-fires**: it fires on invalidations with identical content,
  while the header comment says "changed content".
- The `BeginCreate`-deferral comment does **not** cross-reference the pin-direction
  block that Task 6 had to read first (that dependency travelled by dispatch note
  instead of by comment — exactly the fragility deviation 1 exists to prevent).
- The `ResetForProjectSwitch` comment says a field is "NOT reset" on a line that
  precedes `++entriesStamp`.
- The dash-cell comment over-claims "the board's reading at **every** zoom stop": the
  dash cells are screen-constant while the stroke and end-dot zoom-scale.
- The header carries two different senses of "real" (see deviation 4) and says so, but
  the wording still wants a pass.

**Test gaps**

- No cases for empty/null input, an absent focus, or a degree exactly **at** the cap
  boundary.
- The `"(missing)"` focus label is untested — no case supplies a focus guid whose
  scene was deleted.

**Cosmetic / hygiene**

- The focus combo appears **one frame after** a toolbar lens click (a pre-mutation
  read; the ImGui-conventional lag, and the same one-frame class deviation 10's gate
  handles for N).
- `graphMenuGuid` / `graphWireGuid` survive popup close (stale guids in session state,
  harmless); fix both or neither.
- `model.Find()` runs per node per frame in the new pin loop.
- The in-flight curve is capped at ≤64 chords, so it reads coarse at long-drag /
  low-zoom, where the solid wires are adaptive.
- The legend has no fit guard and can overlap the toolbar on a short panel; only the
  clip rect saves its width.
- `DrawCreateMenuEntries`' `enabled` parameter now has no `false` caller — restate its
  justification comment or delete the parameter.
- `HoverStationaryDelay` is the semantically wrong knob (the stationary half is not
  implemented); `HoverDelayShort` is closer, and the two are value-identical today.
- `IdentityFieldRule.hpp` now pulls `<Json.hpp>` for name-rule-only consumers (all
  three current includers already carry nlohmann; act only if an nlohmann-free
  consumer appears).
- `ProjectTest.cpp` launches `cmd.exe` through a PATH search rather than an absolute
  `%COMSPEC%`.
- `AssetGraphViewModel.cpp`'s sort comparators **allocate**: `SortKey` (`:35`) returns
  a `std::string` by value, called twice per comparison; `SortByImportance` (`:85`),
  the edge comparator, calls `Guid::ToString()` up to four times per comparison (twice
  inside `SortKey`'s tombstone fallback, twice more in the guid tie-break) —
  decorate-sort-undecorate removes it if everything-mode ever profiles hot.

One item that used to sit in this list has been **promoted out of it**: the tombstone
ghost's wash dims the accent bar but not the border, leaving two amber strengths in one
node. That is a visual arbitration, not a minor — it is in the desk-pass section above.

### Post-landing user-directed change — 2026-09-09: gradient edge wires

**Directive** (verbatim): "like our node graph for the shader can we have our custom
gradient lines".

Graph-lens **data edges no longer draw as one flat tone**. Each visible curve is a
**gradient between the two accents its own endpoints wear**: the wire still starts at
the target's right pin and ends at the referencer's left pin (§10's established
geometry, ruling 7's two-layer trick, both unchanged), and each end now takes exactly
the colour that pin already carries — so a wire reads pin-hue → pin-hue the way the
shader graph's do. A **tombstone endpoint ends amber**, because amber is what ruling 11
already gave its pin dot; pin colour and wire-end colour now come from one helper
(`GraphNodeAccentColor`) precisely so they cannot drift apart.

**This supersedes ruling 18's closing sentence** — "Edge color keys off `e.from` (the
graph-semantic source)". That sentence described a single-tone stroke keyed off one
endpoint; there is no longer a single tone to key. The rest of ruling 18 (the display
labels, and "samples" as a material-source reading of `References`) is untouched.

**Technique — copied, not invented.** The shader editor's `DrawGradientWire`
(`ShaderEditorDocument.cpp:5459-5497`) is the source, including its **two-path split**:
equal colours take ImGui's own adaptive `AddBezierCubic`, and only a genuine two-hue
wire pays for the per-segment walk. Its screen-length segment budget
(`clamp(polyLen × viewScale / 6, 12, 64)`, the control polygon as a cheap arc-length
bound), its **midpoint colour sampling** (so both ends of the run land on the pure
endpoint colours), and its butt-cap reasoning (consecutive samples on a curve this
smooth are near-collinear; a shared `PathStroke` takes one colour and so cannot be
used) are all reproduced rather than re-derived. Adapted only in plumbing: `viewScale`
is a parameter (this file's existing `DrawGraphDashedWire` convention — the lens
computes it once per frame) and the caller hands down two FINAL colours, so emphasis
stays where it already lived and the draw helper stays pure paint.

**Emphasis semantics are unchanged** — same trigger (either endpoint is the selected or
the hovered asset), same two functions, now simply applied to both ends instead of one:
`GraphDimColor` at rest, `GraphBrightenColor` on both ends together when emphasized.

**Also unchanged**: the transparent-`ed::Link` hit-testing layer, the control-point
convention, mid-edge labels and their LOD (the label's canvas-toned backing plate still
masks the wire's midpoint and is unaffected by hue), and the dashed amber in-flight
gesture wire — a gesture indicator, not a data edge, so it stays solid amber dashed.
The **anchor → overflow-node connectors stay a subtle single grey**; because both their
colours are equal they take the flat fast path, i.e. genuinely unchanged paint rather
than a gradient that happens to be constant.

**Board divergence — recorded and deliberate.** `OptionD-Graph-FINAL.png` draws
solid-tone edges; the lens now does not. User-directed beats board, so this is a
divergence to **cite, not a defect to fix**. It does not resolve ruling 17's standing
legend mismatch (the legend's first two swatches are still two greys, kept on the
reading that they say "a line", not "this colour means this") — that arbitration
remains open.

**Measured** (headless 1280×720 capture at the shipped code, ReferenceProject, Graph
lens): every traced wire is a monotone per-channel ramp between two dimmed kind tones.
The `uv_marker.png` → `uv_marker.arcsprite` "derives" edge scans, pin dot to pin dot, as
rgb(176,106,91) at the texture pin (the full `#b06a5b` accent) → rgb(77,52,46) where the
wire leaves it (dimmed Texture predicts (78,51,46)) → rgb(72,47,71) at t≈0.78 (the
TEX→SPR line predicts (72,47,70)) → rgb(70,46,77) at the far end (dimmed Sprite predicts
(70,46,78)) → rgb(155,91,176) at the sprite pin (the full `#9b5bb0` accent). Test counts
are **unchanged at baseline** (Debug `~[gpu]` 55294 assertions / 1522 cases): a pure
draw-body change adds no assertions.

### Post-landing user-directed change — 2026-09-09: zoom-table parity fix

**Bug report** (user, verbatim): "For some reason, the text becomes blurry when zooming
in on the graph. I don't think this happens with our other graph system."

**Root cause.** `ImGuiEx::Canvas` (`ThirdParty/imgui-node-editor/imgui_canvas.cpp:
513-536`) implements zoom as a pure post-hoc multiply of already-baked vertex
positions — glyphs are rasterized once at their logical pixel size and the canvas
then bilinearly magnifies the finished quad. The library has no font handling of
any kind (no `PushFont`, no `SetFontRasterizerDensity`, no per-zoom re-bake) and
compensates only geometry anti-aliasing (`imgui_canvas.cpp:491-493`), never glyph
resolution. The Graph lens's `ed::Config` (`AssetsPanel.cpp:3877-3897`, pre-fix)
never installed a custom zoom table, so it fell through to the vendored library's
own default (`imgui_node_editor.cpp:3309-3312`), whose top entry is **8.0**. Wheel
zoom could therefore drive the canvas to 8x, bilinearly magnifying the lens's
12-14px baked text (`kGraphHeaderFontPx`/`kGraphMetaFontPx`/`kGraphLabelFontPx`,
`AssetsPanel.cpp:2881-2882`, `:2997`) into unmistakable mush. The shader editor's
two canvases (`ShaderEditorDocument.cpp`) never showed the symptom because both
already called `ApplyZoomLevels(cfg)`, installing a 20-stop table ported from
Unreal's graph editor whose top entry is **2.0** — so its worst case was a 2x
magnification of 16px ambient text against the Graph lens's 8x magnification of
12-14px text. Full investigation: `.superpowers/sdd/2026-09-08-asset-manager-
plan3-graph/blur-investigation.md`.

**Fix.** Apply the same zoom table to the Graph lens's `ed::Config`, at parity
with the shader editor rather than inventing a third table. The table +
`ApplyZoomLevels` helper — previously defined only inside
`ShaderEditorDocument.cpp`'s anonymous namespace — were hoisted to a new shared
header, `ArcaneEditor/src/Widgets/GraphZoomLevels.hpp`, sibling to
`CanvasPopupScope.hpp` rather than folded into it (zoom is a second, unrelated
concern from that header's one named rule, popup placement) and rather than into
`EditorWidgets.hpp/.cpp` (that vocabulary stays `imgui_node_editor.h`-free by
design, same reasoning `CanvasPopupScope.hpp` already documents). Both the shader
editor's two `ed::Config` sites and the new one in `AssetsPanel.cpp:3877-3897`
now consume this one definition — no hand-synced second copy, the exact smell
this arc already paid down once for the guid predicate (§19 riders, Task 1).
`EditorWidgets.hpp/.cpp` and `AssetsPanel.hpp` gained no node-editor coupling;
the include lands only in `AssetsPanel.cpp`, the one TU plan ruling 1 already
permits to name `ax::NodeEditor` for this panel.

**Verified.** `AssetsGraphCanvasTest.cpp`'s `[graphcanvas]` case gained a
regression check that queries the REAL `ed::Config` the panel's context ended up
with, through the public `ed::GetConfig(ctx)` seam (`imgui_node_editor_api.cpp:
68-84`) — an honest query, not a fake: the panel already exposes its canvas
context as a `void*` for exactly this reason (AssetsPanel.hpp's own note), and
the test reinterprets it the same way `AssetsPanel.cpp` does internally.
RED (table missing, `ApplyZoomLevels` call reverted for the capture): `REQUIRE(
cfg.CustomZoomLevels.Size > 0 )` failed, `0 > 0` (seed 1788947125). GREEN (fix
restored): `20 > 0` and `CHECK( maxZoom == 2.0f )` → `2.0f == 2.0f` (seed
1788947178); `[graphcanvas]` filter all green, 77 assertions / 2 cases (+3 over
the prior 74, exactly the three new checks). `[editor]` filter green, 3595
assertions / 303 cases (seed 1788947186) — the shader editor's own suites are
inside this filter and unaffected by the hoist. Full Debug `~[gpu]`: 55297
assertions / 1522 cases (seed 1788947199) — +3 over the 55294 baseline, exactly
the new assertions, **+0 cases** (the new checks are inline in the existing
`AssetsGraphCanvasTest` case, not a new `TEST_CASE`). Three-config rebuild not
re-run for this one-file fix; Debug alone rebuilt clean, 0 warnings / 0 errors at
`/v:m`, same log-scan basis §19's Measured close already disclosed.

**What this changes for the user:** the Graph lens's wheel zoom now stops at 2x
instead of 8x, matching the shader editor's ceiling exactly, and the zoom stops
in between are the same finer-grained 20-entry table. **Honest limitation,
unchanged from the investigation's own accounting:** this caps how bad the blur
gets: it does not make text crisp. Both canvases still bilinearly magnify baked
glyphs up to 2x, which remains visibly soft — parity with the system the user
called good, not a claim of sharpness. **Recorded follow-up for BOTH canvases**
(not attempted here): ImGui 1.92's rasterizer-density mechanism
(`ImGui::SetFontRasterizerDensity`, `imgui.h:3726`) can re-bake glyphs at the
view scale so the canvas's vertex multiply lands on an already-hi-res bitmap —
genuinely sharp text at every zoom stop, not merely a lower ceiling. The
investigation report's FIX B section traces feasibility (the flag Arcane's NRI
backend needs is already set) and its costs (per-zoom-stop atlas bake churn;
`SetCurrentWindow` silently resets the density on any nested `ImGui::Begin`).

### Post-landing user-directed change — 2026-09-09: graph-canvas consolidation

**Directive** (user, verbatim): *"I want to ENSURE we're not duplicating any unneeded
logic from our existing graph system. One goal we have is to have everything be
scalable and reusable if possible, like the existing graph system from our shader
graph."*

**The audit that answered it.** A read-only comparison of the Graph lens
(`AssetsPanel.cpp`'s graph region, `AssetsPanel.hpp`, `AssetGraphViewModel.*`) against
the exemplar (`ShaderEditorDocument.cpp/.hpp`) and the three headers they already
share, classified every overlap:
`.superpowers/sdd/2026-09-08-asset-manager-plan3-graph/duplication-audit.md`.

| Class | Count | Meaning |
|---|---:|---|
| **A** already shared | 4 | consumed from a shared header by both; neither re-implements it |
| **B** extract | **16** | verbatim or trivially parameterizable duplicate |
| **C** keep separate | 10 | similar-shaped, divergence load-bearing |
| **D** lens-specific | 11 | no counterpart on the shader side (≈ 900 lines, `AssetGraphViewModel` alone 661) |
| **E** framework-deferred | 8 layers | **0 duplicated** |

Class B stood at roughly **167 asset-side lines + 137 shader-side lines ≈ 304 lines in
two places** — about **7 %** of the graph code this arc added. The other 93 % is class
D: original, lens-specific work with nothing to consolidate against.

**Executed in the audit's eight-wave risk order**, one commit per wave, each built and
run against the focused suites before the next began:

| Wave | Items | What moved |
|---|---|---|
| 1 | B1-B4, B10-B13 | pure values and pure functions: bezier eval, sRGB lerp, brighten, the view-scale reciprocal, seven chrome metrics, the grid palette, the selection/hover accents, the links-channel constant, the LOD table |
| 2 | B5, B16 | `Link::GetCurve`'s control points; the wire segment budget (which stood twice inside `AssetsPanel.cpp` alone) |
| 3 | B9 | the pin-dot paint core (placement deliberately not moved) |
| 4 | B14 | ellipsize, against the shared widget layer |
| 5 | B7 | the backdrop composition (view + palette + lattice draw) |
| 6 | B8 | the canvas style application, behind a desc |
| 7 | B6 | the gradient wire stroke — the largest single win, sequenced last so its dependencies had already landed |
| 8 | B15 | the create/delete bracket rule, as RAII types |

**Shared homes created**, all node-editor-coupled and all sibling to the existing
`CanvasPopupScope.hpp` / `GraphZoomLevels.hpp` / `GraphGridPhase.hpp`, one named
concern per file:

- `ArcaneEditor/src/Widgets/GraphCanvasStyle.hpp` — the node chrome metrics, the wire
  thickness, the pin segment count and ring width, the grid palette, the
  selection/hover accents, and `GraphCanvasStyleDesc` + `ApplyGraphCanvasStyle`.
- `ArcaneEditor/src/Widgets/GraphWire.hpp` — `kGraphLinkChannel` (with the whole
  two-layer transparent-`ed::Link` rationale, previously written out twice),
  `GraphCubicBezierAt`, `GraphLerpColor`, `GraphBrightenColor`, `GraphViewScale`,
  `GraphWireControlPoints`, the segment budget, and `DrawGraphWire`.
- `ArcaneEditor/src/Widgets/GraphPinDot.hpp` — `DrawGraphPinDot`, the paint only.
- `ArcaneEditor/src/Widgets/GraphCanvasBackdrop.hpp` — `DrawGraphCanvasBackdrop`, the
  composition that needs `ed::` and therefore cannot live in `GraphGridPhase.hpp`
  (whose whole point is having no device and no node-editor dependency).
- `ArcaneEditor/src/Widgets/GraphNodeLod.hpp` — `NodeLOD` (moved out of
  `ShaderEditorDocument.hpp`), the `kLod*` boundaries and `NodeLODForScale`: the zoom
  table's third column, beside the table.
- `ArcaneEditor/src/Widgets/CanvasEditScope.hpp` — `CanvasCreateScope` /
  `CanvasDeleteScope`.

One item landed in an **existing** home: B14 added a defaulted `ellipsis` parameter to
`EditorWidgets::EllipsisToWidth`. That is the audit's one sanctioned exception to the
boundary rule and not a breach of it — both sides of that duplication are
`ImGui::CalcTextSize` only. **`EditorWidgets.hpp/.cpp` still names nothing from
`imgui_node_editor.h`, and `AssetsPanel.hpp` still holds its canvas as an opaque
`void*`** — both re-verified by grep after the final wave.

**The recorded colour divergence survives by construction.** The 2026-09-08 controller
ruling — the OptionD board is the redline for the Graph lens, and the shader canvas is
deliberately *not* dragged onto it — is now expressed as named `GraphCanvasStyleDesc`
fields with the ruling cited beside them, rather than as two structurally identical
blocks that happened to hold different numbers. That is strictly stronger than before:
the values stay apart on purpose, while a *structural* change (a new style field, a
reordering) can no longer land on one canvas and silently miss the other.

**Line accounting, honestly.** The audit projected ≈ 180 lines of shared surface and a
net deletion of ≈ 124 code lines. Measured over `dd20ebc4..HEAD` (non-blank,
non-comment lines): the two consumer files shed **−267** code lines, the shared headers
gained **+273**, for a net of **+6**. The projection did not account for structure that
existed in *neither* copy and had to be written: `GraphCanvasStyleDesc` and its two
per-canvas factory functions (B8), and the two RAII types (B15 — which the audit itself
scored as "0/0 code lines" because what was duplicated there was an invariant written
out as prose in four places, not code). Total file lines grew by 358, because the
consolidated rationale is written once and written properly, in this codebase's house
style. **The duplication is what was removed, and it is gone: all 16 class-B items now
have exactly one definition site, verified by grep** (see the report's sweep). The
line count was never the goal.

**Class E — the framework question, verified rather than assumed.** The 2026-07-24
standing directive defers extraction of the graph *framework* (schema, node/pin/link
model and id scheme, serialization shape, gesture undo, badges, create menu/searcher)
until a second real consumer drives its design. The audit checked all eight of those
layers against the Graph lens and found **zero duplicated**: no schema (every
`QueryNewLink` is rejected outright — the graph is a read-only projection of the
reference index), no undo (zero `Undo`/`CommandStack`/`EditGesture` hits in the graph
region), no serialization (`cfg.SettingsFile = nullptr`), no editable node model, no
clipboard, no palette, no badges, no backend. **The framework extraction therefore
stays deferred, unchanged** — the consolidation trigger was pulled on duplication, and
there is no framework duplication for it to reach. What class B moved is the *paint
layer*, which is exactly the material the directive's own closing instruction ("keep
`ShaderEditorDocument`'s canvas code cleanly separated") asks to separate now; giving
it one home makes the eventual framework extraction cheaper rather than pre-empting it.

**Verified.** Focused suites after every wave, from the exe directory with seed banners
captured, counts unmoved from the pre-change baseline throughout: `[graphcanvas]` 77/2,
`[editor]` 3595/303, `[material]` 1724/87, `[shadercompile]` 339/23. Full Debug
`~[gpu]` after the final wave: **55297 assertions / 1522 cases (seed 1788952335)** —
identical to the `dd20ebc4` baseline of 55297/1522, zero delta in either number.
Debug rebuilt clean at every wave, 0 warnings / 0 errors at `/v:m`.

**Capture comparison (the no-pixel-test gap's manual mitigation).** A before/after
headless capture pair of the **Assets Graph lens** was taken through the arc's
established temporary-lens-default + staged-`verify-layout.ini` flow (both reverted,
the staged ini re-proven byte-identical to its source, tree clean bar the user's own
untracked files). **The two frames are pixel-identical over the whole canvas and all
editor chrome — 0 differing pixels for y 0..679.** The only differing pixels in the
1280×720 frame are the Console panel's wall-clock timestamp in the bottom bar
(`05:42:00` vs `06:16:28`), which differs between any two captures. The same holds
against the workspace's pre-existing `graph-lens-live.png` reference: 0 differing
pixels outside that band. Artifacts: `graph-lens-BEFORE-consolidation.png`,
`graph-lens-AFTER-consolidation.png`.

**Stated plainly, because it is the honest limit: the shader editor has no headless
capture flow.** Nothing photographed it before and nothing photographs it now. Its
protection is (1) byte-identical values — every constant it now reads from a shared
header was the same literal it previously spelled locally, and every parameterized call
was derived from the code it replaced, argument by argument; (2) its suites, including
`GraphCanvasHeadlessTest`'s four device-less frames through the real `Draw()`; and (3)
code review. A desk item was added to `DESK-CHECKLIST.md` B1 asking for the eyes it
cannot get automatically: open a material, pan, zoom, drag a wire, hover and select a
link, drag a node, open a context menu, look at a comment box.

**Follow-ups filed, not taken:**

1. **The exemplar hand-rolls its own shared type.** `ShaderEditorDocument.cpp` still
   holds a raw `ed::Suspend()` … `ed::Resume()` pair spanning ~317 lines of
   `DrawGraphPanel`, in the same file that uses `CanvasPopupScope` three times — the
   longest such bracket in the codebase is the one outside the type. It is *not* a
   duplication against the Graph lens (which uses the type correctly), it crosses
   early-return paths, and converting it carries behaviour risk this consolidation
   deliberately did not take. **Recorded, not fixed.**
2. **No pixel test covers either canvas.** The golden `editor-ui` lane renders the
   Assets panel's **Browse** lens; it does not open a Material document and does not
   switch to the Graph lens, so neither canvas is in any golden image. The cheapest
   real gate, if one is wanted, is the audit's own suggestion: a device-less
   characterization test that calls the extracted wire function directly and asserts
   the returned midpoint and the emitted `ImDrawList` vertex count.
