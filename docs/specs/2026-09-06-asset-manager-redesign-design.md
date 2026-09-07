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
groups default expanded (derived children default collapsed, §6). Panel state
(active lens, rail filter, search, group collapse, **and the preview pane's
width**) is session-only in v1.

## 6. Browse lens

- **Rail:** `All` + one row per `AssetKind` present (zero-count kinds hidden),
  counts from the model. The hover `+` appears **only on kinds with a Create
  entry** (Materials, Sprites, Meshes, Scenes) and opens the unified Create menu
  pre-scoped to that kind. Textures/Data/Audio/Font get no `+`.
- **Table:** groups = one per distinct content directory (root files under
  `Content/`), sorted lexicographically, collapsible (session state). **Nested
  directories render as indented child groups inside this same table** (2026-09-07,
  user-directed "middle form" — not a separate tree panel, no breadcrumbs): 20px
  indent per nesting depth, and a nested group's label shows only its leaf segment
  (a group under `fx/` reads `glow/`, not `fx/glow/`). Collapse cascades — collapsing
  a parent hides its whole subtree — but each descendant group keeps its own open
  flag, so reopening the parent restores whatever sub-state it had. Asset rows
  indent to their group's depth plus their existing base offset, so they read as
  belonging to that group. **Search reveals matches uniformly** (2026-09-07 review
  fix round 1, Important 4 — this is the corrected wording; an earlier draft of this
  paragraph attributed the rule to an existing fold-child precedent that, on
  inspection, had never actually shipped that override): while a search is active,
  collapse is bypassed at **every** level — a group's own closed flag, any ancestor's,
  and a texture's own folded-children flag all stop hiding a row that matches, and
  every group on the path down to it still renders (even one with zero of its own
  matching entries) so the tree's context stays visible. A directory that holds no
  files of its own but has a populated descendant (whether from nesting alone or
  because a kind filter left its own entries at zero) still gets a group row —
  its count is suppressed rather than shown as a bare `0` — so a subtree's chevron
  is always reachable to reopen it, never orphaned behind an ancestor that itself
  never renders. Rows are Name-only: 18px thumb, name, pills (subkind,
  `boot`, `sliced`, `derived`). Derived 1:1 sprites render only as indented children
  under their texture, **default collapsed** with a count pill, keeping their own
  **extra +20px fold indent** on top of the group's own depth indent — two
  indentation meanings, kept visually distinct as today (chrome group bands vs dim
  child rows). A **refused** asset gets a small amber triangle on its row; queued
  gets no row marker.
- **Preview pane:** 140px thumb, name + kind/subkind pills, path, guid
  (click-to-copy), cook line, `Derived (N)` list (click selects the child), actions
  mirroring the context menu.
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
| group nesting indent per depth / fold-child indent | 20px per level, leaf-segment group labels (§6, 2026-09-07) / +20px beyond the row's own group-depth indent — the same 20px unit, applied once more |
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

Not pushed, per house convention.
