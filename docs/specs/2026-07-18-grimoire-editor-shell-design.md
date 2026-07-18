# Grimoire Editor Shell — Design (Epic 04 second half, M7)

**Date:** 2026-07-18
**Status:** Approved (brainstorm), pending user review of this document
**Epic:** ENG-E04 (`docs/jira/epic-04-grimoire-editor-shell.md`) — remaining subtasks E04-2, E04-3, E04-4
**Predecessor work (landed @8bf50fe9):** E04-1 sim-time control (RunLoop pause/single-step/time-scale) + 04.2 render interpolation + 04.3 Sandbox migration.
**Related specs:** `2026-06-11-engine-architecture-design.md` (M7 definition, host loop), `2026-06-12-arcane-2d-renderer-architecture.md` (render path).

---

## Goal

Stand up **Grimoire** — the native ImGui-on-NVRHI editor shell that wraps the engine
`RunLoop` and gives Arcane a real iteration loop against the live Astra registry. This
document covers the epic's remaining three subtasks as one coherent deliverable:

- **E04-2** — the `Grimoire.exe` host + dockable ImGui shell + sim-time toolbar + scene viewport.
- **E04-3** — a reflection-driven entity hierarchy + component inspector editing the live registry.
- **E04-4** — play-in-editor: load a hosted scene, Play/Stop the sim, restore authored state.

**Definition of Done (from the epic):** `Grimoire.exe` runs the RunLoop with working
pause/single-step/time-scale; a reflection inspector views and edits the live registry;
the shell hosts a plugin/scene and can start/stop the simulation in-editor.

---

## Governing principle: strict modularity (Astra → Manifold2D → Arcane → Grimoire)

The Starworks tool stack is strictly layered with one-way dependency arrows: Mosaic
(zero-dep core), Astra (ECS), Manifold2D (2D physics), **Arcane** (the game-engine
*library* consuming those), and **Grimoire** (the editor *on top of* Arcane). Arcane must
stay fully usable **editor-free** — a developer can build on Arcane without ever pulling
in Grimoire.

This yields the single most important design rule in this document:

> **Every capability the editor needs from the engine becomes public `ARCANE_API`
> surface. Grimoire consumes only that API. No editor hooks, no `#ifdef EDITOR`
> branches, and nothing named "editor" ever appears inside Arcane. The dependency
> arrow never reverses — Arcane must never reference Grimoire.**

This is the editor-side parallel of the standing "servers are supported *by* Arcane
tooling" rule: generic capability is built *in* Arcane as engine API; the
consumer-specific UI lives in the consumer (here, Grimoire). Concretely it drives three
engine-side additions in this epic — the plugin-host lift (§1), an entity-enumeration
API (§4), and a pick function (§6) — each a reusable engine capability, not an editor
special-case.

**Home:** Grimoire lives at `Arcane/Grimoire/` (sibling of `Arcane/Loom/`), an
executable project in `Arcane.slnx`. Arcane will eventually become its own repository;
housing Grimoire beside Loom keeps it a clean, independently-extractable consumer of
`Arcane.dll` in the meantime. Separation is enforced by dependency direction, not by
repository boundary — the same strangler pattern that lifted Manifold2D out of Core.

---

## Architecture overview

```
Grimoire.exe
├── Host spine (reused from Loom's decomposition)
│   ├── GpuContext        SDL3 window + NVRHI device/swapchain      (source-shared w/ Loom)
│   ├── FramePerf         frame timing/diagnostics                  (source-shared w/ Loom)
│   └── Arcane::RunLoop    sim-time control (pause/step/time-scale)  (ARCANE_API, E04-1)
├── Plugin host           loads Sandbox.dll (later Game.dll)         (LIFTED to ARCANE_API, §1)
├── ImGui shell (docking)
│   ├── DockSpace + default layout
│   ├── Toolbar panel      Play/Pause/Step + time-scale  → RunLoop
│   ├── Viewport panel     OffscreenCanvas → ImGui::Image (scene-in-a-panel)
│   ├── Hierarchy panel    live entity list             → SelectionContext
│   ├── Inspector panel    reflection field editors     → live registry
│   └── Console panel      engine-logger sink (basic)
└── Editor state
    ├── SelectionContext   the one selected-entity source of truth
    └── EditorMode         Edit | Play  (drives snapshot/restore + RunLoop pause)
```

The host loop is the one Loom and Grimoire share (per the architecture spec): pump SDL
→ Arcane input → accumulate fixed steps → plugin FixedUpdate/Update → render (scene +
ImGui) → present → plugin-watcher check. Grimoire differs only in *what UI it draws* and
in owning editor state; the loop itself is `Runtime::Loop()`, unchanged (no
Runtime-API or plugin-ABI change).

---

## §1 — Engine-side lift: plugin-host machinery into Arcane.dll

**Problem:** the plugin-host units (`PluginHost` 329 LOC, `Module` 77, `Plugin` 51) live
in `Arcane/Loom/src/`, but `ArcaneTests` already source-compiles them
(`premake5.lua:492-495`). They are shared infrastructure sitting in a host by accident.
Grimoire would be a third consumer — copy-compiling internals three ways violates the
modularity principle.

**Change:** move `PluginHost` / `Module` / `Plugin` from `Loom/src/` into
`Arcane/Arcane/src/Arcane/Plugin/` (beside the existing `PluginABI.hpp`), exported as
`ARCANE_API` in `namespace Arcane`. Loom, ArcaneTests, and Grimoire then all consume
`<Arcane/Plugin/PluginHost.hpp>` — one implementation. Hot-reload (versioned
copy-and-load, ABI check, whole-registry SaveState/LoadState, debounced watcher, last-good
rollback) comes to Grimoire for free.

**Scope discipline:**
- `GpuContext` and `FramePerf` are host boot/diagnostics helpers, not plugin machinery.
  They stay in `Loom/src/`; Grimoire source-compiles them (as ArcaneTests source-compiles
  the plugin units today). They lift only if a future need justifies it — not now (YAGNI).
- This is a **byte-identical strangler move**. No behavior change. The `[loom]` +
  hot-reload test suite passing unchanged is the acceptance gate.
- `PluginABI.hpp` already lives engine-side and is unchanged. No ABI-version bump.

**Interfaces produced:** `Arcane::PluginHost`, `Arcane::Module`, `Arcane::Plugin`
(names preserved; namespace becomes `Arcane`). Loom's includes change from `"PluginHost.hpp"`
to `<Arcane/Plugin/PluginHost.hpp>`; the premake `files{}`/`includedirs{}` for Loom and
ArcaneTests drop the moved sources.

---

## §2 — E04-2: host shell (dockspace + sim-time toolbar)

`Grimoire.exe` owns `GpuContext` → `Arcane::RunLoop` → `ImGuiLayer`, driven by
`Runtime::Loop()`, hosting `Sandbox.dll` by default (`--plugin <path>` to override, like
Loom). Shell specifics:

- **Docking:** enable `ImGuiConfigFlags_DockingEnable`; host a full-window dockspace over
  which the panels dock. Ship a baseline `imgui.ini` default layout (Viewport centre,
  Hierarchy left, Inspector right, Console bottom, Toolbar top) so first launch is usable;
  the user's rearrangements persist via ImGui's own ini.
- **Sim-time toolbar:** Play / Pause / Step buttons + a time-scale slider, wired to the
  E04-1 RunLoop API already on `Runtime::Loop()` — `SetPaused`/`IsPaused`,
  `RequestSingleStep`, `SetTimeScale`/`TimeScale`. (Play/Stop *semantics* — snapshot/restore
  — are §6; the toolbar's pause/step/scale are the raw RunLoop controls.)
- **Console panel (basic, v1):** a dockable panel backed by an spdlog sink attached to the
  engine logger (`Arcane::Log::Engine()`). Because the Mosaic-diagnostics work routes
  Manifold2D/Astra records into that one logger, physics WARNs (stale-handle ops, dropped
  non-finite AABBs, rejected inputs) surface **in the editor console**. v1 is a scrolling
  text view with autoscroll. **Future enhancement (out of v1 scope):** a structured
  **Problems tab** (severity filtering, source-category grouping, click-to-navigate) and a
  richer console (search, per-category toggles, coloring). Recorded, not built now.

**Menu/toolbar chrome** beyond the sim-time controls (File/View/etc.) is minimal in v1 —
a View menu to toggle panels + reset layout. No project/scene-file menu (no on-disk scene
format in this epic; see §7).

---

## §3 — E04-2: scene-in-a-panel viewport

The engine already provides the viewport primitive: `Arcane::OffscreenCanvas`
(`Render/OffscreenCanvas.hpp`, `ARCANE_API`) runs the canonical Canvas → Batcher2D →
Tonemap path into a display-referred texture and exposes it as an `ImTextureID` via
`TextureId()`, with `Draw(fn, clear)` and `Resize(w,h)`. `imgui_impl_nvrhi` already binds
arbitrary user textures (`SetTexID((ImTextureID)handle)`), so `ImGui::Image` of the
offscreen texture works today.

**Grimoire viewport panel:**
- Render the hosted scene into an `OffscreenCanvas` each frame (the scene's existing draw
  path — physics debug overlay + any sprites — supplied as the `Draw` lambda), then
  `ImGui::Image(TextureId(), avail)` inside the **Viewport** dock window.
- **Resize:** when the dock window's content-region size changes, `Resize()` the
  OffscreenCanvas to match (guard against zero/every-frame thrash).
- **Input gating (the one fiddly bit):** scene input (camera pan/zoom, click-pick) is
  consumed **only when the Viewport window is hovered and/or focused**
  (`ImGui::IsWindowHovered`/`IsWindowFocused` semantics); otherwise ImGui owns mouse and
  keyboard. This predicate is factored into a small pure function and unit-tested.
- **Unprojection:** the cursor is already in viewport-texture space (subtract the image's
  screen-rect origin). Convert to world space via the scene camera's `ScreenToWorld`
  seam (the same seam `Sandbox/src/Interaction.cpp` uses). The resulting world point
  feeds picking (§6).

The scene camera for the viewport is the hosted plugin's camera (Sandbox already owns a
pan/zoom camera); Grimoire drives it through the plugin's existing interaction surface
when the viewport has focus.

---

## §4 — E04-3: entity hierarchy + selection

**Entity enumeration is an engine API, not an editor reach-in.** Grimoire sees only
`ARCANE_API`, so we add a small engine-side surface to enumerate live entities with a
display identity (entity id + name/label where a naming component exists). This lives in
Arcane (it operates on the Astra registry the engine owns) and is reusable by any
consumer/tooling, not editor-special.

- **Hierarchy panel:** lists enumerated entities (flat list in v1; true parent/child tree
  follows the transform hierarchy when a scene with hierarchy is loaded). Clicking an entry
  sets the selection.
- **SelectionContext:** a single editor-owned struct holding the currently selected entity
  (and, for pick-cycling, the sorted candidate list under the last click). Hierarchy,
  Inspector, and viewport-pick all read/write this one object — so a viewport click
  highlights the hierarchy row and vice-versa. One source of truth.

Selection of "no entity" (empty click, deleted entity) is a valid state the inspector and
hierarchy handle gracefully (empty inspector, no highlight).

---

## §5 — E04-3: reflection-driven inspector

The inspector edits the **live registry** using the **same reflection the serializer uses**
(`Serialization/ReflectionJson.hpp` field-visitor surface). For the selected entity it
enumerates present components and, per component, visits fields:

- **Supported field types get in-place editors** — scalars (int/float), vectors
  (`vec2`/`vec3`), bool (checkbox), enum (combo), string (text). Edits write straight back
  to the component in the live registry and are immediately visible in the running/paused sim.
- **Unsupported field types render read-only** (shown, greyed, never edited) — the epic's
  explicit "not crashing" criterion. A field type the visitor doesn't recognize is displayed
  by its serialized form, disabled.

Because the inspector is reflection-driven (not hand-written per component), any component
that participates in reflection gains inspector coverage automatically — no per-type UI
code, matching the "new systems get editor coverage for free" intent.

**Edit-during-mode:** in Edit mode, inspector edits *are* authored state (persist). In Play
mode, inspector edits mutate the running sim but are discarded on Stop (they're inside the
snapshot window — §6). This falls out of the snapshot model; no special-casing.

---

## §6 — Viewport click-pick

**Decision (researched against Unity/Godot/Unreal/Bevy/Hazel):** engines whose scenes are
dominated by ECS-authored visual entities (Bevy, Godot-2D) use **CPU collect-and-sort**;
engines needing pixel-perfect picks against arbitrary geometry (Unity/Unreal/Hazel) pay for
a **GPU id-buffer**. Arcane's data model — every sprite carries `WorldTransform.matrix` +
`SpriteRenderer{size, sortingLayer, orderInLayer}` — puts v1 squarely in the first camp.

**v1 — CPU OBB test, `ARCANE_API` pick function** (lives in Arcane; operates on engine
components; reusable + headless-testable):
1. Unproject the click to a world point via the camera (§3).
2. Iterate the registry view over `(WorldTransform, SpriteRenderer)`; inverse-transform the
   world point into each sprite's local space; test against its `[-size/2, +size/2]` OBB.
3. **Collect all hits**, sort by `(sortingLayer, orderInLayer)` descending.
4. Return the sorted candidate list. The top hit becomes the selection; the list is stored
   in `SelectionContext` so **alt-click cycles** to the next entity under the cursor
   (Godot-style stack disambiguation — how the CPU-collect engines resolve "which stacked
   entity did I click").

It is a sub-millisecond linear scan, deterministic, no GPU readback, unit-testable as a
pure function headlessly (OBB hit, z-sort order, alt-cycle, empty-space miss).

**Upgrade path (explicitly deferred — do NOT build in v1):** add an entity-id MRT
(`R32_UINT`) to the existing canvas pass and a 1×1 on-demand readback under the cursor —
pixel-accurate, picks anything rendered, **additive** to the CPU path (CPU stays for
headless/no-GPU tooling). Build when AABB precision proves insufficient (alpha-cutout art,
dense same-bounds overlaps).

The physics point-query (`Interaction.cpp::PickBodyAt`, `PhysicsWorld::OverlapShape`) stays
what it is — a Sandbox gameplay affordance for drag/throw — **not** Grimoire's selection
contract, since most scene content has no physics body.

---

## §7 — E04-4: play-in-editor

Two modes over one RunLoop, using the existing registry snapshot/restore path (the same
`RegistrySnapshot` / `Runtime` snapshot the hot-reload sequence uses):

- **Edit mode (default):** RunLoop paused (§2 toolbar). Inspector/authoring edits mutate
  the live registry and **are the authored state** — they persist.
- **Play:** snapshot the whole registry, then unpause the RunLoop. The sim advances;
  render interpolation (04.2) keeps it smooth. Inspector still edits (into the running sim).
- **Stop:** restore the snapshot **exactly** and re-pause. **All play-time mutation is
  discarded** — the registry returns byte-for-byte to the authored state (standard
  Unity/Godot play-mode semantics). Resources remain intact (snapshot covers the registry;
  engine resources are not torn down).

`EditorMode` (Edit | Play) is editor-owned state; the Play/Stop toolbar buttons drive the
snapshot + RunLoop pause transition. This is the seam the LevelEditor (E15) builds on.

**Acceptance:** load scene → Play (sim runs) → Stop → registry byte-restored to the
snapshot, resources intact, no data loss — demonstrated by a headless scripted test
(snapshot → advance N ticks → mutate → Stop → assert byte-equal to snapshot).

---

## Testing strategy

**Headless CPU (`~[gpu]`, the bulk):**
- Pick pure-function: OBB hit/miss, z-sort ordering by `(sortingLayer, orderInLayer)`,
  alt-cycle advance, empty-space returns empty.
- Entity enumeration: registry with N entities enumerates N with correct ids/labels.
- Play/Stop snapshot round-trip: snapshot → advance → mutate → restore → byte-equal.
- Viewport input-gating predicate: hovered/focused truth table.
- Plugin-host lift regression: `[loom]` + hot-reload suite green **unchanged** (byte-identical lift).

**GPU-tagged (`[gpu]`):**
- OffscreenCanvas → `ImGui::Image` path renders without error.
- Scripted `Grimoire --frames N` headless boot (mirrors `Loom --frames`), asserting
  `Arcane::RenderErrorCount() == 0` (latches nvrhi + VK validation).

**Desk-verify (user; headless cannot judge — GPU-driver-crash hazard under Parsec means
interactive runs are desk-only):** docking feel, viewport pan/zoom + click-pick feel,
Play/Stop visual round-trip, inspector live-edit visibility.

---

## Scope boundaries (YAGNI — explicitly OUT of v1)

These belong to E15 (LevelEditor) or later foundational work, **not** this epic:

- Transform **gizmos** / drag-handles / in-viewport entity manipulation beyond selection.
- **Multi-select**.
- On-disk **scene-file save/load format** (Play/Stop uses in-memory snapshots, not disk).
- **Asset browser** / content pipeline UI.
- **Undo/redo** (noted as the natural next foundational add *after* this epic).
- **GPU id-buffer** picking (the §6 upgrade path).
- Entity **create/delete** UI; **prefab** system.
- **Problems tab** + enhanced console (the §2 future enhancement).

Grimoire v1 = **shell + inspector + click-pick + play-in-editor**. Nothing past the epic's
Definition of Done.

---

## Sequencing (derives the implementation plan)

1. **Lift** `PluginHost`/`Module`/`Plugin` → `Arcane/Plugin` (`ARCANE_API`); rewire Loom +
   ArcaneTests; `[loom]`/hot-reload green. (§1)
2. **Grimoire scaffold:** project + premake/solution integration; host spine
   (GpuContext/FramePerf source-shared, RunLoop, ImGuiLayer); dockspace + default layout;
   sim-time toolbar over Sandbox.dll; basic console sink. (§2)
3. **Viewport panel** via OffscreenCanvas + resize + input gating + unprojection. (§3)
4. **Entity-enumeration `ARCANE_API`** + hierarchy panel + SelectionContext. (§4)
5. **Reflection inspector** (view → edit → read-only fallback). (§5)
6. **`ARCANE_API` pick function** + viewport click-pick + alt-cycle. (§6)
7. **Play-in-editor** (EditorMode + snapshot/restore) + scripted round-trip test. (§7)
8. **`Grimoire --frames` GPU smoke** + desk-verify pass.

---

## Open decisions carried from the Mosaic-diagnostics arc (not blocking Grimoire)

The in-editor console (§2) consumes `Arcane::Log::Engine()`. Two diagnostics-arc items are
relevant but independent: the Runtime-ctor install-window fix and the Mosaic runtime-level
sync knob (both parked for the user's Phase-C decision). Grimoire's console works regardless;
if the level knob lands, the console gains DEBUG/TRACE visibility. No dependency either way.
