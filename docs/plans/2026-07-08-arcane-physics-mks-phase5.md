# Arcane MKS Units — Phase 5 (CCD + VelocityClamp + Joints + Debug-Viz + Wall-Time — THE LAST MKS PHASE) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Burn the last 69 PX-PINs and the last 3 MKS-DEFER markers to ZERO — convert the three remaining test clusters (`PhysicsCcdTest` 29, `PhysicsJointsTest` 22, `PhysicsVelocityClampTest` 18) to MKS, rescale the MouseJoint `maxForce` trio off its px-era `1e9` sentinel, fix the P4-review velocity-ray gate residual, and formalize the full-suite wall-time restoration — completing the MKS units conversion (P1+P2+P3+P4+P6 are already on main).

**Architecture:** Spec §4 P5 (`docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md`) + the accumulated P4-exit carry-forwards. Three test-only clusters + two small ENGINE edits (the MouseJoint `maxForce` trio in `Joints/`, task-scoped with its cluster per the P4 engine-flip lesson; and the debug-draw velocity-ray gate options-field). Fact base: three grep-verified survey docs read full-file against main @b96e765e — `.superpowers/sdd/p5-survey-A-ccd.md`, `p5-survey-B-joints.md`, `p5-survey-C-clamp-gate-walltime.md`. Task briefs point implementers at their survey sections for the complete literal inventories; this plan carries the binding decisions and every value that is NOT a mechanical divide. **The surveys caught three errata (documented per-task below) — implementers verify-not-trust and report any further contradictions.**

**Tech Stack:** C++23 (Arcane Core/Physics/Joints + Arcane DLL Render/PhysicsDebugDraw + Tests), MSVC via msbuild, Catch2. Parity source: vendored `ThirdParty/box2d-3.1.1` — verify invariants against it directly (file:line), never recall.

## Global Constraints

- **Conversion protocol:** `.superpowers/sdd/p2-conversion-protocol.md` applies verbatim (PX-PIN deletion, Rule-3 impulses = mass·Δv with both factors NAMED in-test per the JointSleep precedent, empirical re-baseline = measure → generous headroom or named-constant floor → justify, ASCII-only, explicit-path staging, FOREGROUND-one-command builds/tests, commit trailers). Overrides: branch `feature/arcane-physics-mks-phase5` off main @b96e765e; `[physics]` base = **30638 assertions / 278 cases**; full `~[gpu]` reference = **112060 / 495 @~96 s**.
- **Spec §6 contingency (binding):** behavioral asserts (tunneling gates `p.x < kWallX`, clamp bounds `speed ≈ 400`, joint-hold tolerances, run-twice byte-identity, joint-count/validity, `RenderErrorCount()==0`) pass UNMODIFIED under a re-derived scene. A behavioral failure at MKS content = STOP, diff vs vendored Box2D, report BLOCKED. Never weaken a gate to make it pass.
- **Engine edits are task-scoped:** exactly TWO engine-edit clusters are authorized — the MouseJoint `maxForce` trio (Task 3, three sites in `Joints/`) and the velocity-ray gate options-field (Task 4, `PhysicsDebugDraw.hpp`+`.cpp`). Any other engine/Core/Sandbox diff is a defect. **MKS-DEFER burn-down: 3 → 0** (the trio is the last of them; `grep -rn "MKS-DEFER" Arcane` must return EMPTY at Task 6).
- **PX-PIN burn-down: 69 → 0.** `grep -rc "PX-PIN" Arcane/Tests/src/*.cpp | grep -v ':0'` must return EMPTY at Task 6. Zero-g scenes keep their `gravityX/Y = Real(0)` lines with the PX-PIN marker STRIPPED (deliberate scene statements, not deletions) — the grep counts markers, not lines.
- **Survey-derived cross-cutting rules** (in addition to the protocol's):
  1. **Two pin-grep-INVISIBLE px values exist** (the recurring P3/P4 lesson) and MUST be handled despite carrying no PX-PIN marker: (a) `PhysicsCcdTest` case 4's MISSING `gravityY` pin — a naive delete-pass leaves it at the MKS default `+10` while its comment says "gravity 0"; must ADD `wd.gravityY = Real(0);` (survey A ERRATA E1). (b) `PhysicsJointsTest` `GravityWorld()`'s unmarked `gravityY = Real(400)` (line 79) — a px-era gravity affecting 7 of 10 cases; DELETE it to inherit the MKS default `+10` (these are gravity-driven settling scenes; survey B ERRATA 1).
  2. **The Rule-3 dimensional split is the single most important arithmetic fact for Task 3:** under a `/10` length rescale (density + dt fixed), **linear force/impulse scales `/1000`** (mass `/100` × velocity/accel `/10`) but **torque scales `/10000`** (inertia `/10000` × angular-accel `/1` — rad/s² is length-independent). Applying one factor to both DESYNCS the motor tests. Motor rate/margin bounds (rad/s) are dimensionless-keep and may stay bit-identical because `angularAccel = torque/I` and both scale `/10000` (verify empirically, do not assume).
  3. **VelocityClamp is a DELETE-ONLY conversion.** Its velocity literals (2000/100/1000) are cap-relative, NOT px-scaled — dividing them would drop case 1 under the unchanged 400 cap and defeat the test. Touch only the 18 PX-PIN `WorldDef` lines.
  4. Dimensionless-keep (never rescale): unit-vector axes, `frequencyHz` (Hz), `dampingRatio` (ζ), all `motorSpeed`/`targetSpeed` (rad/s), all bare-angle tolerances (radians), the `length = Real(-1)` "current separation" sentinel, and Approx fractions `t ∈ [0,1]`.
  5. The 400 m/s cap stays 400 (already MKS-honest — `WorldDef::maxLinearVelocity`, PhysicsWorld.hpp:219, = Box2D types.c:21). `kShapeCastTol`/`kSkin`/`kMargin`/`kLinearSlop`/`sleepThreshold` are already MKS (P1–P4). The CCD engine surface needs NO change (survey A §3).
  6. Plan-authoring rule (P3 lesson, binding on this doc's re-measure steps): re-measure steps state the PROCEDURE, never the expected value.
- **Suite green at every commit.** Per-task gates = file tags + `"[physics]"`. Full `~[gpu]`, the SandboxSmoke pair, and Loom run at Task 6 (engine WAS touched this phase — the smoke pair is mandatory).
- **Build:** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m` — ONE FOREGROUND command. Task 5 adds ONE new .cpp → its Step 1 regenerates projects (`ThirdParty\premake5\premake5.exe vs2026` directly — NOT `GenerateProjects.bat`, which hangs on `pause`).
- **Test invocation:** `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "<tags>"` — ONE FOREGROUND command (a backgrounded test stalls and kills the session). Run from the exe's own directory.
- **Commit hygiene:** explicit paths only (NEVER `git add -A` — the working tree carries parked `Client/data` + untracked `AGENTS.md`/`box2d`/etc. noise); commit messages via `[System.IO.File]::WriteAllText` + `git commit -F <file>` (a pipe/here-string puts a BOM in the subject); do NOT `Remove-Item` the temp message file (the guard blocks the whole compound command); use `git diff main HEAD` (not `git diff main`).
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466
  ```

## File Structure

| File | Change | Task |
|---|---|---|
| `Arcane/Tests/src/PhysicsVelocityClampTest.cpp` | delete-only (18 pins → 6 bare zero-g lines + 12 deletions) | 1 |
| `Arcane/Tests/src/PhysicsCcdTest.cpp` | /10 geometry + re-arm v=300 m/s + H1 inset fix + case-4 gravityY; 29 pins | 2 |
| `Arcane/Tests/src/PhysicsJointsTest.cpp` | /10 geometry + Rule-3 (/1000 & /10000) + GravityWorld gravityY; 22 pins | 3 |
| `Arcane/Core/src/Arcane/Physics/Joints/Joint.hpp` | **ENGINE:** `maxForce` 1e9 → 1e6 (site 1) + delete DEFER | 3 |
| `Arcane/Core/src/Arcane/Physics/Joints/Joints.hpp` | **ENGINE:** `m_maxForce` 1e9 → 1e6 (site 2) + delete DEFER | 3 |
| `Arcane/Core/src/Arcane/Physics/Joints/Joints.cpp` | **ENGINE:** ternary fallback 1e9 → 1e6 (site 3) + delete DEFER | 3 |
| `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp` | **ENGINE:** add `velocityRayMinSpeed = 0.05f` options field | 4 |
| `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp` | **ENGINE:** gate `spd > 1.0f` → `spd > opts.velocityRayMinSpeed` | 4 |
| `Arcane/Tests/src/SandboxSmokeTest.cpp` | camera framing tripwire `CHECK(rt.CameraZoom() == Approx(100.0f))` | 4 |
| `Arcane/Tests/src/PhysicsPerfTripwireTest.cpp` | **NEW:** `[perf][physics]` wall-time tripwire | 5 |
| `Arcane/premake5.lua` (verify only) | new .cpp auto-globbed? confirm at Task 5 Step 1 | 5 |

---

### Task 1: Branch, baseline, and VelocityClamp (delete-only warm-up)

**Files:** `Arcane/Tests/src/PhysicsVelocityClampTest.cpp` (18 pins). Add (commit only): this plan doc.

**Survey sections:** C §Item-1 (full inventory + the delete-only verdict + the "keep 2000/100/1000" cap-relative analysis + ERRATA on the false 82a8ab99 claim).

**Why first:** it is the simplest cluster (pure marker deletion, no content re-derivation), so it establishes the branch and the recorded baseline counts cleanly before the arithmetic-heavy tasks.

- [ ] **Step 1: Branch + baseline.** `git checkout -b feature/arcane-physics-mks-phase5` off main @b96e765e. Stage + commit THIS plan doc (explicit path). Build (the one FOREGROUND msbuild command). Record, by RUNNING each once (state the procedure, not a primed number): `ArcaneTests.exe "[physics]"` assertion/case counts; `ArcaneTests.exe "[clamp]"` counts; and the PX-PIN total via `grep -rc "PX-PIN" Arcane/Tests/src/*.cpp | grep -v ':0'` (expect three files summing to 69). If `[physics]` is not 30638/278, STOP and reconcile before proceeding — the base moved since the P4 exit.

- [ ] **Step 2: Delete-only conversion.** In all three `WorldDef` blocks (lines 14-19, 35-40, 55-60), delete the 4 non-gravity PX-PIN lines each (`sleepThreshold = Real(8)`, `restitutionThreshold = Real(20)`, `contactPushMaxVelocity = Real(300)`, `hashCellSize = Real(64)` — 12 lines total; the cases never touch sleep/restitution/contact-push/broadphase, so they inherit MKS defaults 0.05/1.0/3.0/1.0). For the 6 `gravityX/gravityY = Real(0)` lines, STRIP the `// PX-PIN: ...` marker but KEEP the line (deliberate zero-g: the test isolates the clamp from gravity's per-step velocity contribution, which would otherwise perturb case 1's `±0.5` bound). Add a one-line comment above the first surviving block, mirroring the JointSleep idiom (PhysicsJointSleepTest.cpp:51-52): `// zero-g isolates the clamp from gravity; other WorldDef fields inherit MKS defaults.` Touch NO velocity/shape/tolerance literal — `SetVelocity(2000)`, `SetVelocity(100)`, `SetAngularVelocity(1000)`, `MakeCircle(4)`, the `400 ± 0.5` bounds, and the `0.001`/`0.01` tolerances all stay verbatim (cross-cutting rule 3).

- [ ] **Step 3: Gate.** Build (one FOREGROUND command). `ArcaneTests.exe "[clamp]"` green (same counts as Step 1 — delete-only changes no assertion). `ArcaneTests.exe "[physics]"` reconciled to 30638/278 (zero delta expected — no assertion added or removed). `grep -c "PX-PIN" Arcane/Tests/src/PhysicsVelocityClampTest.cpp` == 0.

- [ ] **Step 4: Commit** (explicit paths: the test file + this plan doc, or plan doc in Step 1's commit and the test alone here). Message:
  ```
  test(arcane/physics): velocity-clamp test -> MKS (delete-only) (MKS P5)
  ```
  Body: note the delete-only shape (velocities are cap-relative, kept verbatim; only the 18 WorldDef markers removed, 6 zero-g lines survive); cite survey C §Item-1; record that the P4-carry "82a8ab99 retune" claim was survey-verified FALSE (the file has only ever been touched by 29b07395 authoring + 13cb2cf1 pinning — no content retune to preserve).

---

### Task 2: CCD cluster (re-arm to 300 m/s)

**Files:** `Arcane/Tests/src/PhysicsCcdTest.cpp` (29 pins). No engine change (survey A §3: the CCD surface is already fully MKS).

**Survey sections:** A §1 (full inventory), §2 (the re-arm analysis + per-case §2.3 table — the complete literal mapping), §4 (hazards H1–H7).

**Binding decisions (this plan; the survey's §2.3 table is the complete per-line mapping the implementer follows):**
- **Speed re-arm: v = 300 m/s** (family 150–300; probe C validated 150 m/s vs a thin meter wall). `kSpeed` is authored as displacement/dt: change line 78 from `Real(200) / kStep` to `Real(5) / kStep` (5 m/step × 60 = 300 m/s). Rationale (survey §2.1–2.2): mechanical `/10` gives 1200 m/s which is 3× OVER the 400 cap — it would silently cap-contaminate the dynamic cases and run the kinematic cases uncapped at 1200 (a speed-split + an authoring-rule violation). At v=300 with `/10` geometry: per-full-step 5 m vs 0.6 m miss window = 8.3× (kinematics); per-substep 1.25 m = 2.08× (dynamics, h=1/240) — both genuinely tunnel without CCD.
- **Geometry: mechanical /10** (Option A — keep `kWallX` unchanged, move the one-step start closer): `kWallX 100→10`, `kWallHW 1→0.1`, `kWallHH 50→5`, `kProbeR 2→0.2`. For the four one-step cases (1–4) move the start x from `0 → 6` (gap-to-contact = 9.9 − 0.2 − 6 = 3.7 m < the 5 m velocity-scaled discovery pad, so the speculative contact exists in the crossing step; unobstructed one-step end 6+5 = 11 clears the far face+r 10.3 by 0.7 m). Case 5 (determinism) KEEPS start x=0 with its 8-step loop (movers reach the wall on step 2) and rescales its y-lanes `-10/0/+10 → -1/0/+1` (inside the wall's y[−5,5] band).
- **H1 — the biggest hazard (A7 vacuous-assert class):** the TWO length insets in `CenterInsideWall` (lines 90–91, `kWallX ± kWallHW ∓ Real(0.5)`) MUST scale to `0.05` in lockstep with `kWallHW`. If left at 0.5 while `kWallHW`→0.1, the window `cx > 10.4 && cx < 9.6` inverts and all three `REQUIRE_FALSE(CenterInsideWall(...))` (lines 129/202/234) pass VACUOUSLY — silent loss of the "not buried in the wall" coverage. The line-89 `Real(0.5)` (the center-average `(min+max)*0.5`) is dimensionless — DO NOT touch it.
- **H3 — stale-numeric-cite:** case 2's landing assert (line 164) `Approx(Real(200))` is the SAME 200 as `kSpeed`'s derivation, NOT an independent constant. Re-derive from the scene: `startX + kSpeed*kStep = 6 + 5 = 11` → `Approx(Real(11))`. The `margin(Real(0.01))` stays (kinematic integrate is exact; 0.01 is generous absolute).
- **Case 4 gravityY (survey ERRATA E1 — pin-grep-invisible):** case 4's WorldDef has 5 pins, not 6 — its `gravityY` pin has been MISSING since P1 (commit 13cb2cf1), so case 4 has silently run at gravity `(0,+10)` despite its "gravity 0" comment. When converting, ADD `wd.gravityY = Real(0);` deliberately (marker-free zero-g survivor) so all five cases are genuine zero-g.
- Case 3's clamp assert (line 200): target `Approx(kWallX - kWallHW - kProbeR)` is self-derived from the constants (= 9.7, scale-covariant, correct after /10); its `margin(Real(0.5))` re-derives to `Real(0.05)` (the standoff at v=300 is ~0.011 m: `kBulletEpsilon`-pullback 0.001·5 m + `kShapeCastTol` 0.00625; 0.05 gives ~5× headroom and is the mechanical /10 anyway).

- [ ] **Step 1: Convert the shared helpers + constants.** Per survey A §1.1: `kWallX/kWallHW/kWallHH/kProbeR` /10; `kSpeed` → `Real(5) / kStep`; `CenterInsideWall` insets 0.5 → 0.05 (lines 90–91 ONLY; line-89 average untouched). Recompute the derivation COMMENTS (survey §4 H4: header lines 3–12, the `kSpeed`/wall/inset comment cites at 54–58/73–78/89–91) to the new scene — recompute, do not search-replace ("~12000 u/s" → "300 m/s", "displacement is 200 units" → "5 m/step", "2-wide wall" → "0.2 m wall", etc.).

- [ ] **Step 2: Convert the five cases.** Follow survey §2.3's per-case table: start positions (cases 1–4 → x=6; case 5 x=0, y-lanes /10), case 2's `Approx` → `Real(11)`, case 3's clamp margin → `0.05`, case 4's ADDED `gravityY = Real(0)`. Delete every remaining PX-PIN marker; strip-and-keep the zero-g `gravityX/gravityY` lines. Density `Real(1)` literals (117/225/271/278) are unit-free — unchanged. The relational tunneling gates (`p.x < kWallX` / `p.x > kWallX`, lines 127/165/199/233) and the run-twice byte-identity asserts (301/304-305) pass UNMODIFIED (protocol rule 6).

- [ ] **Step 3: Gate.** Build (FOREGROUND). `ArcaneTests.exe "[ccd]"` green. `ArcaneTests.exe "[physics]"` reconciled (zero delta expected — no assertion added/removed). `grep -c "PX-PIN" Arcane/Tests/src/PhysicsCcdTest.cpp` == 0. If a tunneling gate FAILS at MKS content, STOP — that is a spec-§6 contingency (diff the re-armed speed/geometry arithmetic against survey §2.2 before touching any engine constant; the CCD surface is out of bounds this task).

- [ ] **Step 4: Commit** (explicit path). Message:
  ```
  test(arcane/physics): ccd tests -> MKS, re-armed v=300 m/s (MKS P5)
  ```
  Body: the re-arm rationale (mechanical /10 → 1200 m/s over the cap; v=300 keeps tunneling genuine at 8.3× full-step / 2.08× per-substep with headroom); the H1 vacuous-assert fix (insets 0.5 → 0.05); the case-4 gravityY restoration (survey ERRATA E1, silently un-pinned since P1); case 3's clamp standoff arithmetic. Cite survey A §2.

---

### Task 3: Joints cluster + MouseJoint maxForce trio (ENGINE)

**Files:** `Arcane/Tests/src/PhysicsJointsTest.cpp` (22 pins); `Arcane/Core/src/Arcane/Physics/Joints/Joint.hpp` (site 1), `Joints.hpp` (site 2), `Joints.cpp` (site 3) — the three `maxForce = Real(1e9)` MKS-DEFER sites.

**Survey sections:** B §1 (full inventory + §1d the Rule-3 classification table), §2 (the trio + Box2D parity fact + §2f the MKS-honest-default analysis), §3 (blast radius), §4 (hazards + the JointSleep precedent), ERRATA (GravityWorld gravityY; the false sandbox-mouse-joint premise).

**Binding decisions:**
- **The /1000 vs /10000 Rule-3 split** (cross-cutting rule 2 — survey §1d): under `/10` length (all bodies `density=1`), linear force/impulse `/1000`, torque `/10000`. The five load-bearing literals:
  | Line | Old | New | Kind |
  |---|---|---|---|
  | 259 | `ApplyImpulse(slider, Vec2(Real(2000), 0))` | ~`2.0 N·s` | linear impulse /1000 |
  | 175 | `jd.maxForce = Real(8000)` (TEST content, NOT the engine default) | `8 N` | linear force /1000 |
  | 306 | `jd.maxMotorTorque = Real(1e6)` | `100 N·m` | torque /10000 |
  | 398 | `jd.maxMotorTorque = Real(1e6)` | `100 N·m` | torque /10000 |
  | 354 | `jd.maxMotorTorque = Real(1)` | `0.0001 N·m` | torque /10000 |
  Author the impulse (line 259) as **named `mass × Δv`**, mirroring the JointSleep precedent (PhysicsJointSleepTest.cpp:148-150) rather than a bare scaled magic number: e.g. `const Real sliderMass = Real(1) * kPi * Real(0.5) * Real(0.5); const Real pushDv = Real(2.5); w.ApplyImpulse(slider, Vec2(sliderMass * pushDv, Real(0)));` (≈ 1.96 N·s), with a comment naming both factors and why (`pushDv` well under the 400 cap; slider must cross x>4.5 m). Verify `kPi` is in scope (it is in PhysicsJointSleepTest — confirm the same include/using in this file; add if missing).
- **Motor rate bounds stay bit-identical (verify, don't assume):** case 6 `Approx(targetSpeed).margin(0.5)` and case 7 `CHECK(rate < Real(2))` are rad/s — dimensionless-keep. Because `angularAccel = torque/I` and BOTH scale `/10000`, the measured rate is numerically unchanged if the torque literal is rescaled by exactly `/10000`. Keep 0.5 and 2 as-is, then CONFIRM by running (protocol rule 6). If the run disagrees, re-baseline with headroom and note it.
- **GravityWorld gravityY (survey ERRATA 1 — pin-grep-invisible):** line 79's unmarked `wd.gravityY = Real(400)` is px-era gravity feeding 7 of 10 cases. DELETE the line to inherit the MKS default `+10` (these are gravity-driven settling scenes — pendulum, distance, weld, prismatic, wheel — that need real downward gravity, and +10 is the same y-down direction). Add a one-line comment on the surviving struct noting gravity inherits the MKS default.
- **Settle-time re-baselines are EMPIRICAL (survey §1c step-count note):** the step counts (900/600/240/600/300/120/600) are damped-constraint SETTLING times, NOT free-fall — do NOT apply protocol rule 5's `sqrt(2h/g)` formula. Re-verify each by RUNNING the converted content and checking the settle assertion clears with headroom; adjust the loop count only if forced, and reconcile the arithmetic in the commit body.
- **maxMotorTorque is NOT part of the trio** (survey §4): its `JointDef` default is `Real(0)` (motor disabled) — no sentinel, no DEFER. The 1e6/1/1e6 above are TEST content (file scope, /10000), distinct from the engine `maxForce` default sites.

**ENGINE — the MouseJoint maxForce trio (sites 1/2/3):**
- **Decision: `Real(1e9)` → `Real(1e6)` at all three sites**, delete the three `// MKS-DEFER(P5): ...` markers. Rationale (survey §2e/§2f/§3): the trio is one logical default duplicated three times and is DEAD in production — the ONLY `JointKind::Mouse` construction anywhere (`PhysicsJointsTest.cpp:175`) sets `maxForce` explicitly, and Sandbox drag is a separate hand-rolled bounded-impulse mechanism that never constructs a MouseJoint (survey ERRATA 3 corrects the brief's premise). Zero blast radius. `1e6 N` is Box2D's own drag-sample convention `1000·mass·g` (`ThirdParty/box2d-3.1.1/samples/sample.cpp:338`) evaluated at the heaviest in-range body (100 kg × g=10 × 1000 = 1e6 N), so it is Box2D-derived AND stays "effectively unclamped" for all in-range content (typical spring forces ~1e2–1e5 N sit well under it) — preserving the current usable-out-of-the-box semantics with an MKS-honest number instead of a px-era `1e9`.
- **Documented alternative (for merge-review veto, not implemented):** match Box2D's struct default `b2DefaultMouseJointDef().maxForce = 1.0f` (`joint.c:43-51`) — a deliberately-unusable placeholder that forces every caller to compute its own `1000·mass·g`. More faithful to Box2D's API philosophy but a behavior change (a future default-relying caller gets a ~1 N clamp, unable to hold a 0.785 kg body's 7.85 N weight). Rejected here as higher forward-risk for zero current benefit; recorded so the user can override at review.

- [ ] **Step 1 (ENGINE): rescale the trio.** Edit the three sites to `Real(1e6)` and delete each `// MKS-DEFER(P5): ...` marker, replacing with a real doc comment at site 1 (`Joint.hpp:100`):
  ```cpp
  // Mouse: target + force clamp. Default is an MKS-honest "effectively unclamped"
  // value = Box2D's drag-sample convention 1000*mass*g (samples/sample.cpp:338)
  // at the heaviest in-range body (~100 kg, g=10). Real callers set this per-body;
  // the only in-repo MouseJoint (PhysicsJointsTest) overrides it explicitly.
  Real maxForce = Real(1e6);
  ```
  Sites 2 (`Joints.hpp:166`, the `m_maxForce` member — dead default under MouseJoint's single explicit-arg ctor) and 3 (`Joints.cpp:458`, the `def.maxForce > Real(0) ? ... : Real(1e6)` fallback) get the same `1e6` and a one-line comment referencing site 1. Verify `grep -rn "MKS-DEFER" Arcane` returns EMPTY. Verify `grep -rn "1e9" Arcane/Core/src/Arcane/Physics/Joints` returns EMPTY.

- [ ] **Step 2: Convert the test file.** Per survey §1c/§1d: /10 all scene geometry (anchors/positions/lengths/radii — the §1c table), Rule-3 the five force/impulse/torque literals per the split above (impulse authored as named mass×Δv), DELETE the unmarked `gravityY=400` (inherit +10), delete all 22 PX-PIN markers (strip-and-keep the zero-g `gravityX/gravityY` lines in the motor cases 6/7 and the mouse case 3), and convert every tolerance that is a LENGTH (position/separation/travel tolerances — `<3`, `<2`, `<4`, `<8`, `>45`, `>28`, `<120`, `>0.5` travel; /10) while keeping every RADIAN tolerance (`<0.1`, `>0.5` angle) and every rad/s bound verbatim. The mouse target (line ~168 `Vec2(540,60)`) and chip position /10.

- [ ] **Step 3: Re-baseline settle times.** Build (FOREGROUND). Run `ArcaneTests.exe "[joints]"`. For any case whose settle assertion fails, re-derive the loop count empirically (measure the actual settle step, add headroom) — state the procedure; do not pre-fill an expected count. Confirm cases 6/7's rad/s bounds held bit-identical (or re-baseline + note). Re-run to green.

- [ ] **Step 4: Gate.** `ArcaneTests.exe "[joints]"` green; `ArcaneTests.exe "[physics]"` reconciled (any delta = a re-baselined loop count that changed assertion counts? — none expected, a loop-count change does not add asserts; if `[physics]` moved, explain). `grep -c "PX-PIN" Arcane/Tests/src/PhysicsJointsTest.cpp` == 0. `grep -rn "MKS-DEFER" Arcane` EMPTY. A joint-hold behavioral failure at MKS = spec-§6 STOP (diff the joint solver vs vendored Box2D, do not weaken the hold tolerance).

- [ ] **Step 5: Commit** (explicit paths: the test + the three Joints engine files). Message:
  ```
  feat(arcane/physics): mouse-joint maxForce -> 1e6 (MKS) + joints tests -> MKS (MKS P5)
  ```
  Body: the /1000-vs-/10000 Rule-3 split table; the trio decision (1e6 = Box2D 1000·mass·g at 100 kg, dead-in-production, zero blast radius) + the documented 1.0f alternative; the GravityWorld gravityY fix (survey ERRATA 1); the motor-rate bit-identity finding (verified); any re-baselined settle count. Note MKS-DEFER is now ZERO.

---

### Task 4: Debug-viz correctness — velocity-ray gate (ENGINE) + camera framing tripwire

**Files:** `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp` (+`.cpp`), `Arcane/Tests/src/SandboxSmokeTest.cpp`.

**Survey sections:** C §Item-2 (the gate: raw literal at .cpp:401, `sleepThreshold` accessible, options-field-vs-raw-retune footprints, the P4 options-struct precedent), §Item-4 (the camera tripwire: `Runtime::CameraZoom()` passthrough, the smoke harness reachability).

**Background:** P4's final review flagged (bookkeeping-only carry-forward, commit b96e765e) that the velocity-ray suppression gate `spd > 1.0f` (PhysicsDebugDraw.cpp:401) is an un-flipped px-era magnitude: px relation was 1 vs sleepThreshold 8 (gate ≪ sleep, so awake bodies almost always drew a ray); MKS relation is 1.0 vs 0.05 (gate ≫ sleep, INVERTED 160×), so awake bodies moving 0.05–1.0 m/s draw no ray. Cosmetic (no test reads it — survey §Item-2 confirms), but the last px-proportioned magnitude in the render path.

**Decision: options-field plumb** (matches the P4 precedent shape — `contactMarkerSize`/`comMarkerSize`/`orientationTickLen`/`velocityScale` are all `PhysicsDebugDrawOptions` fields with doc comments; the "address addressable gaps properly" principle argues for the real field over a bare `.cpp` literal). A `float velocityRayMinSpeed = 0.05f`, defaulting to `SleepThresholdDefault()`, gives the HUD a knob and self-documents the sleep-scale relation. (Raw retune `1.0f → 0.05f` is the 1-line alternative; rejected for the same reason P4 used fields.)

- [ ] **Step 1 (ENGINE): add the options field.** In `PhysicsDebugDraw.hpp`, insert after `velocityScale` (line 80, inside `PhysicsDebugDrawOptions`):
  ```cpp
  // Minimum world-space speed (m/s) for a body to draw a velocity ray; below
  // this the ray is suppressed as jitter. Defaults to the MKS sleep threshold
  // (WorldDef sleepThreshold default 0.05 m/s) so any body the solver considers
  // awake shows a ray -- restoring the px-era gate<<sleepThreshold relation that
  // the units flip had inverted (old raw literal was 1.0 world-u/s).
  float velocityRayMinSpeed = 0.05f;
  ```
- [ ] **Step 2 (ENGINE): read the field at the gate.** In `PhysicsDebugDraw.cpp:401`, change `if (spd > 1.0f)   // world units/s threshold -> suppress jitter` to `if (spd > opts.velocityRayMinSpeed)`. `opts` is in scope (the function's `PhysicsDebugDrawOptions` parameter). Verify no other `1.0f` velocity-magnitude gate exists (survey §Item-2: line 401 is the only one of its class; 227/235 are radius guards, 606 is a dimensionless emphasis scale — leave them).

- [ ] **Step 3: Camera framing tripwire.** In `SandboxSmokeTest.cpp`, after the first `StepAndRender(rt, host, *device, *canvas, *batcher, 30)` (line 130, where the plugin has pushed its camera through the real `Sandbox.dll` `Update` → `Runtime::SetCamera`), add:
  ```cpp
  // Framing tripwire (P6 residual): the plugin must push the COMBINED world->screen
  // scale (Camera::WorldToScreenScale() = kPixelsPerMeter * zoom = 100 at the default
  // scene), NOT the raw zoom (1.0). The P6 regression pushed raw zoom -> a 1 px/m
  // render that RenderErrorCount()==0 could not see. CameraZoom() reads back the
  // actual cross-DLL value the plugin pushed.
  CHECK(rt.CameraZoom() == Catch::Approx(100.0f));
  ```
  (This is a `[sandbox][gpu]` case — the tripwire runs on the GPU agent where the regression lived. `rt` is already in scope. Confirm `Catch::Approx` is available via the test's existing Catch2 include; the file uses `catch_test_macros.hpp` — add `#include <catch2/catch_approx.hpp>` if `Approx` is not already visible.)

- [ ] **Step 4: Gate.** Build (FOREGROUND — the DLL rebuild is the engine edit; the smoke test is `[gpu]`). Run `ArcaneTests.exe "[physics]"` green (the gate change is render-only, no `[physics]` assertion touched — zero delta). The `[gpu]` smoke pair + the camera CHECK are verified at Task 6 (they need the GPU agent + serialize on the DLL copy lock). Confirm the engine edit compiles and the field default reads correctly by a quick `ArcaneTests.exe "[render]"` if that tag covers PhysicsDebugDraw unit tests (survey notes `[render]` is broader than the touched files — a green run is sufficient, no count change expected).

- [ ] **Step 5: Commit** (explicit paths: the two Render files + the test). Message:
  ```
  fix(arcane/render): velocity-ray gate -> sleepThreshold-scale options field (MKS P5)
  ```
  Body: the inverted-relation explanation (1.0 vs 0.05, 160×); options-field-over-raw-literal rationale (P4 precedent); the camera framing tripwire (would have caught the P6 raw-zoom regression via `CameraZoom()` readback). Note the two RECORDED-not-fixed residuals carried forward unchanged (DebugDrawTest pixel-readback gap; DebugCapsule silent-shrink) — out of P5 scope.

---

### Task 5: Full-suite wall-time formal tripwire

**Files:** NEW `Arcane/Tests/src/PhysicsPerfTripwireTest.cpp` (`[perf][physics]`).

**Survey sections:** C §Item-3 (no in-repo wall-time-assert precedent; the four mechanisms + their footprints; the JUnit `time` attributes; the false "SpatialGrid budget = time budget" reading).

**Background & decision:** the spec/P4-carry obligates "formalize the full-suite wall-time restoration" (px-era ~1087 s → ~96 s). Survey §Item-3 confirms there is NO active wall-time assert anywhere and that a naive full-suite wall-clock assert is (a) unmeasurable from inside one test and (b) flake-prone in a determinism-prized suite. **Decision: a targeted `[perf]`-tagged, EXCLUDABLE tripwire with order-of-magnitude headroom** — it reconstructs a representative dense-broadphase many-body step workload (the shape that was pathologically slow at px-scale, when px content lived inside the 1 m residency tile) and asserts the step loop completes under a GENEROUS fixed ceiling. Huge headroom (aim ~10× the healthy time) means it only ever catches an order-of-magnitude pathology REGRESSION, never 2× machine variance; the `[perf]` tag lets loaded/CI-variable machines exclude it (`~[perf]`, exactly like `~[gpu]`). The documented full-suite number (mechanism 4 — the de-facto practice, tracked in every phase's exit report) is formalized alongside in Task 6's report. **Documented alternative (merge-review veto):** drop the new test and rely on the documented-number gate alone, keeping wall-clock asserts out of the deterministic suite entirely — recorded so the user can choose the lower-footprint path.

- [ ] **Step 1: Create the file + regenerate projects.** Write `PhysicsPerfTripwireTest.cpp` with the workload below. Because a new .cpp is added, regenerate the solution by running `ThirdParty\premake5\premake5.exe vs2026` DIRECTLY from the Arcane dir (NOT `GenerateProjects.bat` — it hangs on `pause`). Confirm the Tests project globs `Tests/src/*.cpp` (it should — verify in `Arcane/premake5.lua` that no explicit file list excludes it; if the list is explicit, add the file).

  ```cpp
  // [perf] wall-time tripwire: the MKS units conversion restored the full physics
  // suite from ~1087 s (px era -- px content thrashing the 1 m residency tile /
  // broadphase) to ~96 s. This asserts a representative dense many-body step stays
  // fast, catching an ORDER-OF-MAGNITUDE regression (not 2x machine variance -- the
  // ceiling carries ~10x headroom). Tagged [perf] so loaded machines exclude it
  // (ArcaneTests.exe ~[perf]).
  #include <catch2/catch_test_macros.hpp>
  #include <Arcane/Physics/PhysicsWorld.hpp>
  #include <chrono>

  using namespace Arcane;
  using namespace Arcane::Physics;

  TEST_CASE("PhysicsPerf: dense broadphase step stays fast at MKS", "[perf][physics]")
  {
      // Meter-scale walled box + a dense grid of dynamic circles falling and piling
      // -- the workload class that was pathological at px scale. Exact body count /
      // step count / ceiling are set empirically in Step 2 (measure -> ~10x headroom).
      WorldDef wd;                       // MKS defaults (g=+10, sleepThreshold 0.05, ...)
      PhysicsWorld w(wd);

      // <floor + walls (static Aabb, meter-scale) built here>
      // <N dynamic circles r~0.1 m on a grid inside the box built here>

      const auto t0 = std::chrono::steady_clock::now();
      for (int s = 0; s < /*STEPS*/ 0; ++s) w.Step(Real(1) / Real(60));
      const auto t1 = std::chrono::steady_clock::now();
      const double ms =
          std::chrono::duration<double, std::milli>(t1 - t0).count();

      INFO("dense-step wall time (ms): " << ms);
      CHECK(ms < /*CEILING_MS*/ 0.0);    // set to ~10x the measured healthy time
  }
  ```

- [ ] **Step 2: Fill the workload + set the ceiling empirically.** Build a walled box (floor + 2 walls as static `MakeAabb` at meter scale — reuse the SandboxSmoke scene-0 shape: floor ~8-9 m halfW, walls) and a dense grid of ~200–500 dynamic `MakeCircle(Real(0.1))` bodies inside it (enough to exercise real broadphase pair churn — this is the whole point). Pick a step count that runs long enough to pile+settle (~300 steps). Run it, read the `INFO` wall-time, and set `CEILING_MS` to roughly 10× that measured healthy value (state the procedure: measure → ~10× → justify in a comment naming the measured number and the multiple; do NOT hard-code a guessed ceiling). Re-run to confirm green with margin.

- [ ] **Step 3: Gate.** Build (FOREGROUND). `ArcaneTests.exe "[perf]"` green. `ArcaneTests.exe "[physics]"` now includes this case — reconcile the delta explicitly (+1 case, + the small number of asserts this test adds; the base moves from 30638/278 by exactly that, recorded). Confirm `ArcaneTests.exe "~[perf]"` still runs the rest of the suite (the exclude path works).

- [ ] **Step 4: Commit** (explicit paths: the new test + the regenerated `.vcxproj`/`.slnx` if premake changed them). Message:
  ```
  test(arcane/physics): [perf] wall-time tripwire for the MKS restoration (MKS P5)
  ```
  Body: the mechanism decision (targeted `[perf]` tripwire with ~10× headroom + `[perf]`-excludable, over a flaky full-suite wall-clock assert or a documented-number-only gate); the measured healthy time + chosen ceiling; the `[physics]` count delta; the documented alternative (number-only) for the record. Cite survey C §Item-3.

---

### Task 6: Exit verification + burn-down proof + push (controller-run)

**Files:** none (verification + `.superpowers/sdd/progress.md` exit report). This is the last task; the engine WAS touched, so the SandboxSmoke pair + Loom are mandatory.

- [ ] **Step 1: Burn-down proof (the phase's whole point).** `grep -rc "PX-PIN" Arcane/Tests/src/*.cpp | grep -v ':0'` returns EMPTY (was 69 across 3 files). `grep -rn "MKS-DEFER" Arcane` returns EMPTY (was 3). `grep -rn "1e9" Arcane/Core/src/Arcane/Physics/Joints` returns EMPTY. Record the 69→0 / 3→0 transition.

- [ ] **Step 2: Full fast-suite gate.** `ArcaneTests.exe "~[gpu]"` — all pass. Reconcile against the 112060/495 reference: expected delta = the `[perf]` test (+1 case, + its asserts) — enumerate the exact additions; there must be NO un-enumerated assertion-count change. Also run `ArcaneTests.exe "~[gpu]~[perf]"` once to confirm the deterministic core (excluding the wall-clock test) is green independent of the perf tripwire.

- [ ] **Step 3: Engine-touched gates (mandatory — the smoke pair + camera tripwire + Loom).** These SERIALIZE (DLL copy lock) — run each isolated, FOREGROUND: `ArcaneTests.exe "[sandbox][gpu][d3d12]"` then `"[sandbox][gpu][vulkan]"` (both green incl. the new `CameraZoom()==100` CHECK — a Vulkan/D3D12 validation-silent pass, `RenderErrorCount()==0`); then `Loom.exe --frames 180` and `Loom.exe --backend vulkan --frames 180` (exit 0). If the camera CHECK FAILS, the plugin is pushing raw zoom again — that is the exact regression the tripwire exists to catch; STOP and report.

- [ ] **Step 4: Whole-branch assert audit.** `git diff main HEAD` (not `git diff main`). Enumerate every changed assertion line and classify it (PX-PIN marker strip, /10 rescale, Rule-3 re-derive, re-armed CCD speed, H1 inset fix, gravityY fix, settle re-baseline, the `[perf]` addition, the camera CHECK). Confirm ZERO un-enumerated weakenings and that the `[physics]` base moved ONLY by the enumerated `[perf]` additions. Confirm the two engine-edit clusters are the ONLY engine/Core/Sandbox diffs.

- [ ] **Step 5: Exit report + push.** Append the MKS-P5 exit report to `.superpowers/sdd/progress.md`: burn-down 69→0 + 3→0; the two engine edits (maxForce 1e9→1e6, velocity-ray gate options-field); suite counts + the enumerated `[physics]`/`~[gpu]` deltas; the smoke-pair + Loom results; the FULL-SUITE wall-time formalization (the documented ~96 s number + the new `[perf]` tripwire's measured/ceiling values — mechanism 1+4 both recorded); and the recorded-not-fixed residuals (DebugDrawTest pixel-readback, DebugCapsule silent-shrink). State that **the MKS units conversion is COMPLETE — all of P1–P6 on the branch, PX-PIN and MKS-DEFER both ZERO.** `git push -u origin feature/arcane-physics-mks-phase5`, then STOP — whole-branch review (fable) + merge = USER's call. Record the post-P5 closeout queue (A6 SpatialGrid-benchmark decision, B1–B7 parity adjudication, C declare-done, D2 sweep, E01–5 geometry predicates) as the next focus.

---

## Model plan

- T1 VelocityClamp (delete-only, mechanical): impl sonnet / rev sonnet
- T2 CCD (re-arm 300 m/s + H1 vacuous-assert + case-4 gravityY — subtle, no engine): impl opus / rev opus
- T3 Joints + maxForce trio (ENGINE + /1000-vs-/10000 split + settle re-baselines): impl opus / rev opus
- T4 debug-viz gate (ENGINE, small) + camera tripwire: impl sonnet / rev opus
- T5 wall-time [perf] tripwire (new-test design judgment): impl sonnet / rev opus
- T6 exit: controller-run
- Final whole-branch review: fable. Merge + push = USER's call.

## Self-Review (author checklist — completed)

- **Spec coverage:** §4 P5 cluster (Ccd re-armed / VelocityClamp / Joints incl. impulses mass-scaled) → Tasks 1–3 one-for-one; the P4-exit carry-forwards (MouseJoint maxForce → T3; velocity-ray gate → T4; full-suite wall-time assert → T5; camera framing-tripwire → T4; the two recorded residuals → T4/T6 note) each have an owner; §6 acceptance/contingency → the binding Global Constraint + per-task STOP triage.
- **Placeholder scan:** every conversion carries exact old→new values or the survey-section pointer for the full literal inventory; the only execution-derived numbers are (a) settle-time re-baselines (T3), (b) the `[perf]` ceiling (T5), (c) any re-measured baseline counts — all procedurally defined (measure → headroom → justify), never primed (protocol rule 6, this-doc-binding).
- **Type consistency:** `WorldDef`/`JointDef` field names (`maxForce`, `maxMotorTorque`, `gravityY`, `sleepThreshold`), `Real`/`Vec2`/`MakeCircle`/`MakeAabb`/`ApplyImpulse`/`SetVelocity`/`Step`, `PhysicsDebugDrawOptions::velocityScale` (the sibling for the new `velocityRayMinSpeed`), `Runtime::CameraZoom()`, and the three trio sites (Joint.hpp:100 / Joints.hpp:166 / Joints.cpp:458) were all read from the live files this session, not recalled. `kPi` availability in PhysicsJointsTest is flagged for verify-add (confirmed present in the JointSleep precedent).
- **Ordering:** T1 (delete-only) establishes the branch + baseline cheaply; T2/T3 carry the arithmetic; T3's engine edit is task-scoped with its cluster (P4 lesson); T4/T5 are the small final engine/test items; T6 runs the mandatory engine-touched gates (smoke pair + Loom) once, at the end. Each task ends green and independently reviewable.
- **Errata carried:** all three survey errata are encoded as binding decisions (case-4 gravityY ADD; GravityWorld gravityY DELETE; no 82a8ab99-retune to chase) rather than left as prose — the class that produced false bug reports before.
