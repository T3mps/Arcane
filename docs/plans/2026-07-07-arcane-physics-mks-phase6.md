# Arcane MKS Units — Phase 6 (Sandbox + Camera) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-author all 9 sandbox scenes in round meters (uniform /100, 100 px/m), fold `pixelsPerMeter = 100` into the camera transform, retune drag/throw/pick/spawn and the HUD/debug-viz to MKS, convert the 4 px-pinned sandbox files (35 PX-PINs deleted, repo total 670 -> 635), and measure the SandboxSmoke wall-time restoration (~84 min/backend -> minutes).

**Architecture:** Spec `docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md` section "Phase 6 — sandbox + camera" (lines 128-141) + section 6 exit criteria. **ORDER AMENDMENT (user decision 2026-07-07): P6 runs BEFORE P3-P5** — it kills the px-content-on-1m-residency-tile smoke pathology (measured 5066 s/backend at P2 exit) so P4/P5's engine-touching exit gates become cheap. Survey fact base: `.superpowers/sdd/p6-sandbox-survey.md` (all old values below are survey-verified with file:line). Three implementation tasks (scenes/worlds -> camera+interaction -> HUD/debug-viz), then a controller-run exit-verification task. Scenes convert BEFORE the camera so physics re-derives are camera-independent; the interaction TESTS convert with the camera task because cursor coordinates flow through `ScreenToWorld`.

**Tech Stack:** C++23 (Arcane Sandbox plugin + tests), MSVC via msbuild, Catch2, ImGui (HUD). Parity/spec source: vendored `ThirdParty/box2d-3.1.1` + the MKS design spec — verify invariants against them directly, never recall.

## Global Constraints

- **Conversion protocol:** `.superpowers/sdd/p2-conversion-protocol.md` applies verbatim (PX-PIN deletion rule, MKS WorldDef defaults, empirical re-baseline rule ~1.5x headroom + named driving constant, ASCII-only comments, explicit-path staging, commit trailers, FOREGROUND-only builds/tests). Where this plan and the protocol overlap, this plan's exact values win.
- **Scale factor: 100 px/m, uniform.** Every length /100, every velocity /100, angles/densities/frictions/restitutions/zoom-factors/time-scales unchanged. Round to the nearest 0.01 m where the scenario allows (keep size VARIETY — do not collapse 0.54/0.66/0.46 into one value; 2-significant-figure rounding like 0.54 -> 0.54 or 0.55 is fine, document any rounding beyond exact /100).
- **Engine/Core untouched.** This phase edits ONLY `Arcane/Sandbox/src/*` + `Arcane/Tests/src/Sandbox*.cpp` + `CLAUDE.md`. The engine's `MouseJoint maxForce 1e9` (MKS-DEFER(P5)) is NOT in scope — survey confirmed the sandbox drag is a hand-rolled bounded impulse, not an engine mouse joint.
- **Behavioral asserts pass UNMODIFIED** (contact counts, sleep flags, outline-unify contracts, `RenderErrorCount()==0`, draft-render contracts). A behavioral failure = STOP, parity investigation, report BLOCKED.
- **Suite green at every commit.** Sandbox tag runs: `.\ArcaneTests.exe "[sandbox]" ~[gpu]` (fast, per task). Full gates only at Task 4.
- **Branch:** `feature/arcane-physics-mks-phase6` off `feature/arcane-physics-mks-phase2` HEAD (stacked; P2 merge = USER's call, this branch fast-forwards on top).
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466
  ```

## File Structure

| File | Change | Task |
|---|---|---|
| `Arcane/Sandbox/src/Scenes.cpp` | all 9 scenes + helpers -> meters; `Spinner()` dead code DELETED | 1 |
| `Arcane/Sandbox/src/Scenes.hpp` | stress-knob comments (counts unchanged) | 1 |
| `Arcane/Sandbox/src/Sandbox.cpp` | 5 pins deleted; `kGravityY` 900 -> 10 | 1 |
| `Arcane/Sandbox/src/SandboxApp.cpp` | 5 pins deleted (gravity stays caller-supplied) | 1 |
| `Arcane/Tests/src/SandboxVisualsTest.cpp` | 20 pins deleted; 4 standalone worlds + stress bounds -> meters | 1 |
| `Arcane/Tests/src/SandboxHudTest.cpp` | `kGravityY` -> 10; settle arithmetic re-derived; isolated-spawn literals -> meters | 1 |
| `Arcane/Sandbox/src/Camera.hpp` | `kPixelsPerMeter = 100` folded into both transforms | 2 |
| `Arcane/Tests/src/SandboxCameraTest.cpp` | pins the NEW transform form (TDD) | 2 |
| `Arcane/Sandbox/src/Interaction.hpp` | drag caps + spawn size -> MKS; pick radius -> screen-px affordance | 2 |
| `Arcane/Sandbox/src/Interaction.cpp` | `PickBodyAt` converts px radius through the camera | 2 |
| `Arcane/Tests/src/SandboxInteractionTest.cpp` | 5 pins deleted; full conversion incl. the interim drag test rewritten properly | 2 |
| `Arcane/Sandbox/src/Hud.cpp` | slider ranges -> MKS | 3 |
| `Arcane/Sandbox/src/SandboxApp.hpp` | debug-viz defaults -> meters | 3 |
| `CLAUDE.md` | one-line MKS convention bullet | 4 |

---

### Task 1: Scenes, WorldDefs, and scene-dependent tests -> meters (one commit)

**Files:**
- Modify: `Arcane/Sandbox/src/Scenes.cpp`, `Scenes.hpp`, `Sandbox.cpp`, `SandboxApp.cpp`
- Modify: `Arcane/Tests/src/SandboxVisualsTest.cpp`, `Arcane/Tests/src/SandboxHudTest.cpp`

**Interfaces:**
- Consumes: MKS WorldDef defaults (gravity (0,+10), sleepThreshold 0.05, restitutionThreshold 1.0, contactPushMaxVelocity 3.0, hashCellSize 1.0) — pin deletion falls back to these.
- Produces: all scene geometry in meters at exactly /100 of the px layout; Task 2's interaction tests and Task 4's smoke/Loom gates run against THIS content.

- [ ] **Step 1: Branch.** `git checkout -b feature/arcane-physics-mks-phase6` (from the P2 branch HEAD). Build once; run `.\ArcaneTests.exe "[sandbox]" ~[gpu]` from the exe dir and record the baseline counts.

- [ ] **Step 2: Convert the WorldDef sites + gravity.**
  - `Sandbox.cpp`: delete the 5 PX-PIN lines (:57-61); change `constexpr float kGravityY = 900.0f;` (:44) -> `10.0f` (keep the constant + `Configure(kGravityY)` call — the HUD round-trips it).
  - `SandboxApp.cpp`: delete the 5 PX-PIN lines (:49-53); `wd.gravityY = gravityY;` stays caller-supplied.
  - `SandboxVisualsTest.cpp`: delete all 4 blocks x 5 pins (:89-93, :122-126, :193-197, :255-259); the 4 `wd.gravityY = Real(900)` lines above them (:88, :121, :192, :254) -> `Real(10)` OR delete for the default (delete preferred; keep only if the case comments on gravity).
  - `SandboxHudTest.cpp`: `constexpr float kGravityY = 900.0f;` (:55) -> `10.0f`.

- [ ] **Step 3: Convert Scenes.cpp — helpers first.**
  - `MakeFloor` defaults (:219-225): `centerX 640 -> 6.4f, topY 800 -> 8.0f, halfW 880 -> 8.8f, halfH 36 -> 0.36f`.
  - DELETE `Spinner()` entirely (:339-376) — grep-verified dead code (only its own definition references it). Note the deletion in the commit body.
  - `Whisk()` (:385-464): `kWireHalf 15 -> 0.15f`; the `W = R*0.30f` / `hubR = R*0.12f` formulas and `kLoops = 6` unchanged. Update the no-tunnel comment arithmetic (:383-384, :807-809): tip speed = 1.9 * 5.6 = 10.64 m/s -> 0.177 m/step at 60 Hz, under the 0.30 m wire diameter — same margin by construction.
  - Densities/frictions/restitutions in ALL helpers unchanged.
- [ ] **Step 4: Convert the 9 scene builders (/100, exact values).**
  - **Playground** (:469-495): floor `(6.4, 8.2) h(7.6, 0.36)`; walls `(-0.8, 5.6) h(0.36, 3.0)` and `(13.6, 5.6) h(0.36, 3.0)`; boxes `(4.4, 1.2) h(0.54, 0.54)`, `(6.4, 0.4) h(0.66, 0.42)`, `(8.6, 0.9) h(0.46, 0.46)`; circles `(5.4, 2.6) r 0.50`, `(7.6, 2.0) r 0.58`.
  - **Box stack** (:500-520): `kHalf 0.52f` (spec "~0.5"), `kGap 0.03f`, `kTopSurf 8.0f`, `kCenterX 6.4f`, `kCount 8` unchanged.
  - **Pyramid** (:525-551): `kHalf 0.46f`, `kSpacing = 2*kHalf + 0.03f`, `kTopSurf 8.0f`, `kCenterX 6.4f`, `kRows 6` unchanged.
  - **Joint chain** (:559-622): ceiling `(6.4, 0.9) h(5.6, 0.18)`; `kBobR 0.28f`; revolute hub `(3.6, 1.3) h(0.14, 0.14)`, bob `(3.6, 3.6)`, anchor `(3.6, 1.3)`; distance bob `(5.2, 3.4)`, `length Real(2.3)`; weld post `(8.2, 1.3)`, welded `(8.9, 1.3)`, anchor `(8.9, 1.3)`; prismatic slider `(10.0, 1.3)`, axis unchanged. Prismatic nudge impulse (:621) rewrites per protocol rule 3: `const Real bobMass = Real(1) * kPi * kBobR * kBobR; // density 1 (WorldCircle)` then `ApplyImpulse(slider, Vec2(bobMass * kNudgeDv, 0))` with `kNudgeDv = Real(1.5)` m/s named in a comment (visible slide, well under the cap; the px impulse's implied dv was ~3.3 px/s — a value that would be invisible at meters, so this is a deliberate re-author, not /100; verify the slider visibly translates in Step 7's smoke).
  - **Rotation drop** (:631-649): floor `(6.4, 8.2) h(8.2, 0.36)`; polygon boxes `(4.2, 1.6) h(0.58, 0.58) a=0.6`, `(6.2, 0.8) h(0.70, 0.42) a=-0.9`, `(8.2, 2.0) h(0.52, 0.52) a=1.2`, `(6.4, 3.2) h(0.80, 0.34) a=-0.4` (angles radian, unchanged).
  - **CCD bullet** (:657-698): wall `(11.2, 6.6) h(0.09f, 1.5f)` (0.18 m thick — spec); bullet spawn `(1.2, 6.6)`, `rb.velocity = glm::vec2(70.0f, 0.0f)` (spec ~70 m/s; per-step travel 1.17 m >> 0.18 m wall — genuinely tunnels without CCD, under the 400 cap), fixture `halfW/halfH 0.18f`, density 4.0 / friction 0.3 / restitution 0.0 / fixedRotation / bullet flags unchanged. Update the ":678 fast enough to tunnel" comment with the meter arithmetic.
  - **Compound** (:706-750): core `r 0.34f, density 0.5f`; heavy `r 0.42f, density 4.0f, localPos (heavySign * 0.74f, 0)`; instances `(4.6, 2.4)` and `(8.4, 1.8)`.
  - **Mixed shapes** (:758-788): bowl floor `(6.4, 8.2) h(5.6, 0.36)`; walls `(1.2, 6.8) h(0.36, 1.8)` / `(11.6, 6.8) h(0.36, 1.8)`; circles `(5.2, 1.4) r 0.44` / `(7.6, 0.9) r 0.36`; capsules `(6.0, 2.6) halfLen 0.56 r 0.28` / `(8.2, 3.2) halfLen 0.44 r 0.24`; Path-B `WorldStaticBox (6.4, 8.2) h(5.6, 0.36)`, `WorldPolygonBox (5.8, 4.0) h(0.54, 0.40) a=0.5`, `WorldTriangle (7.2, 4.6) size 0.54`.
  - **Stress test** (:821-1009): `kWhiskRadius 5.6f` (spec), `kWhiskOmega 1.9f` unchanged, whisk center `(6.4, 3.6)`; `kMinBodyHalf 0.18f`, `kMaxBodyHalf 0.32f`, `kPitch = kMaxBodyHalf*2 + 0.18f` (= 0.82 m); floor `y 8.8f, halfW 8.8f, halfH 0.4f`; walls `halfW 0.5f, halfH 10.0f`; arena `left -2.2f, right 15.0f`; fillets `halfL 2.5f, halfT 0.36f`, angle/placement offsets `144 -> 1.44f`. `kStressSeed`, shape weights, densities (0.07-0.08), frictions unchanged. Update the Scenes.hpp:14-15 + Scenes.cpp:800-814 spawn-column comments: 10000 bodies / 20 cols -> top row y ~ -404 m (was ~ -40,000 px); stability subset 1200 -> top ~ -40.4 m.
- [ ] **Step 5: Convert the scene-dependent tests (same commit — the suite must stay green).**
  - `SandboxVisualsTest.cpp` standalone worlds (4 blocks): /100 all geometry per the same recipe (each block's floor/bodies mirror scenes above — survey section 2 lists the blocks; apply /100 to every length literal). Stress cases: `kYMin -5000 -> -50.0f` (1200-body column top ~ -40.4 m + fall headroom; re-derive empirically per protocol rule 6 and justify inline naming the column arithmetic). Outline-unify behavioral asserts (zero-SpriteRenderer contract, outline counts) UNMODIFIED.
  - `SandboxHudTest.cpp`: re-derive the 200-step settle (6 call sites listed in survey section 8b): scene-0 drops are now ~5-6.8 m under g=10 -> first contact ~1.05-1.17 s (~63-70 steps) + settle-to-sleep; measure empirically, set the count with headroom, justify inline (probe battery precedent: an 8-box pile fully sleeps by step ~338 from comparable heights; expect 200 still suffices — VERIFY, do not assume). KEEP `WakeAllDynamics` + the one extra `Step()` (the EmitContactConstraints awake-gate is scale-independent). Isolated-spawn literals (:759-771): `(5000, -5000) -> (50.0f, -50.0f)`, `size 10 -> 0.5f`, `tolerance 50 -> 0.5f` ("far from the scene" is the only requirement).
- [ ] **Step 6: Build + run + triage.** Build (foreground). `.\ArcaneTests.exe "[sandbox]" ~[gpu]` green; PX-PIN grep: `Sandbox.cpp`, `SandboxApp.cpp`, `SandboxVisualsTest.cpp` == 0 (SandboxInteractionTest keeps its 5 until Task 2). Triage per protocol rule 6.
- [ ] **Step 7: Visual sanity (cheap, catches transposed digits).** `Loom.exe --frames 120` headless (exe dir `..\Loom`); clean exit. NOTE: the view will look ZOOMED-OUT/tiny (meter content on the still-1:1 camera) — that is EXPECTED mid-branch; the gate is exit 0 + no render errors, not framing.
- [ ] **Step 8: Commit** (explicit paths: the 6 files). Subject: `feat(arcane/sandbox): all 9 scenes + worlds re-authored in meters, pins deleted (MKS P6)`. Body: the /100 mapping table, the Spinner deletion note, every empirical re-derive (settle count, kYMin) with measured -> bound -> justification.

---

### Task 2: Camera pixelsPerMeter + interaction retune + interaction-test conversion (one commit)

**Files:**
- Modify: `Arcane/Sandbox/src/Camera.hpp`, `Interaction.hpp`, `Interaction.cpp`
- Modify: `Arcane/Tests/src/SandboxCameraTest.cpp`, `Arcane/Tests/src/SandboxInteractionTest.cpp`

**Interfaces:**
- Consumes: Task 1's meter scenes (a 12.8 x 7.2 m layout).
- Produces: `Camera::kPixelsPerMeter = 100.0f`; `WorldToScreen(w) = w * (kPixelsPerMeter * zoom) + offset`; `ScreenToWorld(s) = (s - offset) / (kPixelsPerMeter * zoom)`. At default `zoom=1, offset=(0,0)` the 12.8 x 7.2 m layout fills a 1280x720 canvas exactly as the px layout did. Task 3's HUD and Task 4's gates rely on this.

- [ ] **Step 1 (TDD): rewrite SandboxCameraTest to pin the NEW form.**
  - Case "round-trips": `offset=(100,50), zoom=2` -> `WorldToScreen({10,10}) == (10*100*2+100, 10*100*2+50) == (2100, 1050)`; inverse returns `(10,10)`.
  - Case "identity defaults": default camera -> `WorldToScreen(p) == p * 100.0f`; add the explicit framing assertion `WorldToScreen({12.8f, 7.2f}) == (1280, 720)`.
- [ ] **Step 2: Run to verify RED.** `.\ArcaneTests.exe "[sandbox]"` — the two camera cases fail against the old 1:1 transform (expected).
- [ ] **Step 3: Implement Camera.hpp.**
  ```cpp
  // World is METERS (MKS); the canvas is pixels. kPixelsPerMeter folds the
  // unit conversion into the one transform pair everything routes through.
  static constexpr float kPixelsPerMeter = 100.0f;
  glm::vec2 WorldToScreen(glm::vec2 world) const noexcept { return world * (kPixelsPerMeter * zoom) + offset; }
  glm::vec2 ScreenToWorld(glm::vec2 screen) const noexcept { return (screen - offset) / (kPixelsPerMeter * zoom); }
  ```
  Wheel zoom-to-cursor, pan, and every other consumer route through these two functions (survey-verified) — no other camera edits.
- [ ] **Step 4: Interaction constants + pick-radius affordance.**
  - `Interaction.hpp`: `kDragMaxSpeed 4000 -> 40.0f` (m/s, spec), `kDragMaxAccel 40000 -> 400.0f` (m/s^2, spec), `kDragMaxAngVel 8.0f` UNCHANGED (rad/s); `SpawnConfig::size 22 -> 0.22f`; `kPickRadius 4.0f` -> rename `kPickRadiusPx = 4.0f` with the comment: `// screen-px pick affordance; converted to world through the camera each query so picking does not shrink when zoomed out (spec P6)`. 4 px preserves the exact zoom=1 apparent size the old 4-world-unit radius had.
  - `Interaction.cpp` `PickBodyAt` (:45-66): compute `const float worldR = kPickRadiusPx / (Camera::kPixelsPerMeter * cam.zoom);` and use it for the probe circle (function already receives the camera or gains the parameter from its one caller — survey: Tick has `cam` in scope).
  - Drag/throw math (:196-280) is otherwise untouched — the caps are the only px-scale values in it.
- [ ] **Step 5: Convert SandboxInteractionTest.cpp (full).**
  - Delete the 5 PX-PIN lines (:85-89); `kGravityY 900 -> 10.0f` (:61).
  - Every cursor coordinate the tests feed via `Snap(x, y, ...)` is SCREEN px: author test worlds in meters and produce cursor positions via `cam.WorldToScreen(worldTarget)` (identity camera: screen = world * 100). Convert every authored body position/size /100.
  - **The interim drag test (:190-242) is REWRITTEN PROPERLY** (this was closeout item; the 130u/25u numbers were a symptom of px content on the honest 400 cap): body at `(3.0, 3.0)` m, target `(4.2, 2.5)` m (1.3 m drag); 30 ticks at 1/60 s with `kDragMaxSpeed = 40` converges easily; re-derive the closeness threshold empirically (protocol rule 6: measure endDist, bound with ~1.5x headroom, justify inline against kDragMaxSpeed/kDragMaxAccel; the old ~19%-of-distance ratio is the sanity reference, expect FAR tighter at MKS). Delete the 2026-07-02 interim-adjustment comment block (:210-222) — replace with the meter derivation.
  - Pick tests: probe offsets that asserted "just inside/outside pick radius" re-derive from `kPickRadiusPx / 100` at zoom 1 (= 0.04 m) — and ADD one case asserting the affordance: at `zoom = 0.5`, a body `0.06 m` from the cursor's world point still picks (0.04 / 0.5 = 0.08 m world radius) — the anti-shrink contract.
  - Spawn tests: expected half-extents/radii now `SpawnConfig::size = 0.22` derived; zoom/pan/click-through/polygon/hull cases are dimensionless or scale with authored content — convert their authored literals /100, behavioral asserts unmodified.
- [ ] **Step 6: Build + run + triage.** `.\ArcaneTests.exe "[sandbox]" ~[gpu]` green; SandboxInteractionTest PX-PIN grep == 0. Cross-check: `Loom.exe --frames 120` — framing is now RESTORED (meter scenes at ppm=100 fill the window like the px original).
- [ ] **Step 7: Commit** (explicit paths: the 5 files). Subject: `feat(arcane/sandbox): pixelsPerMeter=100 camera + MKS drag/pick/spawn retune (MKS P6)`. Body: transform form, constant table (old -> new -> spec cite), pick-affordance behavioral note, drag-test re-derivation row.

---

### Task 3: HUD ranges + debug-viz defaults -> MKS (one commit)

**Files:**
- Modify: `Arcane/Sandbox/src/Hud.cpp`, `Arcane/Sandbox/src/SandboxApp.hpp`
- Modify: `Arcane/Tests/src/SandboxHudTest.cpp` (only if any case pins a changed default/range — implementer triages)

**Interfaces:**
- Consumes: Task 1's `kGravityY = 10`, Task 2's spawn size 0.22.
- Produces: HUD authored in meters; debug-viz world-unit defaults per spec.

- [ ] **Step 1: Hud.cpp slider ranges.**
  | Control | Old (line) | New | Format |
  |---|---|---|---|
  | Gravity Y | `0..2400` (:443) | `0.0f..30.0f` (spec) | `%.1f` |
  | Size (spawn) | `4..80` (:511) | `0.04f..0.8f` (spec) | `%.2f` |
  | COM size | `2..16` (:535) | `0.02f..0.16f` | `%.2f` |
  | Tick length | `4..48` (:540) | `0.04f..0.48f` | `%.2f` |
  | Time scale / Density / Line thickness / Vel scale | unchanged (dimensionless, kg/m^2, screen px, seconds) | — | — |
- [ ] **Step 2: SandboxApp.hpp debug-viz defaults.** `comMarkerSize 5.0f -> 0.05f` (:97, spec), `orientationTickLen 18.0f -> 0.18f` (:99, spec); `velocityScale 0.15f` UNCHANGED (seconds: arrow = v * scale, now meter-honest by construction); `lineThickness 1.0f` UNCHANGED (screen px); `kMarkerPx 4.0f` UNCHANGED (:259, screen-space by design — add the one-line comment saying so if absent).
- [ ] **Step 3: Build + run + triage.** `.\ArcaneTests.exe "[sandbox]" ~[gpu]` green; fix any HudTest case that pinned an old range/default (re-pin to the new value with the spec cite — that is a content update, not an assert weakening).
- [ ] **Step 4: Commit** (explicit paths). Subject: `feat(arcane/sandbox): HUD ranges + debug-viz defaults in meters (MKS P6)`. Body: the range table above.

---

### Task 4: Phase 6 exit verification (controller-run)

**Files:** Modify: `CLAUDE.md` (one bullet). Otherwise verification only; fixes loop back into the owning task.

**Interfaces:** Consumes everything above. Produces the P6 exit evidence incl. THE WALL-TIME RESTORATION MEASUREMENT.

- [ ] **Step 1: Burn-down.** `grep -rc "PX-PIN" Arcane/Tests/src Arcane/Sandbox/src | grep -v ":0"` — total **635** (670 - 35), ZERO in Sandbox/src and in SandboxInteractionTest/SandboxVisualsTest. `grep -rn "MKS-DEFER" Arcane/Core/src` still exactly the 6 sites (P6 burns none — they are P4/P5's).
- [ ] **Step 2: Full suite.** Build; `.\ArcaneTests.exe ~[gpu]` green; record counts + wall-time vs the P2-exit reference (112050/492, ~11 min).
- [ ] **Step 3: THE restoration measurement.** Run each SandboxSmoke half isolated with `--durations yes` (names need `\,` escapes) and record the durations. P2-exit reference: **d3d12 = 5066 s**. Expected: **two orders of magnitude drop** (meter bodies occupy ~1 residency tile instead of 400-1600). If a half still exceeds ~5 min, that is a FINDING (the residency hypothesis was wrong or incomplete) — profile before pushing, do not shrug.
- [ ] **Step 4: Loom GPU-verify + D1 escape-stress re-confirm.** `Loom.exe --frames 180` AND `--backend vulkan --frames 180` — clean exit 0, RenderErrorCount 0 (spec P6 exit). D1: run the stress stability case (`SandboxVisualsTest` stress bounds, 1200 bodies from a ~40 m column) — green at meter scale re-confirms the isfinite/SaneBox guards against the original escape-crash shape; record it as D1-closed in the report.
- [ ] **Step 5: Spec exit-criteria sweep (spec section 6, P6 line):** piles settle AND sleep (Box stack / Pyramid scenes — verify via the HudTest settle evidence from Task 1); CCD scene demonstrates tunneling-prevention (bullet stops at the wall — SandboxSmoke covers the render; assert via a quick manual `Loom` check or the scene's smoke pass); drag/throw feel restored (Task 2's converged drag test is the proxy; note that FEEL sign-off is the USER's, in interactive Loom).
- [ ] **Step 6: CLAUDE.md.** Insert after line 216 (the `/fp:fast` bullet, per survey section 12): `- **Units are MKS (meters/kg/seconds).** Physics content is authored in meters (bodies 0.1-10 m, g=10); the sandbox camera maps world->screen at pixelsPerMeter=100. Never author px-scale content; deferred px constants are tagged MKS-DEFER(Pn).` Commit separately: `docs: CLAUDE.md gains the MKS convention line (MKS P6)`.
- [ ] **Step 7: Exit report + push.** Consolidated justification table (every re-derive across T1-T3), burn-down number, suite counts, the smoke-duration table (before/after), D1 note, carry-forwards for P3 (sleep/settle cluster: the probe numbers in the Appendix below; the two 0.05 warm-start drift bounds flagged by the P2 final review), P4 (CC retunes + kShapeCastTol + [mks] blind-spot note), P5 (MouseJoint maxForce; CCD re-arm; **full-suite wall-time restoration assert moves here** — P5 is now the last phase). `git push -u origin feature/arcane-physics-mks-phase6`, then STOP — review + merge = USER's call.

---

## Appendix A: MKS probe battery numbers (2026-07-03, archived here for P3 planning — source `.superpowers/sdd/mks-probe-report.md` is gitignored)

ALL 5 PROBES PASS — the P3 contingency (sleep threshold) does NOT trigger:
- **A pile-sleep@0.05:** 5 boxes + 3 circles ALL ASLEEP by step 338, zero re-wake, maxV/maxW == 0 by step 600.
- **B 8-box stack:** slept step 57, drift 0, top-box position error 0.010 m.
- **C CCD@150 m/s:** bullet clamps x=9.93 (wall face 9.98); kinematic baseline tunnels; 400-cap untouched.
- **D restitution 0.6:** apex 0.677 m (analytic e^2*h = 0.72), sleeps step 153.
- **E free-fall:** 50.0 m/s after 5 s (8x under the 400 cap).
- Authoring gotcha for all phases: dynamic `MakeAabb` hard-asserts `fixedRotation` (PhysicsWorld.cpp:981) — rotating rectangles use `MakePolygon`.

## Appendix B: Phase order amendment record

USER decision 2026-07-07: P6 executes before P3-P5 (rationale: kills the px-content residency-tile smoke pathology — measured 5066 s/backend at P2 exit — making P4/P5 gates cheap; visible payoff sooner; D1 folds in here). Survey correction that de-risked the reorder: sandbox drag is a hand-rolled bounded impulse (`Interaction.cpp:196-280`), NOT an engine MouseJoint — the P5 `maxForce 1e9` rescale cannot affect sandbox behavior. The spec's P6 exit assert "full ~[gpu] wall-time restores" is RE-SCOPED to the smoke-pair/[sandbox] measurement (Task 4 Step 3); the full-suite restoration assert belongs to the last phase (P5).

## Self-Review (author checklist — completed)

- **Spec coverage:** spec lines 128-141 -> T1 (scenes, g=10), T2 (camera ppm=100 + SandboxCameraTest new form + drag 40/400 + pick screen-px + bullet 70 in T1), T3 (HUD 0-30 / 0.04-0.8 / tick 0.18 / COM 0.05), T4 (CLAUDE.md line, Loom exit, piles-sleep, CCD-demonstrates, feel-restored); interim tests properly rewritten -> T1 (HudTest settle) + T2 (drag); last sandbox PX-PINs -> T1+T2; spec section 6 P6 exit -> T4 Steps 3-5.
- **Placeholder scan:** every conversion carries exact old -> new values from the survey; the only execution-derived numbers are empirical re-baselines, procedurally defined per protocol rule 6 (designed mechanism, P2 precedent).
- **Type consistency:** `kPixelsPerMeter` name used identically in T2 Steps 1/3/4; `kPickRadiusPx` in T2 Steps 4/5; `kGravityY` stays a named constant at both definition sites.
