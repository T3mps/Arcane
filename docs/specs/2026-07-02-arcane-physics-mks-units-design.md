# Arcane Physics: MKS Units Normalization — Design

**Date:** 2026-07-02
**Status:** Approved (user-reviewed section by section)
**Parity reference:** vendored Box2D v3.1.1 at `ThirdParty/box2d-3.1.1` — every constant in this
spec is cite-checked against that source (file:line), not recalled. Any question or invariant that
arises during implementation MUST be checked against it directly (user directive).

## 1. Motivation

Arcane's world convention has been "1 world unit == 1 canvas pixel" (Camera.hpp:15, Scenes.cpp:42),
with sandbox content authored at ~90 px/m (gravity 900 == 10 m/s^2 at 90 px/m). Engine constants
were a mix: some carried Box2D's meter values verbatim (kLinearSlop 0.005, maxLinearVelocity 400),
some were re-tuned to pixels (sleepThreshold 8, restitutionThreshold 20, contactPushMaxVelocity 300,
tree margin 8). Running meter-valued constants against pixel-scale content put the solver ~90x off
Box2D's tested operating point. Observed casualties:

- Ordinary free-fall rides the 400 u/s clamp as a low terminal velocity (~0.44 s to cap at g=900).
- The dynamic-body CCD tests went VACUOUS when the clamp landed (12000 u/s probes capped to 400
  never reach the wall; one-sided assertions still pass). The sandbox bullet scene (7000 u/s) is
  likewise capped to a crawl.
- The pile never slept until sleepThreshold was hacked from Box2D's 0.05 to an empirical 8 "px/s"
  (2026-06-28 sleep-alignment spec) — treating a mis-scale as a tuning problem.
- Drag/throw interactions cap at 400 u/s (SandboxInteractionTest was interim-adjusted).

**User directive (2026-07-02): Arcane uses meters for all distance, m/s for speed, and defaults to
Box2D v3 for constants. Constants are never rescaled to content; content is rescaled to MKS.**

## 2. Units convention (binding, inherited by future milestones)

- Arcane world space is **MKS**: meters, seconds, kilograms, radians. **y-down** (+y = down), so
  Box2D's default gravity (0,-10) y-up (types.c:12-13) carries as **(0,+10)**.
- Physics, transforms, scenes, and all engine defaults are in meters. Nothing in Core or the render
  core carries a pixel unit.
- **The camera owns the meters->pixels projection** (industry standard: Unity 1u=1m + camera
  projection + per-asset pixels-per-unit; Box2D manual: "do not use pixels as your length unit").
  Sandbox: `screen = world * (pixelsPerMeter * zoom) + offset`, **pixelsPerMeter = 100**, zoom
  stays the user-adjustable factor (zoom=1 -> 100 px/m). M7/Grimoire's camera system inherits this
  principle; an engine-level RenderContext scale seam is explicitly deferred until a real consumer
  exists. Sprite-asset pixels-per-unit is a future import-time concern.
- Bodies are authored in Box2D's happy range (~0.1-10 m); scenes use round meter numbers
  (re-authored, not mechanically divided).

## 3. Engine constant flips

Every "Box2D v3" value below is verified against the vendored source.

| Constant (Arcane) | Today | MKS target | Box2D v3 cite |
|---|---|---|---|
| `WorldDef.gravityX/Y` | (0, 0) | **(0, +10)** y-down | types.c:12-13 (0,-10) y-up |
| `WorldDef.sleepThreshold` | 8 | **0.05** | types.c:34 (BodyDef-level in b2; Arcane keeps WorldDef default + BodyDef inherit=-1 architecture, value carries) |
| `WorldDef.restitutionThreshold` | 20 | **1.0** | types.c:15 |
| `WorldDef.contactPushMaxVelocity` | 300 | **3.0** | types.c:16 `maxContactPushSpeed` |
| `WorldDef.maxLinearVelocity` | 400 | **400** (unchanged, now honest m/s) | types.c:21 |
| `WorldDef.contactHertz` / `contactDampingRatio` | 30 / 10 | unchanged (already match) | types.c:17-18 |
| `kLinearSlop` | 0.005 | **0.005** (unchanged, now honest meters) | constants.h:23 |
| `kSkin` | 0.01 | **0.02** | constants.h:38 `B2_SPECULATIVE_DISTANCE = 4 * B2_LINEAR_SLOP` |
| `kMaxRotation` | 0.25*pi | unchanged (unit-free) | constants.h:33 |
| `kSleepTime` | 0.5 s | unchanged (unit-free seconds) | constants.h:47 `B2_TIME_TO_SLEEP` |
| `DynamicTree::kMargin` | 8 | **0.05** | constants.h:44 `B2_AABB_MARGIN` (NOTE: 0.05 in v3.1.1, not the 0.1 of older Box2D) |
| `WorldDef.hashCellSize` | 64 | **1.0** | Arcane-specific grid tuning (~2-4x typical body) |
| `SpatialHash` default cellSize | 64 | **1.0** | Arcane-specific |
| residency grid tile (`m_residencyGrid`) | 64 | **1.0** (TODO wire to map tile size remains) | Arcane-specific |
| `CharacterController::kMaxSubstep` | 8 (px) | **0.1** | Arcane-specific retune (max depenetration step) |
| `CharacterController::kDepenetrationSkin` | 0.05 | **0.02** (= kSkin) | Arcane-specific retune |

**Known scale landmines (Phase 1 fixes + sweeps for more):**
- `pad = max(2, specMargin)` hardcoded 2-px floor in the contact pad (PhysicsWorld.cpp:2714-2717).
  Left alone it becomes a 2-METER pad. Re-derive from `kSkin` (e.g. `max(2*kSkin, specMargin)` —
  exact expression decided against the vendored contact-margin logic in implementation).
- SpatialGrid guard bounds re-derive sanely at 1 m tiles: magnitude bound 65536*tileSize -> ~65 km,
  total-cell budget 1<<22 at 1 m tiles -> ~2 km x 2 km solid; both still far beyond legitimate
  content — values stay, semantics re-checked in tests.
- `kBulletEpsilon = 0.001` already meter-sane; unchanged.
- Phase 1 includes a literal-audit pass over Core/Physics for any remaining numeric constant with
  length semantics (grep for numeric literals in expressions involving positions/velocities/AABBs).

Angular quantities (rad, rad/s), Hz, damping ratios, Baumgarte/beta factors, and iteration counts
are unit-free and unchanged. `m_maxExtent` is computed from geometry and auto-scales.

## 4. Migration: phased strangler with PX-PIN scaffolding

User-selected over flag-day (full-MKS rewrite of all ~40 test files WAS selected over pinning
forever — the pins below are temporary scaffolding, not the end state).

**Phase 1 — flip + pin (one merge, suite green with identical counts).**
Land the Section-3 table in Core. In the same branch, a mechanical pass over every existing
physics/sandbox test file writes the OLD values explicitly into each `WorldDef`/`BodyDef` that
today inherits a flipped default, each tagged with a greppable marker:
`wd.sleepThreshold = 8; // PX-PIN: remove when this file converts to MKS`.
Sandbox app code gets the same pinning in its WorldDef setup (keeps the app behaving until
Phase 6). `grep -c PX-PIN` is the burn-down meter. Behavior is unchanged by construction —
verified by identical suite counts.

**Phases 2-5 — per-cluster MKS conversion (one merge each).** Each phase rescales its cluster's
content to meters (bodies 0.1-10 m, g=10, m/s velocities), deletes its PX-PINs, re-derives
baselines on the Box2D operating point:
- **P2 solver/dynamics:** Solver, Baumgarte, Dynamics, Rotation, CompoundCom, CompoundSlide,
  SimdSolver, CompactedSolve, BodyContacts, PersistentContact, PersistentIsland, Determinism,
  Phase1/Phase2 harnesses.
- **P3 sleep/settle/island (carries the acceptance risk, Section 6):** SleepThreshold,
  StaticSettle, AwakeSet, Island, IslandWakeMerge, JointSleep, SensorIsland.
- **P4 broadphase/spatial:** Broadphase, SpatialGrid, FixtureBroadphase, TileGrid, Queries,
  QueryRotation, Character, Invariants, SolverMtInvariance, BroadphaseMtInvariance, NarrowphaseMt.
- **P5 CCD + clamp + joints:** Ccd (re-armed: dynamic probes at ~300 m/s — genuinely fast, under
  the 400 cap, walls thin in meters), VelocityClamp, Joints (lengths/anchors/impulses in meters,
  impulses mass-scaled).
- Geometry-pure files (EPA/MPR/GJK/manifold/shapes/fixtures/contact-pool/debug-draw) are
  scale-relative; they rescale opportunistically in whichever phase touches them.

**Phase 6 — sandbox + camera.**
- All 8 scenes re-authored in round meters (floor ~8-9 m halfW, boxes ~0.5 m half, whisk radius
  ~5.6 m, stack kHalf ~0.5 m), `kGravityY = 10`.
- Camera: `pixelsPerMeter = 100` folded into the transform; SandboxCameraTest pins the new form.
- Interaction: `kDragMaxSpeed` 40 m/s, `kDragMaxAccel` 400 m/s^2, `kDragMaxAngVel` unchanged
  (rad/s). **Pick radius becomes screen-px converted through the camera** (picking is a screen
  affordance; it must not shrink when zoomed out).
- Bullet scene: ~70 m/s (per-step displacement 1.17 m >> 0.18 m wall — genuinely tunnels without
  CCD, well under the 400 cap).
- HUD slider ranges to MKS (gravity 0-30, spawn size 0.04-0.8 m, COM/tick sizes in meters);
  debug-viz world-unit defaults retuned (orientationTickLen 0.18, comMarkerSize 0.05).
- Sandbox tests converted (including properly rewriting the two interim-adjusted tests from
  2026-07-02: SandboxInteractionTest drag, SandboxHudTest settle arithmetic); last PX-PINs deleted.
- CLAUDE.md Arcane section gains one line stating the MKS convention.

Each merge is CI-green. Mid-stream the tree stays consistent: new defaults live, converted
clusters MKS, unconverted clusters pinned and visibly marked.

## 5. What does NOT change

- Solver algorithms, island/sleep machinery, broadphase structure, MT machinery, determinism
  guarantees (ST==MT byte-identity per run) — this is a pure data/constant rescale; the MT
  invariance tests re-baseline like everything else.
- `m_staticList` consolidation stays deferred (benchmark decision, unchanged).
- Engine-level render seam, sprite-asset PPU import, client (Love2D) port conversion: out of scope.
- b2 `enableSleep`/`enableContinuous` WorldDef toggles: Arcane has no equivalents; adding them is
  NOT in scope (noted as possible future parity knobs).
- Vendored `ThirdParty/box2d-3.1.1` stays untracked; committing it is the user's call.

## 6. Testing & acceptance

- **Per-phase:** full suite green at every merge; converted clusters on re-derived baselines,
  unconverted clusters pinned-identical. `grep PX-PIN` count decreases monotonically to zero.
- **Phase 6 exit:** headless `Loom --frames` GPU-verify clean; no px-unit comment anywhere in
  Core/Physics; piles settle AND sleep; CCD scene demonstrates tunneling-prevention; drag/throw
  feel restored.
- **THE acceptance risk — pile sleep at 0.05 m/s (Phase 3).** The px-era pile jittered at
  ~7 px/s (~0.078 m/s-equivalent), above 0.05 — that is why the 8 hack existed. Expectation: the
  jitter was the product of mutually mis-scaled solver lengths (slop acting 90x too tight, push-out
  300, margin 8, skin 0.01), i.e. operating far off Box2D's tested point; at true MKS the solver
  sits exactly where 0.05 is validated (Box2D piles sleep). P3 ports the pile/settle scenes to
  meters and asserts sleep within kSleepTime margins. **Contingency: if piles do not sleep, it is
  a parity BUG — diff Arcane's solver against the vendored Box2D on a matched scenario and fix the
  divergence. Do NOT bump the threshold; that was the px-era hack.**
- f32 precision improves (coordinates 0.1-10 m vs 10-1300 px).

## 7. Open items recorded elsewhere

- Box2D-parity program notes (separate ledger): kinematic/pinned bodies are not speed-clamped
  (awake-set integrate covers dynamics only; Box2D clamps its whole awake set, solver.c:79-122);
  gravity/damping ordering divergence (Arcane damps gravity, Box2D adds undamped gravity delta
  after damping, solver.c:102-106). Both pre-existing, both orthogonal to units.
- 2026-06-28 sleep-alignment spec's `sleepThreshold=8` decision is superseded by this spec
  (root cause was scale, not threshold).
