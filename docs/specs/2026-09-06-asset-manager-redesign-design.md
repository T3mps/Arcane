# Asset Manager Redesign — Design

**Date:** 2026-09-06
**Status:** Approved design, pre-plan
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
path+guid hover tooltip.

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
Narrow docks: rail fixed 180px, preview fixed 330px; below a 720px panel width the
preview hides first — the table never drops below readable width. Folder groups
default expanded (derived children default collapsed, §6). Panel state (active
lens, rail filter, search, group collapse) is session-only in v1.

## 6. Browse lens

- **Rail:** `All` + one row per `AssetKind` present (zero-count kinds hidden),
  counts from the model. The hover `+` appears **only on kinds with a Create
  entry** (Materials, Sprites, Meshes, Scenes) and opens the unified Create menu
  pre-scoped to that kind. Textures/Data/Audio/Font get no `+`.
- **Table:** groups = one per distinct content directory (nested dirs are their own
  groups; root files under `Content/`), sorted lexicographically, collapsible
  (session state). Rows are Name-only: 18px thumb, name, pills (subkind, `boot`,
  `sliced`, `derived`). Derived 1:1 sprites render only as indented children under
  their texture, **default collapsed** with a count pill. A **refused** asset gets a
  small amber triangle on its row; queued gets no row marker.
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
events. Last-known-good on `nullopt` (§3.2).

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
- **Focus is mandatory in practice:** default focus = boot scene; "everything" is
  allowed only under a node cap (~100) before the lens demands a scope.
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
| dock tab strip / toolbar wells / bottom bar | 30px / 24px / 24px |
| table rows / rail rows / group rows | 24px / 26px / 24px (chrome bg) |
| rail / preview pane widths | 180px / 330px |
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
