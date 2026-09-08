# Asset Manager Plan 3 — Graph Lens Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land spec §10 — the Graph lens: assets as nodes, references as edges,
focus scoping with depth/breadth caps, layered layout, and pin-drag
`Derive Instance…` creation — completing the one-panel/three-lens design.

**Architecture:** A pure `AssetGraphViewModel` (new, headless-testable) projects
`AssetPanelModel::Entries()` + `RefIndex()` into a scoped node/edge/layer set
(BFS depth ≤2 from the focus, per-node breadth cap ~20 with synthetic "+N more"
overflow nodes, columns by dependency depth). The lens body draws it with
**`ax::NodeEditor`** — the same vendored library the shader editor's canvas is
built on (already linked; pan/zoom, hit-testing, selection, context menus and
the drag-to-connect gesture come from it) — plus the shader editor's proven
idioms: `GraphGridPhase` for the grid, the hand-drawn header band, `DrawPinDot`
-style pins, and the two-layer transparent-link + hand-drawn-bezier trick that
makes per-kind edge colors, labels, selected-edge brightening and the dashed
in-flight bezier (the arc's one new draw technique) possible. Pin-drag lands as
the FIRST producer of the existing `createPrefillParent` limb. A riders task
pays the ledger's small debts, including the EditorLock zombie fix.

**Tech Stack:** C++23, ImGui 1.92.9 + imgui-node-editor (both vendored), Catch2
(ArcaneTests), premake5/msbuild (VS18).

**Spec:** `docs/specs/2026-09-06-asset-manager-redesign-design.md` — read it
first; this plan implements §10, the Plan-3 rows of §11.1–§11.3, and closes the
arc. §17/§18 record what Plans 1–2 actually landed — the panel this plan
extends is theirs. The mocks bind: `renders/OptionD-Graph-FINAL.png` is the
Graph redline, the Demo board binds behavior (drag-off-a-pin derive end-to-end;
Focus-in-Graph jumps lenses), `Interactions-FINAL.png` binds hover/click
(Graph tooltip adds an edge-summary line and brightens the node's own edges).

## Global Constraints

- Build: `Arcane.slnx` via VS18 msbuild
  (`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`),
  never a bare `.vcxproj`; `ARCANE_SDK` may be stale in the process env.
- Tests: run `ArcaneTests.exe` FROM the exe dir
  (`bin\Debug-windows-x86_64-md\ArcaneTests\`); capture the Catch2 seed banner;
  `~[gpu]` for baseline-comparable runs.
- ABI: **no bump anticipated** — Plan 3 is editor-side; the riders touch engine
  code only as a behavior bugfix (EditorLock) and a header-only predicate share,
  neither a layout/API/format change. If any task finds itself growing an
  engine seam, STOP and surface it to the controller.
- Editor sources are premake-globbed; **ArcaneTests production TUs are
  explicit** — `AssetGraphViewModel.cpp` must be added to the ArcaneTests
  `files{}` block (the `AssetPanelModel.cpp` pattern, `premake5.lua:826-848`;
  do NOT copy the redundant test-TU line at `:840` — test TUs are globbed via
  `:656`). Then re-run `GenerateProjects`.
- Fidelity: §11.2 Plan-3 values verbatim — nodes w 180–220, header 24px, accent
  bar 3px, pins 9px; §11.3 kind hexes verbatim (Texture `#b06a5b`, Material
  `#6a9b5b`, Mesh `#5b9bb0`, Sprite `#9b5bb0`, Scene `#b09b5b`). Selection
  amber / hover cyan follow the editor-wide language
  (`ShaderEditorDocument.cpp:246-250`); the canvas surface is `Theme::kPanel`
  (the `kCanvasColor` precedent, `:233-238`).
- Every ImGui table: `NoSavedSettings`. Square corners in panel chrome; the
  `ed::` node rounding (4.0f) is the canvas's own language — keep it (the
  shader editor's nodes round; the mocks' graph nodes do too).
- **Visual keystone:** `.superpowers/design/asset-manager-mockups/README.md` —
  `renders/OptionD-Graph-FINAL.png` at 960×620; structural comparison per §11.2,
  never a pixel diff. The Demo board's graph vignette (amber-glow pin, dashed
  amber in-flight edge, ghost menu with parent pre-picked) binds the pin-drag
  behavior.
- Commit after every task; do NOT push — the push follows the user's desk pass
  (Task 7).

## Design rulings pinned by this plan (argue with the plan, not the executor)

1. **The lens uses `ax::NodeEditor` (`ed::`).** Spec §10's "reuses the shader
   editor's canvas vocabulary" IS this library — the shader editor has no
   hand-rolled canvas (228 `ed::` sites, `ShaderEditorDocument.cpp:185`), and
   pin-drag creation (mandatory, §10) is the library's shipped, desk-debugged
   gesture. The **graph-framework extraction stays deferred** (standing user
   directive, 2026-07-24): no schema/undo/serialization layer is invented; all
   `ed::` usage is lens-local in `AssetsPanel.cpp`, mirroring
   `CanvasPopupScope.hpp:16-19`'s explicit refusal to couple shared widget
   headers to the node editor.
2. **No canvas persistence.** `ed::Config::SettingsFile = nullptr` (the library
   persists node positions by default — spec §10 says layout is computed each
   build, not persisted). Node positions are written from the view model's
   layout every rebuild via `ed::SetNodePosition`; the library's default node
   dragging stays enabled but repositioning is **transient by design** — the
   next model rebuild snaps back to computed layout. Recorded as intended
   behavior, not a bug.
3. **Graph nodes are NOT `ARCANE_ASSET` drag sources in v1.** The canvas owns
   drag gestures (pan, box-select, node reposition, pin-drag-create); stacking
   an ImGui drag-drop source on nodes fights all four. §8's drag clause is
   satisfied by every other representation; deferred with this rationale,
   recorded for the §19 addendum. (Right-click context menu and double-click
   DO apply to nodes — they don't conflict.)
4. **Graph gets its own selection stamp** — `state.seenSelectionStampGraph`.
   The existing `seenSelectionStamp` is a single shared field with Browse as
   its only consumer; a second consumer on the same field swallows the other's
   pending scroll/center. Browse's field and idiom are untouched.
5. **Kind colors are a panel-local table** in `AssetsPanel.cpp`'s anonymous
   namespace — `KindAccentColor(AssetKind)` in the `PinColorForWidth` shape
   (`ShaderEditorDocument.cpp:491`). `EditorTheme.hpp:27-32` rules domain
   color-coding out of the theme; the `kPillAmberBorder` precedent
   (`EditorWidgets.cpp:304`) covers spec-pinned hexes with no token.
6. **Focus scoping:** default focus = boot scene; BFS depth ≤ 2 from the focus
   in BOTH directions (outbound refs and inbound referencers); per-node breadth
   cap ~20 with a synthetic "+N more" overflow node per truncated side, edges
   sorted most-important-first (`DerivesFrom` before `References`) before the
   cut (§10 verbatim). **"Everything" = no depth cut** (there is no root to
   measure from), breadth caps still apply per node.
7. **Edges use the two-layer trick** (`ShaderEditorDocument.cpp:311-357` — read
   that comment before copying): `ed::Link` submitted fully transparent for
   hit-testing/selection, visible curve hand-drawn into the links channel with
   the `LinkStrength=100 / (1,0) / (−1,0)` control-point convention. This is
   what per-kind edge coloring, mid-edge labels, selected-node edge
   brightening, and tombstone-edge styling need; the library's solid
   uniform-color links can do none of them.
8. **The dashed in-flight bezier is a hand technique** (spec §11.1's "one new
   technique"): during an active create query, the from-pin→mouse curve is
   drawn dashed amber via `CubicBezierAt` + an on/off arc-length phase
   accumulator over the existing per-segment loop skeleton
   (`ShaderEditorDocument.cpp:5484-5496`; read `:5480-5483` on caps first).
   `ed::Flow`'s marching dots are NOT used — not the mock's language.
9. **Edge display labels:** `DerivesFrom` → "derives"; `References` → "uses",
   EXCEPT a material-kind source's `References` edges → "samples" (§10: a
   display label derived from the source asset's kind, not a third
   `AssetRefKind`). Drawn mid-edge, 12px dim; the render comparison against
   OptionD arbitrates final placement.
10. **No digest→Graph deep-link.** The spec routes the digest to Status
    (§5, `spec:175`); the only reserved Graph deep-link is the Scenes rollup's
    Focus-in-Graph button. Do not invent others.
11. **Tombstone nodes render.** A `RefIndex()` node with
    `exists == false && !inbound.empty()` is a dangling target — drawn as a
    ghost node (dim body, `kAmber` border accent, name = short guid, "missing"
    pill) so the §9.1 dangling story finally has pixels. This is Graph-lens
    vocabulary, not a Status card (Ruling 13 of Plan 2 deferred the UI;
    the graph is its natural home and §10's node set is "nodes/edges straight
    from the index", tombstones included).
12. **"N of M" in the bottom bar:** N = real asset nodes in the current graph
    build (synthetic overflow nodes excluded), M = `health.total`.
    `AssetReferenceIndex::NodeCount()` is NOT a UI number (its own doc says so).
13. **Riders (Task 1) pay recorded debt:** the EditorLock zombie fix (a
    desk-found, twice-blocking defect — check the `exited` FILETIME
    `GetProcessTimes` already returns), the `{hi,lo}` shape-predicate share,
    the index self-reference test, the boot-guid parse hoist (Plan 3 would be
    its third copy), the redundant premake `:840` line, and the
    `kStatusPillLineHeight` duplicate. Each is a one-liner-to-small item with a
    ledger citation; none expands scope beyond its citation.
14. **In-canvas tooltips may need `CanvasPopupScope`** — inside
    `ed::Begin/End` the editor moves ImGui into canvas space
    (`DrawCanvasBackdrop`'s `:5320-5324` rationale). The executor must verify
    where `DrawAssetPeekTooltip` can be called for node hover (likely after
    `ed::End`, keyed off `ed::GetHoveredNode()`, or inside a Suspend bracket)
    and record which in the report — do not fight the canvas.

---

### Task 1: Riders — paid debt + shared foundations

**Files:**
- Modify: `ArcaneClient/src/Arcane/Project/Project.cpp` (EditorLock `ReadLive`, `:603-636`)
- Modify: `ArcaneClient/src/Arcane/Serialization/IdentityFieldRule.hpp` (gains `IsGuidShapedJson`)
- Modify: `ArcaneClient/src/Arcane/Serialization/ReflectionJson.hpp` (collector uses the shared predicate)
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp` (`ScanSceneJson`'s shape test uses it too)
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (boot-guid hoist: one file-local `BootSceneGuid(project)` helper replacing the two copies at `:1712-1714` and `:2501-2503`; `kStatusPillLineHeight` replaced by an exported widget constant)
- Modify: `ArcaneEditor/src/Widgets/EditorWidgets.hpp/.cpp` (export `kPillLineHeight`)
- Modify: `premake5.lua` (delete the redundant `:840` AssetReferenceIndexTest entry)
- Test: `ArcaneTests/src/AssetReferenceIndexTest.cpp` (self-reference case), `ArcaneTests/src/ProjectTest.cpp` or the suite housing EditorLock coverage (zombie-lock case — find where EditorLock is tested; if untested, add the case beside Project tests)

**Interfaces:**
- Consumes: `EditorLock::ReadLive`'s existing `GetProcessTimes` call (the
  `exited` FILETIME is already fetched and discarded); the two byte-identical
  `{hi,lo}` shape tests (`ReflectionJson.hpp` collector, `Assets.cpp:155-157`).
- Produces: `Arcane::IsGuidShapedJson(const nlohmann::json&) -> bool` in
  `IdentityFieldRule.hpp` (both TUs delegate); `EditorLock` treating an
  exited-but-handle-held process as stale; `Editor::kPillLineHeight` exported.

- [ ] **Step 1: Write the failing tests.**

Zombie-lock: the honest headless approximation — a lock naming the CURRENT
process's pid but with `start` matching AND the process **not** exited cannot be
faked; what CAN be pinned is the new discipline's core: a lock whose pid has a
**nonzero exit time** reads as stale. On Windows the test can spawn a
short-lived child (`cmd /c exit 0`), keep its handle open (the zombie), write a
lock with its pid+creation-time, and assert `ReadLive` returns `nullopt`:

```cpp
TEST_CASE("EditorLock: a lock held by an exited process is stale, even while a handle keeps the pid reserved", "[project]")
{
    // Spawn a child that exits immediately; KEEP the handle open so the pid
    // stays reserved (the Hub-zombie repro from the 2026-09-08 desk pass).
    // Write a lock with that pid + its real creation time.
    // ReadLive must answer nullopt -- the process has a nonzero exit time.
}
```

(NOTE for the executor: use `CreateProcessW` + `WaitForSingleObject` + keep
`hProcess` open; fetch its creation FILETIME via `GetProcessTimes` for the lock
body; close the handle at test end. Verify `EditorLock::Write`'s JSON shape at
`Project.cpp:545-551`.)

Shape-predicate share: the Task-2-era equivalence test already trips if the two
predicates drift — add one direct case in `SceneAssetTest.cpp` or
`AssetReferencesTest.cpp` only if the share cannot be proven by the existing
suite (executor's call; say which in the report).

Self-reference index case (Plan-2 T3 deferred minor):

```cpp
TEST_CASE("AssetReferenceIndex: a self-referencing asset neither corrupts nor leaks on re-walk and delete", "[editor]")
{
    idx.Update(A, true, Refs({ {A, References} }));
    CHECK(idx.InboundCount(A) == 1);
    idx.Update(A, true, Refs({}));       // re-walk away from itself
    CHECK(idx.InboundCount(A) == 0);
    idx.Update(A, true, Refs({ {A, References} }));
    idx.Update(A, false, std::nullopt);  // delete while self-referenced
    CHECK(idx.Find(A) == nullptr);       // no tombstone survives a self-only ref
}
```

- [ ] **Step 2: Run to verify the new cases fail** (`[project]`, `[editor]`
  from the exe dir).

- [ ] **Step 3: Implement.**

3a. EditorLock (`Project.cpp:614-630` region): the fix is one check in
`ReadLive` — `GetProcessTimes` already fills `exited`; a nonzero exit time
means the process is dead regardless of the handle keeping its pid reserved:

```cpp
const bool ok = ::GetProcessTimes(h, &created, &exited, &kernel, &user);
::CloseHandle(h);
if (!ok)
    return std::nullopt;
// A process object can outlive its process: a parent (the Hub) holding the
// handle keeps the pid reserved and OpenProcess succeeding, with the original
// creation time intact -- the 2026-09-08 desk-pass zombie. Exit time is the
// tell: nonzero means it exited, however many handles remain.
if (exited.dwHighDateTime != 0 || exited.dwLowDateTime != 0)
    return std::nullopt;
```

Update the `:615-617` comment ("the validation that defeats staleness") to name
all three tells: liveness, creation-time match, and zero exit time.

3b. `IsGuidShapedJson` in `IdentityFieldRule.hpp` (the object/size-2/hi+lo/
unsigned test, verbatim semantics); `ReflectionJson.hpp`'s collector and
`Assets.cpp`'s `ScanSceneJson` both delegate; delete the "provably the same
rule" hand-sync comments in favor of "the same function".

3c. Boot-guid hoist + `kPillLineHeight` export + premake line removal — small
mechanical edits per the Files list; `AssetsPanel.cpp`'s two parse copies
become calls to the one helper (the third consumer, Task 4, uses it too).

- [ ] **Step 4: Build Debug; run `[project]`, `[editor]`, `[assets]`, `[scene]`
  green from the exe dir; full `~[gpu]` Debug run green.**

- [ ] **Step 5: Commit** —
`fix(editor,engine): plan-3 riders -- zombie-lock staleness, shared guid-shape predicate, small paid debt`

---

### Task 2: `AssetGraphViewModel` (pure unit)

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetGraphViewModel.hpp`
- Create: `ArcaneEditor/src/Panels/AssetGraphViewModel.cpp`
- Modify: `premake5.lua` (ArcaneTests `files{}`: add the production TU with a house-style comment; the test TU is globbed)
- Test: `ArcaneTests/src/AssetGraphViewModelTest.cpp` (new)

**Interfaces:**
- Consumes: `AssetPanelModel::Entries()` shape (`unordered_map<Guid, AssetPanelEntry>` — but take it as injected data, NOT the model type: the unit stays model-free for testability), `AssetReferenceIndex` (`Find`/`Node`), `AssetRefKind`.
- Produces (Tasks 3-6 call exactly these):

```cpp
namespace Arcane::Editor
{
    // One node in the built graph. Synthetic overflow nodes ("+N more") have
    // isOverflow=true, a nil guid is NOT used for them -- they carry the guid
    // of the node they overflow FROM plus a direction tag, so ids stay stable.
    struct GraphNode
    {
        Arcane::Guid guid;                 // real asset, or overflow anchor
        std::string  label;                // name, or "+N more"
        AssetKind    kind = AssetKind::Other;
        int          layer = 0;            // column, 0 = left (sources)
        int          row = 0;              // stacking index within the column
        bool         isOverflow = false;
        bool         overflowInbound = false; // which side it truncates
        int          overflowCount = 0;
        bool         isTombstone = false;  // dangling target (exists=false)
    };

    // Edge with its display label already decided (ruling 9).
    struct GraphEdge
    {
        Arcane::Guid from;                 // referencer
        Arcane::Guid to;                   // target
        Arcane::AssetRefKind kind = Arcane::AssetRefKind::References;
        const char* label = "uses";        // "derives" | "uses" | "samples"
    };

    struct GraphBuildInput
    {
        // Injected snapshots -- the unit never touches AssetPanelModel.
        const std::unordered_map<Arcane::Guid, AssetPanelEntry>* entries = nullptr;
        const AssetReferenceIndex* index = nullptr;
        Arcane::Guid focus;                // nil = "everything" (ruling 6)
        int depthLimit = 2;                // per direction, from focus
        int breadthCap = 20;               // per node per direction
    };

    struct AssetGraphViewModel
    {
        std::vector<GraphNode> nodes;      // real + overflow, layer/row assigned
        std::vector<GraphEdge> edges;      // between real nodes only
        int realNodeCount = 0;             // "N of M"'s N (ruling 12)

        void Build(const GraphBuildInput& in);
        void Clear();
    };
}
```

- [ ] **Step 1: Write the failing tests** (headless, hand-built entries map +
  index — the `AssetReferenceIndexTest` posture; tag `"[editor]"`, case names
  `"AssetGraphViewModel <behavior>"`). Cases:

```cpp
TEST_CASE("AssetGraphViewModel layers by dependency depth: sources left, scenes right", "[editor]")
// texture <- sprite <- scene chain: layer(texture)=0, sprite=1, scene=2;
// a material sampling the texture shares the sprite's layer only if its own
// longest outbound chain says so -- layer = longest outbound-path length to a leaf.

TEST_CASE("AssetGraphViewModel scopes to focus: BFS depth 2 both directions", "[editor]")
// chain A->B->C->D->E focused on C: A..E minus the ends beyond depth 2;
// inbound and outbound both walk.

TEST_CASE("AssetGraphViewModel caps breadth with a synthetic overflow node, most-important edges first", "[editor]")
// one texture with breadthCap+5 inbound referencers, a mix of DerivesFrom and
// References: DerivesFrom edges survive the cut first (§10), overflow node
// carries "+5 more", overflowInbound=true, and is EXCLUDED from realNodeCount.

TEST_CASE("AssetGraphViewModel renders tombstones", "[editor]")
// index has a dangling target (exists=false, inbound nonempty), no entry for
// it: node appears with isTombstone=true, label = short guid form.

TEST_CASE("AssetGraphViewModel everything-mode applies no depth cut but keeps breadth caps", "[editor]")

TESTS also pin: deterministic row order (name-sorted within a column);
edge labels (derives / uses / samples per ruling 9, including the
material-source special case); Clear().
```

- [ ] **Step 2: premake + generate + build; run to verify failure.**

- [ ] **Step 3: Implement.** Layering: `layer(n) = longest outbound-path length
  to a leaf`, memoized DFS over the scoped node set (cycles: the index can
  theoretically hold one — guard with an on-stack set; a cycle member's layer
  is the max of its non-cycle continuations, and the build must not hang —
  add a cycle test if the guard is nontrivial). Rows: name-sorted stacking per
  column. Overflow: per node per direction, after sorting that node's edge
  list `DerivesFrom` first; the synthetic node's `row` stacks beneath the
  survivors. Edges to/from culled nodes are dropped with their side's overflow
  accounting.

- [ ] **Step 4: `[editor]` green; full `~[gpu]` Debug green.**

- [ ] **Step 5: Commit** —
`feat(editor): AssetGraphViewModel -- scoped, layered, breadth-capped graph projection`

---

### Task 3: Graph lens I — canvas, nodes, edges

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.hpp` (state gains the Graph fields: `ed::EditorContext*` handle **as an opaque `void*` or forward-declared pointer** — do NOT include `imgui_node_editor.h` in the panel header; `GraphGridPhase graphGrid;` is fine (`GraphGridPhase.hpp` is ImGui-only); `Arcane::Guid graphFocus;` ; `std::uint32_t seenSelectionStampGraph = 0;`)
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (`DrawGraphLens` + helpers in the anonymous namespace; `KindAccentColor` table; the `else` branch at `:2566-2571` becomes the dispatch)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` / `EditorAppProject.cpp` (context lifetime: destroy on project switch beside `ResetForProjectSwitch` and on shutdown — find where panel state is owned and add the `ed::DestroyEditor` calls; NOTE: the destroy calls live app-side where including the node-editor header is already precedented via documents)
- Test: none headless for the draw body (house precedent) — but see Step 5's device-less canvas test.

**Interfaces:**
- Consumes: Task 2's view model; `GraphGridPhase.hpp` (`GraphGridView`/`DrawGraphGridFallback`); the shader editor's idioms by citation (copy the SHAPE, not the file): `ApplyGraphCanvasStyle` (`ShaderEditorDocument.cpp:576`), `DrawNodeTitleBand` (`:555`, drawn after `EndNode` via `GetNodeBackgroundDrawList`), `DrawPinDot` (`:507` — radius becomes 4.5f for §11.2's 9px), `DrawGradientWire`'s control-point convention (`:5427-5446`) + two-layer channel discipline (`:311-357`, `:5454-5457`), `ViewScale`'s reciprocal trap (`:441`), `ed::ScreenToCanvas` only before `ed::Begin` (`:5317-5341`), unconditional `ed::EndCreate` (`:5559-5561` — the frame-2 crash).
- Produces: `DrawGraphLens(state, model, project, docs, services, actions)` (the `DrawStatusLens` signature shape); nodes rendered per §10 anatomy; solid per-kind edges with labels; selected node's edges brighten; tombstone ghost nodes.

- [ ] **Step 1: Canvas foundation.** Lazy-create the `ed::EditorContext` with
  `config.SettingsFile = nullptr` (ruling 2); apply an
  `ApplyGraphCanvasStyle`-shaped style block (transparent `StyleColor_Grid`/
  `Bg`; node body/border from the shader editor's constants; selection amber /
  hover cyan per the editor-wide language). Grid: fill a `GraphGridView` from
  the canvas rect + `ViewScale()` (mind the reciprocal trap) and call
  `DrawGraphGridFallback` before `ed::Begin`, exactly as `DrawCanvasBackdrop`
  does. Rebuild the view model when `model` rebuilt or focus changed (cheap
  dirty check: stamp the model's rebuild via a counter the panel can compare,
  or rebuild the view model per-frame ONLY if `Entries()` size/stamp moved —
  executor picks the cheapest honest trigger and documents it; per-frame full
  rebuild is NOT acceptable).
- [ ] **Step 2: Nodes.** Per view-model node: `ed::SetNodePosition` from
  layer/row (column pitch ~260px, row pitch ~90px — tuning values, render
  comparison arbitrates), `ed::BeginNode`, §10 anatomy: header row (kind icon +
  name, 24px band hand-drawn after `EndNode` per the title-band idiom), 3px
  kind-accent bar (hand-drawn beside the band, `KindAccentColor`), body row
  (18px thumb via `services.resolveAssetThumb` or kind icon + meta line), pins:
  one input pin (left, inbound/referencers) + one output pin (right, outbound/
  refs) drawn `DrawPinDot`-style at 9px, kind-colored, filled iff connected.
  Width clamped 180–220 (§11.2). Overflow nodes: dim body, "+N more" label, no
  pins. Tombstones per ruling 11.
- [ ] **Step 3: Edges.** Transparent `ed::Link` per edge + hand-drawn cubic in
  the links channel using the documented convention; color = source-kind accent
  dimmed, brightened when either endpoint is the selected node; mid-edge 12px
  dim label per ruling 9 (skip labels while `ViewScale()` is below the LOD the
  executor tunes — cite the shader editor's LOD tiers).
- [ ] **Step 4: Wire the dispatch** (`else if (state.lens == AssetLens::Graph)
  DrawGraphLens(...)`) — but do NOT enable the mask yet (Task 5 flips it; until
  then the branch is reachable only programmatically, which Step 5 exploits).
- [ ] **Step 5: Device-less canvas test.** Mirror
  `ArcaneTests/src/GraphCanvasHeadlessTest.cpp` (the 4-frame load-bearing loop)
  with a new `[editor][graphcanvas]` case that forces `state.lens = Graph` and
  drives `DrawAssetsPanel` four device-less frames over a small real model —
  this is exactly the harness that caught the shader editor's frame-2
  `EndCreate` crash. (This requires `AssetsPanel.cpp` in the test link — it
  already is NOT; check how `GraphCanvasHeadlessTest` links
  `ShaderEditorDocument` (`premake5.lua:885-887`) and mirror: add
  `AssetsPanel.cpp` + its transitive needs to the ArcaneTests files block IF
  the link closes; if the transitive closure explodes, report
  DONE_WITH_CONCERNS naming what it needs and the controller rules.)
- [ ] **Step 6: Build; `[editor]` + `[graphcanvas]` green; full `~[gpu]` Debug
  green; commit** —
`feat(editor): Graph lens canvas -- ed:: foundation, layered nodes, two-layer kind-colored edges`

---

### Task 4: Graph lens II — interactions

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (`DrawGraphLens` grows; `DrawAssetPeekTooltip` gains the edge-summary option)

**Interfaces:**
- Consumes: `model.Select`/`selectionStamp`; `state.seenSelectionStampGraph` (ruling 4); `ed::GetHoveredNode`/`GetDoubleClickedNode`/`ShowNodeContextMenu`/`SelectNode`/`NavigateToSelection`; `CanvasPopupScope`; the row context-menu action set (`DrawRowContextMenu`, `AssetsPanel.cpp:581` — reuse its action-emitting body for nodes, NOT a copy: extract the shared menu-items helper if needed).
- Produces: click selects (bridged to the model); external selection centers once; peek tooltip with edge-summary line; double-click opens (same routing as rows — scenes via `actions.openScene`, documents via `docs`); right-click = the unified asset context menu.

- [ ] **Step 1: Selection bridge.** Node click → `model.Select(guid)` (use
  `ed::` selection callbacks/queries; keep `ed::` selection and model selection
  in sync one-way each frame: model is the authority). External change
  (`state.seenSelectionStampGraph != model.selectionStamp`): if the selected
  guid has a node, `ed::SelectNode` + `ed::NavigateToSelection` ONCE, then
  acknowledge the stamp; if absent (filtered out of the scope), acknowledge
  silently (the Browse idiom at `:1188-1202`).
- [ ] **Step 2: Peek tooltip.** `DrawAssetPeekTooltip` gains
  `bool withEdgeSummary = false`; the summary line reads from
  `model.RefIndex().Find(guid)` — "N in · M out" plus up to three named
  targets. Call it for `ed::GetHoveredNode()` per ruling 14 (verify the
  canvas-space constraint; record where the call landed). Hovered node's own
  edges brighten (same mechanism as selection brightening).
- [ ] **Step 3: Double-click + context menu.** Double-click routes exactly as
  rows do (verbatim behavior clause, spec §6). Right-click:
  `ed::ShowNodeContextMenu` → `CanvasPopupScope` → the same menu items the row
  context menu emits (kind-specific entries, `Create ▸`, Show in Explorer,
  Copy Path, Copy Guid) — share the emitting code, don't fork it.
- [ ] **Step 4: Build; `[editor]` + `[graphcanvas]` green; commit** —
`feat(editor): Graph lens interactions -- selection bridge, peek+edges, context menu, double-click`

---

### Task 5: Focus UI + enable the lens

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (toolbar combo; mask `0b101u`→`0b111u`; bottom bar; `DrawSceneCard` signature + Focus-in-Graph enable)
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.hpp` (if the focus field didn't land in Task 3)

**Interfaces:**
- Consumes: `state.graphFocus` (nil = everything); `BootSceneGuid(project)` (Task 1's hoist); the toolbar layout math at `:385-406` (no slot exists — ADD the combo between search and the strip and subtract its width + one `ItemSpacing.x` at `:392-393`); `DrawBottomBar`'s branch chain `:457-472`; `DrawSceneCard` `:2260` + its call site `:2515` + the disabled button `:2302-2304`.
- Produces: the focus combo (label = focused scene name or "everything"; entries = "everything" + each Scene entry, name-sorted); Graph's bottom-bar line `"N of M · focus: <name>"`; the Graph button enabled; Focus in Graph live.

- [ ] **Step 1: Toolbar combo.** Per-lens: drawn only when
  `state.lens == Graph` (spec §5 — the slot is empty on other lenses). Default
  `state.graphFocus` = boot scene guid on project open (reset with the panel's
  project-switch reset; find where `state` is reset and add it).
- [ ] **Step 2: Bottom bar.** New leading branch
  `else if (state.lens == AssetLens::Graph)` — `"%d of %d \xC2\xB7 focus: %s"`
  with N/M per ruling 12 and the focus name ("everything" when nil). **Widen
  `char left[64]` to 128** (survey: a real scene name overflows 64).
- [ ] **Step 3: Enable.** Mask → `0b111u` + comment rewrite (all three lenses
  live; the strip layout never moves — Plan-1's pinned-layout promise pays off
  here). Replace the now-dead backstop comment at the dispatch.
- [ ] **Step 4: Focus in Graph.** `DrawSceneCard` gains `AssetsPanelState&`
  (thread it from `:2515`); the button un-disables:
  `state.graphFocus = e.guid; state.lens = AssetLens::Graph;` (+
  `model.Select(e.guid)` so the graph centers on the scene — the Demo board's
  "Focus in Graph jumps lenses" behavior).
- [ ] **Step 5: Build; suites green; render comparison** — capture the Graph
  lens (the Task-3 device-less path can't screenshot; use the Task-7 flow's
  temporary lens-default + `--screenshot` trick from Plan 2's precedent) and
  compare structurally vs `OptionD-Graph-FINAL.png` (§11.2 values; node
  anatomy; layer direction sources-left). Record deviations. **Commit** —
`feat(editor): Graph lens live -- focus combo, bottom bar, Focus-in-Graph, mask 0b111`

---

### Task 6: Pin-drag `Derive Instance…`

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (the create-query handling in `DrawGraphLens`; the dashed in-flight bezier)

**Interfaces:**
- Consumes: `ed::BeginCreate`/`QueryNewNode`/`AcceptNewItem`/`RejectNewItem`/`EndCreate` (UNCONDITIONAL EndCreate — the `:5559-5561` folklore); the three-hop gesture shape (`:5510-5562` accept in canvas space → stash → `:4019-4038` open popup in a Suspend block); `CanvasPopupScope`; `actions.requestCreateKind` + `actions.createPrefillParent` (**their first real producer** — consumer verified structurally at `EditorAppFrame.cpp:2328-2334`); `CreateKindForAssetKind` (`CreateAssetDialog.hpp:189-199`).
- Produces: dragging off a **material-kind node's** output pin and releasing over empty canvas opens a ghost menu (`CanvasPopupScope` + `BeginPopup`) whose one entry `Derive Instance…` sets `requestCreateKind = MaterialInstance` and `createPrefillParent = <node guid>`; the in-flight curve draws dashed amber (ruling 8); non-material sources show the menu entry disabled (the `DrawCreateMenuEntries` `enabled` param was kept for exactly this producer, `:336-337`).

- [ ] **Step 1:** the create-query handling (accept → stash guid + `wireActive`
  flag → popup next block), dashed amber in-flight bezier drawn while the
  query is live (from-pin pivot → mouse, canvas space).
- [ ] **Step 2:** the ghost menu → actions; dialog opens pre-parented (verify
  live: the parent field arrives filled, picker closed —
  `EditorAppFrame.cpp:2490-2495`).
- [ ] **Step 3:** the device-less canvas test gains a frame that walks the
  create-query path far enough to prove `EndCreate` is unconditional (the
  crash class the exemplar exists for).
- [ ] **Step 4: Build; suites green; commit** —
`feat(editor): Graph pin-drag -- Derive Instance via pre-parented CreateAssetRequest, dashed in-flight bezier`

---

### Task 7: Gate, re-bless, baselines, spec §19, desk checklist

Mirror Plan 2's Task 9 discipline exactly (its brief and §18 are the template):

- [ ] **Step 1:** Three-config build 0 warnings; `~[gpu]` per config + one
  unfiltered Debug run; derive-never-recall with seeds.
- [ ] **Step 2:** Baselines re-derived; note attributes Plan 3's rise per-suite
  (and any rider-test rise separately).
- [ ] **Step 3:** Golden gate: the editor-ui lane diffs (the lens strip's Graph
  button goes enabled-bright) → read the artifact FIRST (confined to the
  Assets panel band), bless SOURCE dx12, restage BOTH hosts md5-proven, rerun
  green (`gatePassed` + verdicts from JSON), `-SelfTest` observed failing.
  **Copy the diff artifacts into the workspace BEFORE any gate re-run** (they
  are deleted per-lane on the next run — the Plan-2 lesson).
- [ ] **Step 4:** Spec addendum **§19 LANDED (Plan 3)**: scope, measured close,
  every ruling above that shipped as behavior (1-14, including the
  drag-source deferral and the tombstone-rendering decision), deviations, and
  a dated correction to §15's follow-up list where items landed (dangling-ref
  UI; the graph scoping).
- [ ] **Step 5:** DESK-CHECKLIST.md (present, don't self-certify; machine
  claims stamped with commits): side-by-side vs OptionD-FINAL at ~960×620;
  focus combo (boot default, everything, a scene); depth/breadth caps visible
  (needs a scene with enough assets — Gacha's, if mounted, else note the
  fixture's small graph shows no overflow and the unit tests carry the caps);
  selection sync both directions (click node → Browse follows on switch; select
  in Browse → Graph centers once); tooltip + edge brightening; double-click
  opens; context menu parity; pin-drag derive end-to-end (dialog pre-parented,
  instance lands selected); dashed in-flight edge; tombstone ghost (break a
  ref by hand, temporarily); Focus-in-Graph from Status; transient reposition
  snapping back on rebuild (recorded as intended); restore-the-tree before
  push.
- [ ] **Step 6: Commit** — `chore(editor): plan-3 gate + baselines catch-up`;
  do NOT push (the user's desk pass gates it).

---

## Self-review notes (kept for the executor)

- Spec §10 maps: nodes/edges/labels → T2+T3; scoping → T2+T5; layout → T2;
  canvas vocabulary → T3; pin-drag → T6; node anatomy/§11.2/§11.3 → T3;
  Focus-in-Graph → T5; §8's contract on graph surfaces → T4. §11.1's Plan-3
  widget row lands as lens-local drawing per ruling 1/5 (the CanvasPopupScope
  precedent rules shared-header membership out; §11.1's "shared EditorWidgets
  layer" phrasing yields to the stronger no-coupling precedent — record as a
  deviation in §19 if the reviewer flags it).
- The view model is deliberately model-type-free (`GraphBuildInput` takes the
  entries map + index by pointer) so its tests never construct an
  `AssetPanelModel`.
- Two "verify live before building on it" items: `createPrefillParent` has
  ZERO producers today (T6 is the first — the consumer limb is structurally
  verified, `EditorAppFrame.cpp:2328-2334`); in-canvas tooltip placement
  (ruling 14).
- The `ed::` context must never leak across project switches (destroy beside
  the model's `ResetForProjectSwitch` seam) and `SettingsFile` must stay null —
  the library writing a json beside the exe would be a silent new artifact.
- Expected gate outcome at T7: editor-ui diff confined to the lens-strip
  region (the Graph button brightens); anything wider means a Browse-lens
  regression — investigate before blessing.
