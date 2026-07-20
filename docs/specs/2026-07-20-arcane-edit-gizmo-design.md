# Transform Gizmo (2D TRS) — Design

**Goal:** A screen-constant, procedurally-drawn 2D transform gizmo for the Grimoire editor viewport — translate, rotate, and scale handles over the selected entity, with a Global/Local axis toggle and hold-to-snap. The gizmo *core* (geometry, hit-testing, drag math) is an editor-free `ARCANE_API` capability; Grimoire is the thin consumer that owns selection, mode/space state, and brackets each drag into the undo `CommandStack` (one drag = one undo step).

This is the second half of SPEC #2 ("gizmos + unified undo/redo"); the undo/redo command foundation (first half, spec `2026-07-20-arcane-edit-undo-command-design.md`) is done and this builds directly on its `CommandStack`/`ScopedTransaction` transaction grouping.

## Context (what already exists)
- `Arcane::LocalTransform` (`Arcane/Arcane/src/Arcane/Scene/Components.hpp`): `glm::vec2 position`, `float rotation` (radians), `glm::vec2 scale` — full 2D TRS, reflected.
- `Grimoire::SelectionContext` (`Arcane/Grimoire/src/SelectionContext.hpp`): **single** selected entity (`selected`, `HasSelection`, `Select`, `Clear`). No multi-select yet.
- `Arcane::Batcher2D` (`Arcane/Arcane/src/Arcane/Render/Batcher2D.hpp`): lines, quads/rects, SDF circles — the primitives a gizmo needs. Linear-HDR canvas, straight alpha.
- `Arcane::CommandStack` + `ScopedTransaction` (`Arcane/Arcane/src/Arcane/Edit/`): transaction-grouped undo/redo. Frame-spanning gestures use explicit `Begin`/`SnapshotComponent`/`Commit` (same pattern the Inspector uses). A no-op transaction self-drops (after==before). The stack is built with a `std::function<Astra::Registry&()>` resolver and is Edit-mode gated in Grimoire.
- Grimoire viewport: renders the scene into a panel with a camera (offset + zoom; pixels-per-meter convention = 100) and already does click-pick via `Arcane::PickEntitiesAt` with a `view = {CameraOffset, CameraZoom}`.
- SPEC #1 polling reconcile: writing a `LocalTransform` on a physics entity makes the body follow — so gizmo drags (and their Ctrl+Z) on physics entities Just Work (Unreal PostEditUndo for free).
- T3 Inspector precedent: gesture bracketing is gated to Edit mode; input keybinds are gated on `!IsPlaying()` and not-capturing-keyboard, with member-persisted edge detection over raw scancodes.

## Architecture
The gizmo core lives in Arcane as an editor-free `ARCANE_API` capability (`Arcane/Arcane/src/Arcane/Edit/Gizmo.{hpp,cpp}`); Grimoire consumes it. Rationale: matches the strict layering directive ("editor needs become ARCANE_API surface, never editor hooks inside Arcane") and the Edit/CommandStack precedent — and because the core is pure math, it is **headlessly unit-testable**, leaving only rendering + interactive feel for desk verification.

The core is **stateless** (pure functions over value inputs). ALL interaction state (current mode, space, drag-in-progress) lives in Grimoire.

## Arcane API — `Arcane/Edit/Gizmo.{hpp,cpp}` (ARCANE_API, editor-free, stateless)

Types:
- `enum class GizmoMode { Translate, Rotate, Scale };`
- `enum class GizmoSpace { World, Local };`
- `enum class GizmoAxis { None, X, Y, Center };`  // Center = free-move (translate) / uniform (scale)
- `struct GizmoTransform { glm::vec2 position{0,0}; float rotation = 0.0f; glm::vec2 scale{1,1}; };`  // decoupled from Scene; Grimoire maps LocalTransform<->this so Edit/Gizmo has no Scene dependency.
- `struct GizmoView { glm::vec2 cameraOffset{0,0}; float zoom = 1.0f; float pixelsPerMeter = 100.0f; glm::vec2 viewportOriginPx{0,0}; glm::vec2 viewportSizePx{0,0}; };`  // world<->screen. REUSE the existing viewport/pick camera-view type if one exists (pin at read-first); else this minimal struct.
- `struct GizmoSnap { bool enabled = false; float translate = 0.5f; float rotationDeg = 15.0f; float scale = 0.1f; };`  // defaults; increments become settings later.

Functions (screen-space mouse in; all pure):
- `GizmoAxis HitTest(GizmoMode, GizmoSpace, const GizmoTransform&, const GizmoView&, glm::vec2 mouseScreen);`
  Analytic: axis = distance to the handle segment within a pixel threshold; Center = inside the center box; Rotate = within the ring's radius band. Mode-dependent handle set (see Layout). Center takes priority over axes on overlap.
- `void Draw(Batcher2D&, GizmoMode, GizmoSpace, const GizmoTransform&, const GizmoView&, GizmoAxis hovered, GizmoAxis active);`
  Builds screen-constant geometry, tinted X=red / Y=green / Center=yellow; the hovered or active handle brightened. The consumer calls this after the scene (on top).
- `GizmoTransform ApplyDrag(GizmoMode, GizmoSpace, GizmoAxis, const GizmoTransform& start, const GizmoView&, glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen, const GizmoSnap&);`
  Pure, computed from `start` (no accumulation drift). Returns the new transform.

Internal helpers: world<->screen via `GizmoView`; pixel->world sizing = `px / (zoom * pixelsPerMeter)`.

## Handle layout + rendering
Screen-constant: handle lengths/sizes are authored in pixels and converted to world via `px/(zoom·ppm)`, so they render at a fixed on-screen size at any zoom.
- **Translate:** X arrow (line + filled triangle head, +X), Y arrow (+Y), Center square (free 2D move). Axis directions = world (+X right, +Y up) in World space, or rotated by `transform.rotation` in Local space.
- **Rotate:** a single SDF ring centered on the pivot (fixed px radius). Space-agnostic (2D has one rotation axis). Only the ring is interactive.
- **Scale:** X handle (line + box end), Y handle (line + box end), Center box (uniform). Axes are **ALWAYS local** (rotated by `transform.rotation`) — scaling a rotated object along world axes is nonsensical.

Colors: X=red, Y=green, Center=yellow; hovered/active brightened; a thin dark outline for contrast on the HDR canvas. Hit priority: Center > nearest axis (within threshold) > (rotate) ring.

## Interaction — Grimoire consumer (Viewport panel)
Grimoire owns: `GizmoMode m_gizmoMode` (default Translate), `GizmoSpace m_gizmoSpace` (default World), and drag state `{ GizmoAxis active; GizmoTransform startXform; glm::vec2 mouseStartScreen; bool dragging; }`.

Per frame, only when **Edit mode (`!m_play.IsPlaying()`)** AND `m_selection.HasSelection()` AND the viewport is hovered/focused AND ImGui is not capturing the mouse:
1. Map the selected entity's `LocalTransform` -> `GizmoTransform`.
2. If not dragging: `hovered = HitTest(mode, space, xform, view, mouseScreen)`.
3. On mouse-down with `hovered != None`: start drag — record `active=hovered`, `startXform`, `mouseStartScreen`; **`m_undo->Begin("Gizmo <mode>")` + `SnapshotComponent(selected, <LocalTransform descriptor>)`**.
4. While dragging: `newXform = ApplyDrag(mode, space, active, startXform, view, mouseStartScreen, mouseCur, snap)`; write it into the live `LocalTransform` (physics body follows via SPEC #1 reconcile). `snap.enabled = Ctrl held`.
5. On mouse-up: **`m_undo->Commit()`** (one undo step; a no-move drag self-drops via after==before); clear drag state.
6. Always (has-selection, Edit mode): `Gizmo::Draw(viewportBatcher, mode, space, xform, view, hovered, active)` after the scene.

Mode/space controls:
- Keys **W** = Translate, **E** = Rotate, **R** = Scale (edge-triggered, same gating/edge-detection as the T3 keybinds). Confirm no conflict with viewport camera keys at read-first; fall back to **1/2/3** if W/E/R are taken by camera pan.
- Toolbar toggle button **Global/Local** (world default) sets `m_gizmoSpace`; no key needed in v1. Show current mode + space in the toolbar alongside the Undo/Redo buttons.

The drag reuses the exact `CommandStack` bracketing the Inspector uses, so a gizmo drag is one transaction — and multi-select (future) collapses to one undo step for free.

## Transform math (per mode)
All from `start` (captured at mouse-down); mouse positions unprojected screen->world via the view.
- **Translate:** `axisDir` = world (X=(1,0), Y=(0,1)) or local (rotated by `start.rotation`). `deltaWorld = mouseCurWorld - mouseStartWorld`. Axis: `newPos = start.position + dot(deltaWorld, axisDir) * axisDir`. Center: `newPos = start.position + deltaWorld`. Snap (Ctrl): quantize the moved component(s) to the `snap.translate` grid.
- **Rotate:** `pivot = start.position` (world). `a0 = atan2(mouseStartWorld - pivot)`, `a1 = atan2(mouseCurWorld - pivot)`. `newRotation = start.rotation + (a1 - a0)`. Snap: quantize `newRotation` to `snap.rotationDeg`.
- **Scale:** `axisDir` = local (rotated by `start.rotation`). Axis X: `d0 = dot(mouseStartWorld - pivot, axisDirX)`, `d1 = dot(mouseCurWorld - pivot, axisDirX)`; `factor = d1/d0` (guard `|d0| < eps` -> factor 1); `newScale.x = start.scale.x * factor`. Y analogous. Center (uniform): `factor = length(mouseCurWorld - pivot) / length(mouseStartWorld - pivot)` (guard denom); `newScale = start.scale * factor`. Snap: quantize scale component(s) to `snap.scale`. Clamp each component to `>= kMinScale` (e.g. 0.01) to avoid zero/negative.

## Edge cases
- Click a handle without moving -> mouse-up commits an unchanged transform -> `CommandStack` drops it (no history entry).
- Drag starting off any handle (`hovered==None`) -> no drag, no transaction; the click falls through to the normal viewport pick/deselect.
- Selection cleared or entity deleted mid-drag -> abort: `Cancel()` the open transaction (defensive), clear drag state.
- Entering Play mid-interaction -> the Edit-mode gate stops gizmo input; `CommandStack::Clear()` on Play (already wired) resets any open transaction.
- Degenerate scale pivot distance (`|d0|` / `length` ~ 0) -> factor 1 (no change) rather than div-by-zero.

## Testing
- Headless `[gizmo]` (CPU-only, no GPU, pure value tests — no Registry needed, operate on `GizmoTransform` values):
  - `HitTest` returns the correct `GizmoAxis` for cursor positions at each handle (X tip, Y tip, center box, rotate ring band) and `None` off-gizmo, per mode.
  - `ApplyDrag` translate (world + local): a known screen delta -> expected `position`; local rotates the axis.
  - `ApplyDrag` rotate: mouse angle a->b -> `rotation += (b-a)`; snap to 15deg quantizes.
  - `ApplyDrag` scale: axis distance ratio -> expected `scale`; center uniform; clamp at `kMinScale`; snap to 0.1.
  - Snap quantization exactness (translate / rotate / scale).
- Desk-verify (GPU/interactive, T4-style): draw correctness (screen-constant size across zoom, colors, hover highlight), drag feel per mode, Global/Local toggle, Ctrl-snap, Ctrl+Z reverts a gizmo drag in one step (physics body follows), W/E/R switching, Play gating, no NVRHI/validation noise.

## Non-goals (v1)
- Multi-select (deferred — needs a multi-select `SelectionContext`; the `CommandStack` transaction grouping is already ready for it).
- Non-`LocalTransform` gizmos (collider/light/camera handles).
- A configurable snap-increment UI (hardcoded defaults; settings surface later).
- Plane / combined handles beyond the center handle (2D -> the center handle is the plane).
- A gizmo in the Play/runtime view (Edit-mode only).

## Implementation-time confirmations (pin at read-first, not deferred TODOs)
- Reuse the existing viewport/pick camera-view type if one exists instead of `GizmoView` (match `PickEntitiesAt`/the viewport camera); else define the minimal `GizmoView`.
- Exact `Batcher2D` draw API (line / triangle / filled-quad / SDF-ring calls, color + thickness params) and whether a filled-triangle primitive exists for arrowheads (else compose from a thick short segment / two triangles).
- `InputSnapshot` mouse API (screen position, button down/pressed/released, wheel) and the viewport hover/focus + ImGui-capture accessors (reuse what T3 used).
- Where the viewport's world-space `Batcher2D` pass is, to hook `Gizmo::Draw` after the scene.
- Mode-key choice (W/E/R vs 1/2/3) after checking the viewport camera's key bindings for conflicts.
