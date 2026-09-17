# F4 — editor authoring for 3D: +Y up, one view transform, the editor camera, grids, picking, the 3D gizmo

**Date:** 2026-09-17
**Status:** Design, approved in brainstorm 2026-09-17. Step 2 of the binding
order (`docs/research/2026-09-16-direction-and-sequencing.md`: F4 → F3 → F5);
implementation plans follow this spec (§11). Two plans; F3 may start after
plan 1.
**Charter:** the user's 2026-08-23 scope (a SEPARATE editor camera, the DCC
model; viewport controls to flip 2D ↔ 3D perspective; a 3D grid to ground
perspective plus a good 2D grid; `.arcmesh` creation on the normal
asset-creation flow) plus the pivot's F4 line (3D gizmo, camera orbit, 3D
picking — `docs/research/2026-08-21-3d-foundations-assessment.md` §F4).
**Prior law this builds on:** F1 (`docs/plans/2026-08-21-f1-transform-spine.md`:
`Transform` = vec3 + quat + vec3, `WorldTransform` = mat4); F2a
(`docs/specs/2026-08-22-f2a-scene-3d-vocabulary-design.md`: `Camera::projection`,
`PerspectiveCameraView`, the right-handed / [0,1] forward-Z conventions pinned
in `SceneCamera.hpp`); F2c (`docs/specs/2026-09-10-f2c-mesh-import-design.md`:
`.arcmesh`, `MeshDocument`, `MeshRenderer`); the 2D gizmo
(`docs/specs/2026-07-20-arcane-edit-gizmo-design.md`); id-buffer picking
(`docs/specs/2026-07-19-arcane-entity-id-picking-design.md`); the physics-2D
wiring (`docs/specs/2026-09-11-physics-2d-wiring-design.md`, whose "+Y is
down" ruling §2 supersedes).
**Standing rules obeyed:** every design states what Source 2 / Deadlock does
and why we match or diverge (§10); editor UX defers to Unreal (§4, §6, §10).

---

## 0. In one paragraph

Arcane commits to **one right-handed world with +Y up**, and the 2D world flips
to match now, while it is cheap. Every consumer of the camera — sprites, the
mesh pass, picking, the gizmo, the physics debug draw, the camera rect — takes
**one `ViewTransform`** (view, projection, viewport); today's affine
`screen = world * zoom + offset` is its orthographic case. The editor viewport
owns a camera that is never a scene entity, with **two persisted transforms**
(a 2D orthographic one and a 3D orbit one) selected by a `2D | Persp` toggle,
Unreal's navigation, and a grid per mode: a line grid with decade LOD in 2D
and a depth-tested analytic ground grid in perspective. Plan 2 makes meshes
pickable through the existing id buffer and lifts the gizmo to 3D TRS. Mesh
creation was already on the normal flow; it gains per-primitive entries and a
scene-level "Add 3D Object".

## 1. What the surveys found (2026-09-17), and what the four items reduce to

| Scope item | State today | Gap |
|---|---|---|
| Separate editor camera | **Exists, 2D only**: `EditorCamera { glm::vec2 offset; float zoom; }` (`ArcaneEditor/src/Viewport/EditorCamera.hpp`), driven in Edit; the scene camera wins in Play. The editor's mesh pass ignores it and renders through the **scene's** perspective `Camera` entity (`EditorAppFrame.cpp` `ArmGraphViewportFrame`), so with no active perspective camera in the scene **meshes do not draw in the editor at all** | a 3D pose, orbit/fly/pan, and a seam that pushes an editor view into the mesh pass |
| 2D ↔ 3D toggle | nothing; the two camera types (`SceneCameraView` vec2 affine, `PerspectiveCameraView` mat4 pair) are disjoint by design | view mode + the unification below |
| Grids | none in the viewport; **no line-list pipeline, no wireframe fill mode anywhere**; the node-canvas grid (`ArcaneEditor/src/Widgets/GraphGridPhase.hpp`) is the in-repo quality bar | both grids, new |
| `.arcmesh` on the create flow | **done**: `Create > Mesh...` → the one `CreateAssetRequest` dialog → `MintMeshAsset` (Cube default) → `MeshDocument` with five primitives (`MeshSource { Plane, Cube, UvSphere, Cylinder, Capsule, Imported }`) | per-primitive entries; a scene-level "add 3D object" |

Two findings changed the arc's shape:

1. **The Y contradiction.** The 2D world is +Y down (sprite shader `1.0 - y`
   flip, `PhysicsConfig::gravity{0, 9.81}`, the `Transform::position` tooltip).
   The 3D path is right-handed, +Y up, camera forward −Z (`SceneCamera.hpp`,
   `PerspectiveCameraTest`). These are the *same* right-handed world viewed from
   opposite sides (a 180° turn about X), so there is no handedness bug — but an
   entity at `y = +1` is below the origin in the 2D view and above it in 3D, and
   gravity points up on screen in perspective. One `Transform`, one world, one
   convention: §2.
2. **Sprites in a 3D view.** The sprite path takes canvas pixels with no
   matrix and `clip.z = 0` (`data/shaders/sprite.hlsl`). A 3D view through the
   editor camera would show sprites stuck to the screen — the mirror of the
   mesh bug that scoped F4 on 2026-08-23. An honest toggle needs one view
   transform for everything: §3.

Also verified, not assumed: **3D picking is not free.** The pick pass is a GPU
id buffer, but `CollectPickables` is a 2D quad emitter that never collects
`MeshRenderer` (`ArcaneClient/src/Arcane/Render/PickEmit.hpp`). §7.

## 2. The +Y flip (user ruling 2026-09-17)

**One right-handed world, +Y up, camera forward −Z, [0,1] forward-Z depth** —
what the perspective path already pins. **The 2D plane is XY viewed down −Z.**
Unity's model; glTF's; every DCC's.

What changes, and it is small today because there is no Aphelyon content:

- `PhysicsConfig::gravity` default → `{0, -9.81}` (project manifest) and the
  `PhysicsSettings` component default likewise. Manifold2D is
  convention-agnostic; its tests set gravity explicitly and keep their values
  (the solver does not care which way is down).
- The sprite path stops flipping in the shader: the orthographic projection
  produces NDC directly (§3). `Transform::position`'s tooltip is rewritten.
- `ReferenceProject` scenes and the ArcaneTests fixture scenes re-save with
  negated Y. The scene JSON version bumps (5 → 6); the loader migrates a ≤ 5
  file by negating Y on load (re-saved as 6 on the next save), so no file is
  ever silently mirrored and no file is refused for being old.
- Every viewport golden and thumbnail moves once (§9).

**Superseded:** the physics-2D wiring spec's "+Y is down: the world is
screen-space" (its §"Project"). That ruling was a canvas convenience taken when
the world was 2D-only. The direction record's premise — Arcane must not
foundationally change under Aphelyon — is exactly why this flips *before*
Aphelyon feature work, not after.

**Rejected:** keeping +Y down and pointing the 3D camera down +Z with up = −Y
(every 3D author, importer and the Deadlock-target work fights it forever);
per-view conventions (Godot's two worlds — with ONE `Transform` it mirrors the
same entity between views).

## 3. One view transform

```cpp
// ArcaneCore/src/Arcane/Scene/ViewTransform.hpp
struct ViewTransform
{
    glm::mat4  view;         // world -> view (RH, forward -Z, +Y up)
    glm::mat4  projection;   // view -> clip, [0,1] forward-Z (perspectiveRH_ZO / orthoRH_ZO)
    glm::uvec2 viewport;     // pixels
    glm::vec3  WorldToScreen(glm::vec3 world) const;   // .xy = pixels (y down on screen), .z = ndc depth; w-divide inside
    Ray        ScreenToRay(glm::vec2 pixel) const;      // perspective: origin = eye; ortho: per-pixel origin on the NEAR plane, constant direction (UE unprojects at NDC z = 0.5 under reversed-Z; with forward-Z [0,1] the near plane is the safe origin)
    bool       IsOrthographic() const;
};
```

- `PerspectiveCameraView` becomes it. `ActiveSceneCamera` (the orthographic
  scene sweep) produces one too: `glm::orthoRH_ZO(-w, w, -h, h, near, far)` from
  `Camera::orthographicSize` (half-height) and the viewport aspect, eye at the
  camera entity's world position looking down −Z. Its `WorldToScreen` equals
  today's `world * zoom + offset` for the same centre and half-height, modulo
  the Y flip — pinned by a test so the 2D path stays byte-stable in X.
- **`ClientRuntime::SetCamera(glm::vec2 offset, float zoom)` becomes
  `SetView(const ViewTransform&)`** (`ArcaneClient/src/Arcane/Client/ClientRuntime.hpp:97`).
  The runtime host, the plugin seam and the editor push one type.
  `Runtime::CameraOffset()/CameraZoom()` retire in favour of `Runtime::View()`.
  Plugin ABI bumps (cheap).
- `MeshNode` takes the same object instead of its own (view, projection) pair.
- **The batcher gains two spaces.**
  - **World space**: sprites carry a `glm::vec3` world position per vertex
    and `sprite.hlsl` multiplies by a view-projection constant. Tilted or
    distant sprites interpolate UVs with correct perspective. *Rejected:*
    projecting sprite corners on the CPU into pixels — keeps the vertex format
    but interpolates UVs affinely, visibly wrong the moment a sprite is not
    screen-parallel.
  - **Overlay space**: `Line / Rect / Circle / Triangle` stay in screen pixels;
    callers project through `ViewTransform::WorldToScreen` first. The gizmo,
    the camera rect, the physics debug draw and the 2D grid keep their
    primitives with one call-site change each: they take a `ViewTransform`
    instead of the affine pair. `PickView` and `GizmoView` are replaced by it.
- **Unchanged, deliberately:** sprites still write no depth and the mesh pass
  still draws over the 2D batch (`MeshNode.hpp` "the canvas's second writer").
  That is F5's compositing contract; F4 only makes both passes agree on the
  camera.

## 4. The editor camera

Unreal's model — **two persisted transforms selected by view mode**
(`FEditorViewportClient::GetViewTransform()` returns
`ViewTransformPerspective` or `ViewTransformOrthographic`), so switching
restores the other mode's framing instead of reprojecting one camera.

```cpp
// ArcaneEditor/src/Viewport/EditorCamera.hpp
enum class ViewMode : std::uint8_t { TwoD = 0, Perspective = 1 };   // persisted as int; append only
struct Ortho2D { glm::vec2 center{0}; float halfHeight = 5.0f; };     // today's state, in world units (was offset/zoom)
struct Orbit3D { glm::vec3 pivot{0}; float yawDeg = -30, pitchDeg = 30, distance = 10, fovYDeg = 60; };
struct EditorCamera
{
    ViewMode mode = ViewMode::TwoD;
    Ortho2D  ortho;  Orbit3D orbit;
    ViewTransform Resolve(glm::uvec2 viewport) const;     // pure; the ONE producer the host pushes
    void Frame(const Aabb3&);                              // mode-aware (below)
};
```

**2D mode** = the XY-plane orthographic view down −Z, near/far symmetric about
z = 0 (±1000 m) so 2D content at any authored Z is visible (UE auto-calculates
ortho planes from a depth pass; ours are fixed because sprites write no depth
until F5, which inherits the question). Navigation is today's: right-drag pans
(starts in the viewport, keeps tracking outside), wheel zooms multiplicatively
about the cursor (`kWheelStep = 1.12`; UE: `bCenterZoomAroundCursor`),
`halfHeight` clamped to [0.01 m, 10⁶ m] (UE's `MIN_/MAX_ORTHOZOOM`), `F`
frames the selection, `Home` frames the scene. `FramingBounds` becomes a 3D
AABB projected onto XY.

**Perspective mode** follows Unreal's input model (the UX directive):

| Input | Action | Unreal reference |
|---|---|---|
| right-drag | mouselook (yaw/pitch at 0.2°/px, UE's `MouseSensitivty` default), WASD fly, Q/E down/up; Shift ×2 boost is OURS (Unity's) — UE has no boost and disables its flight keys while Shift is held | `IsFlightCameraInputModeActive`, `ConvertMovementToDragRot` |
| wheel while right-drag | camera speed ×1.1 / ÷1.1 per notch, clamped to a min/max (UE 5.8's continuous `OnChangeCameraSpeed`; the old 8-step ladder is deprecated there) | `OnChangeCameraSpeed`, `FEditorViewportCameraSpeedSettings` |
| Alt + left-drag | orbit the stored pivot | `ShouldOrbitCamera` (Alt, no Ctrl/Shift, perspective-only) |
| middle-drag | pan in the view plane (moves the pivot with the eye) | `MoveViewportCamera` camera-relative branch |
| wheel | dolly along the view vector (never toward the cursor — UE centres zoom on the cursor only in ortho) | `OnDollyPerspectiveCamera` |
| `F` | frame selection: pivot = bounds centre; `radius` = the AABB's half-diagonal; `distance = radius / tan(fovY/2)`, radius widened by the aspect when it exceeds 1 | `FocusViewportOnBox` |
| `Home` | frame scene | — |

**Fly and pan speed scale with the distance to the pivot** — UE's
`bUseDistanceScaledCameraSpeed` shape, `min(distance / 10 m, 1000)`, which UE
ships OFF by default and without a floor; ours is ON by default with a 0.1
floor so a camera parked on its pivot still moves. That is what makes "F,
then fly" feel right at every zoom. A per-user **speed scalar** multiplies on
top (UE's `CameraSpeedScalar`). Wheel dolly is multiplicative about the pivot
(Unity's; UE's is additive along the view vector and can cross the pivot), and
middle-drag pan is grab-style (the world follows the cursor, matching our 2D
pan; UE's default is the opposite sign). Pitch
clamps to ±90° minus an epsilon (UE's pitch lock). Fly moves the pivot along
with the eye so a later orbit turns about what you are looking at (Unreal's
`LookAt` semantics; a dolly does not move it). **Rejected:** UE's left-drag
"move forward and yaw" — the left button is selection here; and UE's
"orbit around selection" option — the pivot is always the stored one that
`F` sets (Unity's model), with the option a later view setting if wanted.

**Persistence:** an `ImGuiSettingsHandler` block `[EditorViewport][Camera]`
per project, the `[EditorPlayMode][State]` pattern — `Mode`, both transforms,
the grid toggles and plane, fovY, camera speed. Never scene data, never the
docking layout. The headless verify layout (`Saved/verify-layout.ini`) seeds
the default, so a developer's last camera never leaks into a gate run; a
`--view-mode {2d|perspective}` editor flag seeds the mode for witness runs.

**Play mode:** unchanged. The scene camera wins in Play (`RefreshSceneResolution`),
the editor camera in Edit; the toggle, grids and the camera rect are Edit-mode
affordances gated exactly like the gizmo (`!InPlayMode()`).

## 5. Grids

### 5.1 The 2D grid (2D mode)

World XY-plane lines drawn as overlay lines through the view transform at the
**bottom batcher layer** (`SetLayer(0, 0)`), so sprites paint over it. The LOD
math is ported from `GraphGridPhase.hpp` as pure functions:

- **decade levels on the metre**: 0.1, 1, 10, 100 m …; a **major** line every
  ten minors;
- a level becomes visible when its screen spacing passes **8 px** and its alpha
  ramps to full by **24 px**, so levels crossfade instead of popping. (UE's
  `DrawGridSection` picks the level by a log of spacing against screen density
  exactly like this, but toggles the minor lines **binary** at the half-way
  point of the fraction — no crossfade; the ramp is our improvement.)
- the **major-line period is fixed at ten minors** (UE makes it a user
  setting, `GetGridInterval`); a setting if anyone asks;
- the visible line range comes from the viewport's world-space extent (the
  orthographic inverse), never a fixed count;
- the **axes** draw last at full colour: X red, Y green.

The grid unit is the metre (MKS) and does **not** follow the gizmo's
`GizmoSnap::translate`; the two are independent settings (Hammer's shape,
not Unreal's, where the grid *is* the snap grid — see §10).

### 5.2 The 3D grid (perspective mode)

An **analytic shader grid on a large ground quad**: a new graph node,
`GridNode`, declared after `MeshNode`, **depth-tested against the mesh pass's
depth without writing it** (`CompareOp::LESS_OR_EQUAL`, `depth.write = false`),
alpha-blended into the canvas. A cube occludes the grid and sits on it —
"grounding", the stated purpose.

- Minor lines every 1 m, major every 10 m, computed from screen-space
  derivatives (`fwidth`) for anti-aliasing; fades with distance and with
  grazing angle; the quad is centred under the camera and wraps its UV origin
  to a 10 m multiple to keep precision at large coordinates (Unreal's
  `FmodFloor` trick).
- **Plane is a view setting**: XZ by default (Unity's ground under +Y up),
  XY available because the first product's world is that plane. Axis lines:
  X red, Z blue (XZ) or X red, Y green (XY).
- **`MeshNode` exposes its depth attachment as a graph resource** so
  `GridNode` can read it. A small step toward F5's shared depth; the pass
  order and the clear seam are untouched.
- **The grid is never pickable**: `GridNode` draws only into the canvas and
  contributes nothing to the pick pass (UE clears the hit proxy before drawing
  its grid for the same reason).

*Rejected:* a line-list pipeline (none exists; aliases; cannot fade) and
`MeshBuilder` line geometry (same aliasing, and a mesh-sized draw for a
screen-space effect).

## 6. Toggle and view settings

The viewport tool overlay (`EditorPanels.cpp` `Select/Move/Rotate/Scale +
Local/World`) gains, at its left, a segmented **`2D | Persp`** control —
Unity's affordance — with Unreal's keys: **Alt+G** perspective, **Alt+J** the
XY orthographic view. (Alt+J is Unreal's **Top**, `LVT_OrthoXY`; UE is Z-up,
so its "Front" (Alt+H) is the YZ plane and would be the wrong analogue for
Arcane's 2D plane.) A **view-settings** dropdown beside it:
show grid, grid plane (XZ / XY), field of view, camera speed. All persisted
per §4. Hidden in Play.

## 7. Plan 2 — picking and the 3D gizmo

### 7.1 Mesh picking through the id buffer

- `CollectPickables` gains a **mesh drawable** `{ glm::mat4 world; MeshHandle mesh; uint32_t id; }`
  beside the sprite and collider kinds. Sprite pick quads move to the same
  world-space vertex path as the batcher (§3) so they project correctly in
  perspective.
- `PickNode` renders mesh drawables with the `ViewTransform` into the existing
  `R32_UINT` id target using a **depth transient it mints** (`LESS`, write),
  **after** the 2D drawables (no depth), so meshes resolve among themselves by
  depth and over sprites — the main pass's order, reproduced. The 2× supersample,
  the outline chain and the deferred readback (`DeferredPick`) are unchanged.
- Hover and click semantics are unchanged; `MeshRenderer` entities become
  selectable and outlined.

### 7.2 The 3D gizmo

`ArcaneClient/src/Arcane/Edit/Gizmo.{hpp,cpp}` stays `ARCANE_API`, stateless,
editor-free, pure.

- `GizmoTransform { glm::vec3 position; glm::quat rotation; glm::vec3 scale; }`;
  `GizmoView` → `ViewTransform`. `DecomposeTRS` / `ComposeTRS` become full 3D
  (polar-decomposition-free: scale from column lengths, rotation from the
  normalised basis, as `ActivePerspectiveSceneCamera` already does). The
  `IsPlanarBasis` precondition and the `RotationZ` / `RotationAboutZ` bridges
  **retire** for the gizmo (physics and sprite submission keep theirs until F5).
- **Handles.** Translate: three axis arrows, three plane squares, a centre.
  Rotate: three axis rings plus a screen-space ring. Scale: three axis boxes
  plus a uniform centre. Global / Local unchanged (scale always local).
- **Screen-constant size**: Unreal's formula — scale by the projected
  `w` of the gizmo origin (distance-proportional in perspective, collapses to
  the orthographic zoom in 2D), plus a user gizmo-size setting added on top
  (UE's `TransformGizmoSize`, Alt+[ / Alt+]) — the setting lands with the
  view-settings dropdown, the shortcuts are deferred (§12).
- **Hit testing stays in screen pixels** on the projected handle geometry
  (projection-independent, as today). **Dragging uses rays** from
  `ScreenToRay`: axis translate = closest point between the mouse ray and the
  axis line; plane translate and rotate = ray–plane intersection (rotation
  angle by `atan2` in the ring's plane); scale = screen delta along the
  projected axis. Perspective and orthographic share the code because the ray
  constructor differs, not the drag math (Unreal's `FViewportCursorLocation`
  split).
- **Draw**: overlay primitives after projection, top layer, no depth
  (ImGuizmo's posture). In **2D mode the Z handles hide and Z components are
  untouched**, so today's behaviour is the planar special case.
- Undo (one `CommandStack` step per drag), the off-viewport drag continuation,
  and `GizmoSnap { 0.5 m, 15°, 0.1 }` are unchanged.

## 8. Mesh creation polish

- **`Create > Mesh >` becomes a submenu** of the five primitives; each entry
  raises the one `CreateAssetRequest` with its `MeshSource` preset, so the
  dialog and `MintMeshAsset` stay the single entry (the `CreateAssetDialog.hpp`
  invariant: no creation path may bypass the request).
- **Scene-level `Add > 3D Object > Cube / Plane / Sphere / Cylinder / Capsule`**
  (Outliner context menu and the Scene menu): spawns an entity with a
  `Transform` at the view's focus point (2D: the centre at z = 0; perspective:
  the pivot) and a `MeshRenderer` bound to `Content/Meshes/<Primitive>.arcmesh`,
  **minted on first use and reused by name after** — the
  `MintOrReuseSpriteForTexture` precedent. Undoable as one command; selected
  and framed after spawn.

## 9. Testing and verification

- **Core units, headless (ArcaneTests):** `ViewTransform` round trips
  (`WorldToScreen` ∘ `ScreenToRay` hits the point) in both projections; the
  orthographic case equals today's affine mapping in X for the same centre and
  half-height; a +Y point projects **above** centre in both projections (the
  convention test); `EditorCamera::Resolve` for both modes; framing distance
  from radius and fov; grid LOD as pure functions (`GraphGridPhaseTest`'s
  shape); gizmo drag math under rapidcheck — an axis drag moves only along
  that axis; orthographic and perspective agree for planar cases; decompose ∘
  compose is the identity for any TRS.
- **Goldens move once.** The flip and the grids change every viewport golden
  and thumbnail (`docs/research/2026-09-15-thumbnail-golden-lane-research.md`
  already flags F4). One re-bless: bless the STAGED slot, then copy to source
  immediately; sweep Source + Content + Verify before blaming a diff on a race.
  The verify layout seeds the default camera.
- **Witness:** a headless editor run with `--view-mode perspective` on
  ReferenceProject renders a cube on the XZ grid (new golden); the runtime
  host's golden changes only by the flip. The `--report` census gains
  `viewMode`.
- **Desk pass** (the items NRI Phase 4's Task 10 could not perform, now
  performable): orbit a cube; toggle 2D ↔ Persp and back with both framings
  kept; `F` frames a mesh in both modes; the cube occludes the grid; click-
  select a mesh in perspective; move it along X with the gizmo; Ctrl+Z reverts
  the drag in one step; sprites in perspective stay in the world, not on the
  screen.
- **Baselines** re-booked at each plan's close (`~[gpu]`), as every arc does.

## 10. Comparison (the standing rule)

**Unreal 5.8.2 (UX reference; every claim below verified against
`D:\dev\_reference\UnrealEngine-5.8.2-release` on 2026-09-17), matched:** the
editor camera is never a scene actor; two persisted transforms by projection
(`GetViewTransform()`); hit-proxy picking with per-axis widget proxies; `F`
focus sets the orbit pivot and solves `distance = radius / tan(fov/2)`;
right-drag fly with WASD, a continuous wheel-adjusted speed with a user
scalar, and distance-scaled speed; Alt+left orbit (perspective-only); a
material grid in perspective (UE's default is texture-based; its analytic
mode is `r.Editor.NewLevelGrid 1`) and a line grid in orthographic whose level
is a log of spacing against screen density, decimating ×10 for non-power-of-
two grid sizes; screen-constant widget size by projected `w` plus a size
setting; ray construction forked by projection type; Alt+G perspective, Alt+J
the XY (Top) view; per-map persistence of the last view (`UWorld::EditorViews`).

**Source 2 (Hammer), matched:** a separate authoring camera; WASD fly; a
metric grid with power-of-ten levels independent of the transform tool's snap.

**Diverged, with reasons:**

| Reference | Arcane | Why |
|---|---|---|
| Hammer's four panes / Unreal's per-viewport type menu | one viewport with a `2D \| Persp` toggle | the first product is 2D; Unity's affordance is what the user asked for |
| six axis-aligned orthographic views | two modes (XY ortho = UE's Top, perspective) | Front/Side views are triggered by 3D level authoring, not by a 2D game |
| Unreal's grid *is* the snap grid (`GEditor->GetGridSize()` feeds both), major period a user setting | grid on the metre, snap independent, major every ten | units are metres (MKS); Hammer's shape; one fewer setting until the two need to agree |
| Unreal's ortho minor lines toggle binary at the level's half-way fraction | alpha crossfade between 8 and 24 px | popping is the thing the "good 2D grid" ask is about |
| Unreal's perspective grid is texture-based by default, its ortho grid is line-drawn | analytic derivative-AA grid in perspective; primitive lines in ortho | no line pipeline exists; an analytic quad is one shader and fades cleanly |
| Unreal's "orbit around selection" option; left-drag forward+yaw | stored pivot set by `F`; left button = selection | Unity's model; fewer modes |
| Unreal auto-calculates ortho near/far from a depth pass | fixed ±1000 m | sprites write no depth until F5 |
| Unreal reverse-Z (both projections) | forward-Z [0,1] | pinned by F2a; reverse-Z is a renderer-tier decision (T1) |

## 11. Plans

1. **Plan 1 — the foundation F3 needs.** `ViewTransform`; the +Y flip (gravity
   defaults, sprite shader, scene migration + JSON version bump, tooltips,
   docs); `SetView` replaces `SetCamera` (ABI bump); the batcher's world-space
   sprite vertex; overlay callers take a `ViewTransform` (gizmo 2D as-is,
   camera rect, physics debug); `MeshNode` consumes `ViewTransform` and exposes
   depth; `EditorCamera` with both modes, navigation, persistence, `--view-mode`;
   the 2D grid; `GridNode`; the toggle and view settings; mesh-create polish
   (§8); tests, goldens re-blessed, witness; the desk pass minus picking/gizmo.
2. **Plan 2 — the authoring loop.** Mesh pickables + the pick pass's depth
   transient; the 3D gizmo (transform, handles, rays, draw, 2D special case);
   tests; goldens; the full desk pass; retire the two "F4 owns this" comments
   (`Gizmo.hpp` "THE GIZMO STAYS 2D", `EditorCamera.cpp` framing stays planar)
   and reword NRI Phase 4 Task 10's desk items.

## 12. Non-goals and seams

| Out | Owner / trigger |
|---|---|
| Per-mesh bounds, frustum culling, draw sorting | **F3** — receives the frustum from `ViewTransform` and the AABB primitive from `ComputeMeshBounds` |
| Shared depth, the declarative clear-op, sprite depth, 2D-over-3D ordering | **F5** — receives `MeshNode`'s exposed depth resource; F4 leaves the pass order |
| Piloting / looking through a scene `Camera` (Unreal's actor lock) | cinematic or gameplay-camera authoring |
| Top / Bottom / Side orthographic views | 3D level authoring |
| Multiple viewports, view-mode buffers (wireframe, depth, ids) | later editor arcs |
| The glTF Import-Mesh dialog | parked by F2c; unchanged |
| Snap-to-grid affordances beyond the existing Ctrl snap; grid-size shortcuts (`[` `]` in UE); gizmo-size shortcuts (Alt+`[` `]`) | when the grid and the snap first need to agree; the settings exist in the dropdown from plan 1 |
| Ortho near/far auto-calculation from scene depth | F5, once sprites carry depth |
| A realtime / on-demand redraw toggle for the viewport | not an F4 concern; the editor's redraw policy is its own item |
| "Orbit around selection" as an option | a view setting if asked; the stored pivot is the default |
| Camera-relative sprite billboarding | F5 (the assessment's "billboarding as a per-sprite flag") |

## 13. Rulings ledger (2026-09-17)

| # | Ruling | Rejected |
|---|---|---|
| R1 | One right-handed world, **+Y up everywhere**; the 2D world flips now | keep +Y down with an inverted 3D camera; per-view conventions |
| R2 | F4 = the four items **plus 3D picking and the 3D gizmo**, as two plans | four items only, picking/gizmo as a later F4b |
| R3 | **One `ViewTransform`** for every consumer; 2D is the orthographic case; sprites carry world-space vertices | keep the affine path, 3D mode shows meshes + grid only |
| R4 | Mesh creation reduces to per-primitive Create entries + scene "Add 3D Object" | "done as-is"; the import dialog |
| R5 | Editor camera = two persisted transforms by view mode; Unreal's navigation and keys | one camera switching matrices |
| R6 | 2D grid = overlay lines with decade LOD and crossfade; 3D grid = analytic depth-tested `GridNode` after `MeshNode` | line-list pipeline; MeshBuilder geometry |
| R7 | Grid unit = metre, independent of the gizmo snap | Unreal's grid-is-snap coupling |
| R8 | Picking: meshes rasterised into the existing id buffer with a pass-local depth | CPU ray-vs-AABB picking |
| R9 | Gizmo: screen-space hit test, ray-based drags, overlay draw on top; 2D mode = Z hidden | a depth-tested 3D gizmo mesh |

## 14. Costs and risks, stated plainly

- **The flip touches every scene file and every viewport golden.** Cheap now
  (test scenes and ReferenceProject only); the plan does it first and alone,
  gate green, before any other F4 change lands.
- **`SetCamera` → `SetView` is a plugin-ABI change**; Aphelyon's module does not
  call it today (its module is three files), so the cost is the bump.
- **World-space sprite vertices** change `Batcher2D`'s vertex layout and the
  sprite pipeline; the 2D batch's painter ordering is unchanged, but the
  vertex size grows by 4 bytes.
- **`GridNode` reads `MeshNode`'s depth**, which must therefore outlive
  `MeshNode`'s record — the graph's transient lifetime rules decide whether
  that is a declaration change or a resource-ownership move; the plan settles
  it in its first review.
- **No line pipeline still**: the 2D grid rides thickness-expanded quads like
  every overlay line today. Fine at a few hundred lines; a dedicated line
  pipeline is a later arc if the physics overlay or the grid ever needs
  thousands.
- **The verify layout seeds the camera**, so goldens depend on the seed;
  changing the default pitch/yaw later is a re-bless.

---

## Appendix A — code this spec builds on (survey 2026-09-17)

| Piece | Where |
|---|---|
| `EditorCamera { vec2 offset; float zoom }`, `Pan/ZoomAt/Frame`, `FramingBounds` | `ArcaneEditor/src/Viewport/EditorCamera.{hpp,cpp}` |
| camera push (Edit) / scene camera (Play), mesh-pass arming, resize, pick arming | `ArcaneEditor/src/App/EditorAppFrame.cpp` (`UpdateEditorCamera`, `RefreshSceneResolution`, `ArmGraphViewportFrame`, `ApplyPendingViewportResize`) |
| viewport panel + tool overlay | `ArcaneEditor/src/Panels/EditorPanels.{hpp,cpp}` |
| `SceneCameraView` (affine), `PerspectiveCameraView`, `ActiveSceneCamera`, `ActivePerspectiveSceneCamera`, the RH / [0,1] conventions | `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp` |
| `Camera` component (`projection`, `orthographicSize`, `fovYDegrees`, `nearZ`, `farZ`) | `ArcaneCore/src/Arcane/Scene/Components.hpp` |
| `ClientRuntime::SetCamera(vec2, float)` | `ArcaneClient/src/Arcane/Client/ClientRuntime.hpp:97` |
| `Batcher2D` (`Line/Rect/Circle/Triangle`, `SetLayer`), sprite pipeline, `sprite.hlsl` | `ArcaneClient/src/Arcane/Render/Batcher2D.hpp`, `Nri/nodes/Batch2DNode.cpp`, `data/shaders/sprite.hlsl` |
| `MeshNode` (D32 depth it mints, LESS, declared after batch2d) | `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.{hpp,cpp}` |
| `PickEmit` (`PickView`, `PickDrawable`, `CollectPickables`), `PickNode` / `OutlineNode`, `DeferredPick` | `ArcaneClient/src/Arcane/Render/PickEmit.hpp`, `Nri/nodes/PickOutlineNodes.*`, `ArcaneEditor/src/Viewport/DeferredPick.hpp` |
| 2D gizmo (`GizmoTransform`, `GizmoView`, `IsPlanarBasis`, `HitTest/Draw/ApplyDrag`) | `ArcaneClient/src/Arcane/Edit/Gizmo.{hpp,cpp}` |
| physics debug draw (overlay lines through the affine) | `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.{hpp,cpp}` |
| node-canvas grid LOD/phase math (the quality bar) | `ArcaneEditor/src/Widgets/GraphGridPhase.hpp` |
| create flow (`CreateAssetRequest`, `CreateAssetKind`, `MintMeshAsset`, `MintOrReuseSpriteForTexture`) | `ArcaneEditor/src/Panels/CreateAssetDialog.{hpp,cpp}`, `Panels/AssetPanelCommon.cpp`, `App/EditorAppProject.cpp` |
| `MeshSource`, `MeshBuilder` primitives, `ComputeMeshBounds`, `MeshDocument`'s fixed three-quarter preview camera | `ArcaneCore/src/Arcane/Mesh/{MeshAsset,MeshBuilder}.hpp`, `ArcaneEditor/src/Documents/MeshDocument.cpp` |
| settings persistence pattern (`[EditorPlayMode][State]`), the imgui.ini veto, `--headless` verify layout, `--dump-layout` | `ArcaneEditor/src/App/EditorApp.cpp`, `EditorApp.hpp`, `ArcaneEditor/src/main.cpp` |
| gravity default `{0, 9.81}` (+Y down, superseded) | `ProjectManifest::PhysicsConfig`; `PhysicsSettings` component |

## Appendix B — Unreal references consulted

Source: `D:\dev\_reference\UnrealEngine-5.8.2-release\Engine\Source` (the
centralised reference checkout since 2026-09-17; the old in-repo `.example/`
dump is gone). Every Unreal claim in this spec was verified against it on
2026-09-17; the first draft's errors (Front vs Top, `sin` vs `tan`, wheel
dolly "toward the cursor", the ortho grid "crossfade", the 8-step speed
ladder, the ±89° clamp, "legacy perspective grid") were corrected in the same
day's follow-up commit.

`Editor/UnrealEd/Public/EditorViewportClient.h` (`FViewportCameraTransform`,
`GetViewTransform`, realtime overrides), `Editor/UnrealEd/Private/EditorViewportClient.cpp`
(`CalcSceneView`, `ShouldOrbitCamera`, `ConvertMovementToDragRot`,
`MoveViewportCamera`, `OnOrthoZoom`, `OnDollyPerspectiveCamera`,
`OnChangeCameraSpeed`, `GetCameraSpeed`, `FocusViewportOnBox`,
`FViewportCursorLocation`, `GetPivotForOrbit`, distance-scaled speed, the
pitch lock), `Editor/UnrealEd/Public/Settings/EditorViewportSettings.h`
(`FEditorViewportCameraSpeedSettings`), `Editor/UnrealEd/Classes/Editor/UnrealEdTypes.h`
(`ELevelViewportType`: `LVT_OrthoXY = Top`, `LVT_OrthoFront = NegativeYZ`),
`Editor/UnrealEd/Private/EditorComponents.cpp` (`FGridWidget::DrawNewGrid`,
`DrawOldGrid`, `DrawGridSection`, `DrawOriginAxisLine`, `r.Editor.NewLevelGrid`),
`Editor/UnrealEd/Private/UnrealWidget.cpp` (screen-constant scale by projected
`w` + `TransformGizmoSize`, `HWidgetAxis`), `Editor/UnrealEd/Private/EditorViewportCommands.cpp`
(Alt+G/H/J/K, `[` `]`, Alt+`[` `]`), `Editor/UnrealEd/Private/ViewportToolbar/UnrealEdViewportToolbar.cpp`
(speed slider + scalar), `Runtime/Engine/Public/EngineDefines.h`
(`MIN_/MAX_ORTHOZOOM`), `Runtime/Engine/Classes/Engine/World.h` (`EditorViews`
per-map persistence). Shapes only; no code copied.
