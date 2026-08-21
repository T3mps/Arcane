# F1 — the transform spine

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development`
> (recommended) or `superpowers:executing-plans` to implement this task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One `Transform` that carries three dimensions, and a propagation system
that does not copy the scene twice a frame.

**Why now:** NRI Phase 4 built a 3D renderer the scene cannot describe. The
assessment behind this plan is
`docs/research/2026-08-21-3d-foundations-assessment.md` — **read it first; do not
re-derive it.** This is F1, the first and load-bearing item on its list. Nothing
else in the foundations can start until the spine is 3D.

**Architecture:** Four ordered changes, each varying exactly one dimension so
each is independently verifiable. Astra gains the one accessor that lets a
consumer cache on structure change. The inspector learns quaternions *before*
anything stores one. Then the types go 3D through machinery that already works.
Then the propagation algorithm is replaced with the types already settled.
Changing types and algorithm in the same task would make neither reviewable.

**Tech stack:** C++23, MSVC (VS 18), glm, Astra (vendored), Catch2. **No new
vendored libraries** — Box3D is a later arc and is explicitly not part of F1.

---

## Global Constraints

- **`ARCANE_SDK` must be set per-invocation.** The process value can be stale
  even when the User-scope value is correct: `export ARCANE_SDK="D:\\dev\\starworks\\Arcane"`.
- **Build `Arcane.slnx`, never a bare `.vcxproj`** — a project file defaults to
  Win32 and fails MSB8013.
- **Build all three configs.** A Dist-only break hides behind
  `#if !defined(ARCANE_DIST)`. Run Dist in its own MSBuild call.
- **All three configs at 0 warnings.**
- **Gate baseline at plan start: Debug 49192/1027 · Release 49192/1027 ·
  Dist 49131/1022.** Every task states what it does to these numbers. A task
  that moves them without saying so is a defect. **Derive counts by running,
  never by recalling.**
- Run the gate **from each config's own exe dir**:
  `bin/{Debug,Release,Dist}-windows-x86_64-md/ArcaneTests` → `ArcaneTests.exe "~[gpu]"`.
- **`[gpu]` tests do not run in an agent session** — windowed d3d12 SIGSEGVs
  this machine under Parsec. Agent runs prove they compile and link.
- **A green gate proves nothing about either host.** `ArcaneTests` compiles
  neither `EditorApp.cpp` nor `RuntimeApp.cpp`. Anything touching host frame
  order or editor panels is desk-verified or unverified.
- **If a gate run reports a failure, capture the Catch2 random-seed banner
  BEFORE re-running.** This repo has a known order-dependent flake class.
- Leave the untracked `out.txt` at the repo root alone — it is the user's.

## Decisions already made — do not re-open

| Decision | Where it came from |
|---|---|
| **One 3D `Transform`, never a parallel `Transform3D`** | User, 2026-08-21. UE/Unity/Godot all do this; a second hierarchy is the "parallel infrastructure" the project's directives forbid. |
| **Quaternion in storage, Euler at the inspector** | Avoids gimbal lock in stored data; matches `FTransform`/`Transform`/`Transform3D`. |
| **Scene files: CLEAN BREAK, no upgrade path** | User, 2026-08-21. The corpus is two authored files. |
| **Physics stays 2D, on the XY plane of the 3D transform** | The correct degenerate case. Box3D is a later arc and F1 must not collide with it. |
| **The orthographic camera path stays byte-identical** | Every existing 2D scene uses it. |
| **`SpriteRenderer` keeps rendering exactly as today** in F1 — reads `position.xy` and Z-axis rotation, ignores Z | The world-quad question is real but belongs to F5. F1 must not pre-empt it. |
| **No new vendored libraries** | Box3D lands after F1, as its own arc. |

## Out of scope, deliberately

| Item | Why |
|---|---|
| `MeshRenderer`, mesh/material assets | F2. F1 is the spine only. |
| Bounds, frustum culling, draw sorting | F3. |
| 3D gizmo, camera orbit/fly, 3D picking | F4. The gizmo must keep working in 2D through F1; making it 3D is not F1's job. |
| Declarative clear-op, unified world pass | F5. |
| Box3D / 3D physics | Its own arc, after F1. |
| Phase 4 Task 8 (bindless table) | Held pending F2 — see the assessment. |
| Making sprites world quads | F5, and it needs the open decision in the assessment. |

---

## File structure

**Created**

| File | Responsibility |
|---|---|
| `ArcaneTests/src/TransformSpineTest.cpp` | 3D TRS, quaternion composition, hierarchy composition, SLERP |
| `ArcaneTests/src/TransformOrderTest.cpp` | the flat order: topological validity, rebuild-on-structure-change, dirty skipping |

**Modified**

| File | Change |
|---|---|
| *(Astra repo)* `Registry/RelationshipGraph.hpp` | public `StructureVersion()` |
| *(Astra repo)* `Registry/Registry.hpp`, `Relations.hpp` | surface it to consumers |
| `Scene/Components.hpp` | `Transform`, `WorldTransform`, `PreviousTransform` go 3D + reflection |
| `Scene/TransformSystems.hpp` | mat4 composition, then the flat-order rewrite |
| `Scene/PhysicsSystem.hpp`, `PhysicsComponents.hpp` | write back to the XY plane |
| `Scene/RenderSystems.hpp`, `SceneCamera.hpp`, `SceneModule.hpp` | mat4 consumers; ortho path byte-identical |
| `Render/Batcher2D.hpp`, `Render/PickEmit.{cpp,hpp}`, `Render/PhysicsDebugDraw.cpp` | mat4 consumers |
| `Edit/Gizmo.{cpp,hpp}`, `Edit/EntityOps.{cpp,hpp}`, `Edit/ComponentEditCommand.hpp` | mat4 consumers; gizmo stays 2D |
| `Serialization/SceneSerializer.hpp`, `SceneAsset.hpp` | reflection-driven — expect little or no edit |
| `Base/Runtime.cpp`, `Host/ProjectBoot.{cpp,hpp}` | mat4 consumers |
| `Plugin/PluginABI.hpp` | ABI 15 → 16 + changelog line |
| `ArcaneEditor/src/Panels/InspectorFields.{cpp,hpp}` | `glm::quat` field type |
| `ArcaneEditor` App/Panels/ComponentCatalog/EditorCamera | mat4 consumers |
| `ReferenceProject/Content/scenes/main.arcscene` | re-authored |
| ~25 files under `ArcaneTests/` | mat4 consumers |

---

## Task 1: Astra exposes the structure version

The whole Astra-side change, and it is small on purpose.

**Context:** `RelationshipGraph::m_structureVersion` already exists and already
drives Astra's own traversal-cache invalidation — but it lives in the **private**
section (private begins at `RelationshipGraph.hpp:652`; the version is used at
`:662`, `:710`, `:750`, `:761`, `:791`, and incremented at `:819`). No consumer
can ask "has the hierarchy changed since I last looked?"

That single accessor is what turns `ForEachDescendant` from a **twice-per-frame**
cost into a **once-per-structure-change** cost — without altering its copy
semantics, which exist for a real reason (the caches live in a non-pointer-stable
`FlatMap`; a reference escaping the lock can dangle after a rehash — IM-5).

**This is Astra work and follows the Astra-first workflow: commit in the Astra
repo FIRST, then `scripts\sync-astra.ps1` into Arcane.** Vendored Astra must
always be current.

**Interface produced:**
```cpp
// RelationshipGraph
[[nodiscard]] std::uint32_t StructureVersion() const noexcept;   // acquire load
// surfaced through Registry / Relations so a system can reach it
```

- [ ] **Step 1: Write the failing test in the Astra repo** — the version is
  stable across component mutation and pure reads; it **increments** on
  parent/child structural change (attach, detach, reparent, entity destroy).
  Pin that reads do NOT bump it — a version that changes every frame is useless
  for caching, and that is the property this whole task exists for.
- [ ] **Step 2: Run it and confirm it fails** (`StructureVersion` undeclared).
- [ ] **Step 3: Implement** — a `[[nodiscard]]` acquire-load accessor, matching
  the memory ordering already used internally at `:761`. Surface it through
  `Registry`/`Relations` in whatever way matches those headers' existing style.
- [ ] **Step 4: Run Astra's own suite**, commit in the Astra repo.
- [ ] **Step 5: `scripts\sync-astra.ps1`**, then build Arcane's three configs.
  **Gate moves: +0/+0 in Arcane** — this task adds no Arcane test. State the
  Astra-side numbers separately.
- [ ] **Step 6: Commit (Arcane)** — `chore(astra): sync -- RelationshipGraph exposes its structure version`

---

## Task 2: The inspector learns quaternions

Additive, gate-green, and it removes a dependency from Task 3. Doing it first
means the big type change never blocks on inspector work.

**Context:** `ArcaneEditor/src/Panels/InspectorFields.cpp:39-40` already registers
`TypeID<glm::vec3>` and `TypeID<glm::vec4>` and draws them at `:216-220`. There is
**no** `glm::quat` handling. The precedent for angle presentation already exists:
`ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)` on the
current scalar `Transform::rotation`.

**The rule this field must obey:** **Euler is a VIEW, quaternion is the STORAGE.**
Round-tripping quat → Euler → quat must not drift when the user does not edit,
and editing one axis must not silently rewrite the other two. This is the classic
inspector bug and the test is what prevents it.

- [ ] **Step 1: Write the failing tests** — a quat field renders three Euler
  components; a no-op edit round-trips to the *same quaternion* (within
  tolerance) rather than a different-but-equivalent one; editing yaw alone leaves
  pitch/roll displayed unchanged; the ±180°/gimbal-adjacent cases do not explode.
  Prefer testing the pure conversion helpers rather than ImGui.
- [ ] **Step 2: Run and confirm they fail.**
- [ ] **Step 3: Implement** the `glm::quat` field beside the existing vec3/vec4
  cases, following that file's registration idiom exactly.
- [ ] **Step 4: Run the tests, then the full gate.** State the delta.
- [ ] **Step 5: Commit** — `feat(editor): the inspector can edit a quaternion as Euler angles`

---

## Task 3: `Transform` goes 3D

The type change. **Large and atomic by nature** — the tree does not compile until
every consumer is updated. ~57 files touch these types (ArcaneClient 22 ·
ArcaneEditor 9 · ArcaneRuntime 1 · ArcaneTests 25 · ReferenceProject 0).

**Interface change:**
```cpp
struct Transform
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};   // identity; w,x,y,z
    glm::vec3 scale{1.0f};
    glm::mat4 ToMatrix() const;                    // was mat3
};
struct WorldTransform   { glm::mat4 matrix{1.0f}; };          // was mat3
struct PreviousTransform{ glm::vec3 position{0.0f}; glm::quat rotation{1.0f,0,0,0}; };
```

**Keep the propagation ALGORITHM as it is.** `TransformSystems` still calls
`ForEachDescendant`; only the composition type changes (`mat3` → `mat4`). Task 4
replaces the algorithm. **Changing both here would make neither reviewable.**

**Load-bearing requirements:**
- **`PreviousTransform` interpolation becomes SLERP.** Its existing contract
  (`Components.hpp:46-51`) is explicit that it is decomposed *"so rotation
  interpolates on the shortest arc, NOT by lerping matrix components."* The 3D
  expression of that intent is `glm::slerp` — not a component-wise lerp, not a
  matrix lerp. Preserving the stated intent is what makes this a port.
- **The orthographic camera path stays byte-identical.** Every existing 2D scene
  uses it.
- **`SpriteRenderer` renders exactly as today** — reads `position.xy` and the
  Z-axis rotation, ignores Z. Do not make sprites world quads here.
- **Physics writes back to the XY plane and the Z-axis rotation**, and says so in
  a comment naming the degenerate case deliberately, so a later reader does not
  read it as an oversight.
- **The gizmo keeps working in 2D.** Making it 3D is F4.
- Reflection rows updated; `Transform::rotation`'s `AngleFormat` attribute now
  applies to the quaternion field from Task 2.

- [ ] **Step 1: Write the failing tests** in `ArcaneTests/src/TransformSpineTest.cpp` —
  identity `ToMatrix()` is the identity mat4; TRS composes in the documented
  order; a quaternion rotation about Z reproduces the old 2D rotation exactly for
  the same angle (**this is the test that proves the 2D case survived**);
  hierarchy composition `parent * local` matches a hand-computed mat4;
  `PreviousTransform` SLERP takes the shortest arc across the ±180° boundary.
- [ ] **Step 2: Run and confirm they fail.**
- [ ] **Step 3: Change the three component types and their reflection.**
- [ ] **Step 4: Fix every consumer until all three configs build at 0 warnings.**
  Work subsystem by subsystem — Scene, Render, Edit, Serialization, Boot, then
  ArcaneEditor, then ArcaneTests. Report anything that is not a mechanical
  widening; those are the interesting ones.
- [ ] **Step 5: Bump the plugin ABI 15 → 16** with a `PluginABI.hpp` changelog
  line in the style of the existing v7 entry ("LocalTransform renamed to
  Transform"), which is the precedent that a `Transform` change is ABI-relevant.
- [ ] **Step 6: Run the gate in all three configs.** **Expect pre-existing case
  counts to hold** — this is a type change, not a behaviour change. Any moved
  count is a defect to explain, not a number to accept. State the delta.
- [ ] **Step 7: Commit** — `feat(scene): one Transform, three dimensions`

---

## Task 4: The propagation rework

Now that the types are settled, replace the algorithm.

**The problem, measured** (full detail in the assessment):
`Relations::ForEachDescendant` (`Astra/Registry/Relations.hpp:158`) copies the
entire traversal cache **by value** via `GetDescendantsCached`
(`RelationshipGraph.hpp:759`), under a `std::shared_mutex`. `TransformSystems`
calls it **twice per frame** (`:45` to collect entities needing a
`WorldTransform`, `:59` to propagate). So each frame pays two mutex locks, two
hash lookups, and **two heap allocations plus full copies of every entity in the
scene** — then four random-access lookups per entity — before any matrix work.

**The fix — decouple structure from values:**
- Own a flat, topologically sorted `order` array plus a `parentIndex` array
  (index into `order`; a sentinel for roots), rebuilt **only when Task 1's
  `StructureVersion()` changes**.
- The per-frame pass becomes linear:
  `world[i] = parentIndex[i] == kRoot ? local[i] : world[parentIndex[i]] * local[i]`.
  Parents precede children, so the parent's world matrix is already final.
- Dirty flags so untouched subtrees are skipped entirely.

**The ordering and dirty policy live in Arcane** — Astra supplied the version and
nothing more. Do not push Arcane's notion of "spatial" down into Astra.

- [ ] **Step 1: Write the failing tests** in `ArcaneTests/src/TransformOrderTest.cpp` —
  the built order is topologically valid (**every parent's index is strictly less
  than its child's**, checked over a deliberately awkward tree: multiple roots,
  a deep chain, a wide fan, and entities added out of order); the order is
  **rebuilt when the hierarchy changes and NOT rebuilt when only component values
  change** (count rebuilds through a counter — this is the property the whole
  task exists for); world matrices after the linear pass are **identical** to
  those the Task 3 recursive implementation produced for the same scene (pin it
  against a known-good reference, not against itself); a clean subtree is skipped.
- [ ] **Step 2: Run and confirm they fail.**
- [ ] **Step 3: Implement** the order cache, the linear pass, and dirty skipping.
- [ ] **Step 4: Confirm the traversal count.** A steady-state frame with no
  structural change must call `ForEachDescendant` **zero** times. Assert it, do
  not eyeball it.
- [ ] **Step 5: Run the gate in all three configs.** State the delta.
- [ ] **Step 6: Commit** — `perf(scene): transform propagation stops copying the scene twice a frame`

---

## Task 5: Re-author the scenes, restamp the consumers

**Clean break — no upgrade path, no version shim.** The corpus is two authored
files; the other five `.arcscene` on disk are build-output copies.

- [ ] **Step 1:** Re-author `ReferenceProject/Content/scenes/main.arcscene`
  against the new shape. It is the engine's canonical fixture — it must open in
  the editor and run in `ArcaneRuntime`.
- [ ] **Step 2:** Restamp Gacha's `Game/Aphelyon.arcproj` to engine ABI 16 and
  re-author `Game/Content/scenes/test.arcscene`. **This is a change in the Gacha
  repo** (`D:\dev\starworks\Gacha`) — a separate commit there, in the style of the
  existing `chore(game): restamp Aphelyon.arcproj to engine ABI 15`.
- [ ] **Step 3: Gate all three configs**, then **stop** — the rest is the desk
  checkpoint.
- [ ] **Step 4: Commit** — `chore(reference): re-author main.arcscene for the 3D transform`

---

## Task 6: DESK CHECKPOINT F1 (USER)

**Do not attempt, simulate, or mark this done. Stop and hand off.**

No GPU or windowed runs in an agent session, and `ArcaneTests` compiles neither
host. Everything below is the half no automated signal in this repo can reach.

- [ ] Open `ReferenceProject` in the Release editor. Entities appear at the right
      places; the Inspector shows position/rotation/scale with **rotation as
      Euler angles** that edit sanely and do not drift when untouched.
- [ ] **The 2D path is visually unchanged** — sprites, the selection outline in
      Edit **and** Play, physics debug draw, and the game UI in Play.
- [ ] The gizmo still translates/rotates/scales in 2D, and **one drag is still
      one undo**.
- [ ] Reparent entities in the Outliner; children follow their parents, and the
      order cache rebuilds without a hitch or a frame of stale transforms.
- [ ] `ArcaneRuntime --project ReferenceProject` runs the same scene.
- [ ] Both hosts load their game module on both backends at ABI 16.
- [ ] **Phase 4's two carried desk items**, still owed and now testable in the
      same sitting: the `frontCounterClockwise = true` + `CullMode::BACK` pair,
      and the depth-attachment path in `RenderGraphExec.cpp` that has never
      executed anywhere in this tree.

---

## Verification ladder (F1 exit criteria)

1. All three configs build with **0 warnings**.
2. The gate passes in all three configs at the number these tasks accumulate to —
   **derived by running, never recalled**.
3. **No pre-existing case count moved** except where a task explains why.
4. A steady-state frame makes **zero** `ForEachDescendant` calls.
5. World matrices from the linear pass are identical to the recursive reference.
6. Both hosts load their module in all three configs at ABI 16.
7. The desk items above are confirmed by hand.

## Carried into F2 and later

- **`SpriteRenderer` as a world quad** — still open, and it is what resolves the
  compositing question in F5 rather than working around it. Recommendation is in
  the assessment.
- **F2** — `MeshRenderer`, mesh and material assets, and the cook pipeline that
  makes them real. Phase 4's **Task 8 (bindless table) is held for this**.
- **F3** — bounds, frustum culling, draw sorting. None exist today.
- **F4** — 3D gizmo, camera orbit/fly, and *verifying* whether 3D picking already
  works (picking is a GPU id-buffer read, so it may be free).
- **F5** — declarative clear-op, unified world pass with shared depth.
- **Box3D** — vendored as its own arc after F1, per the assessment's reasoning.
- **Phase 4 Task 9** then becomes trivially correct: the scene will have meshes.
