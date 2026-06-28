# Arcane physics: align sleep + contact stiffness with Box2D v3 — design

**Date:** 2026-06-28
**Status:** Design approved (brainstorming). Next: writing-plans.
**Findings basis:** `docs/superpowers/research/2026-06-28-arcane-deep-penetration-never-settle-findings.md`
**Baseline:** `main` @ `bad2303a`. Branch off `main`.

---

## Problem

A resting pile of dynamic bodies **never sleeps** in the gravity-900 sandbox world. Measured (300 bodies, no whisk): 290/292 bodies sit dead-idle for 38+ seconds, but the whole island stays awake forever because 1-2 boundary bodies (circles with a residual ~0.08 rad/s roll) jitter just over Arcane's `0.05 rad/s` angular sleep gate. Island-unit sleep lets those 1-2 veto hundreds.

Root cause: Arcane's sleep predicate is a **port of the original Lua engine's magic numbers** (`Island.hpp:89-100` — `kSleepLinVel2 = 4`, `kSleepAngVel = 0.05`, `kSleepTime = 0.5`), using **separate, gravity-unaware, body-size-unaware** linear and angular gates. It diverges from Box2D v3's **combined, extent-weighted, per-body-thresholded** sleep test and is ~2.25× (linear) / ~2.8× (angular) too strict at the sandbox's ≈90 px/m scale. The thresholds do not scale with gravity, which is why tests (gravity 400) sleep fine while the sandbox (gravity 900) never does.

Two related Box2D-fidelity gaps surfaced in the same investigation and are folded into this coordinated change:
- **No static-contact stiffening.** Arcane uses one `contactSoft` for all contacts; Box2D uses a 2× stiffer `staticSoftness` for dynamic-vs-static contacts (`physics_world.c`, `contact_solver.c b2PrepareContactsTask`). Arcane's pile foundation is half as stiff as Box2D's → more residual jitter.
- **Hertz-cap drift.** Arcane clamps contact hertz to `0.25/h` (`SoftStep.cpp:211`); Box2D clamps to `0.125·inv_h` (`physics_world.c`). No effect at the default 4 substeps (both clamp 30→30); a latent correctness gap if substeps < 4.

## Goal

A resting pile sleeps (island drains to `awake≈0`), stacks are stiffer/cleaner against the static world, and the hertz clamp matches Box2D — without regressing genuine motion (thrown/falling bodies don't sleep mid-flight) and without disturbing solver determinism beyond the intended behavioral changes.

## Non-goals

- Scene-8 bowl sizing / whisk behavior (synthetic stress; the whisk is designed to never settle). Out of scope.
- The deep-interpenetration symptom (shown to be synthetic: whisk + undersized bowl; realistic piles are sub-pixel). No solver-correctness change.
- Per-island sleep granularity / island splitting changes. The island-unit rule stays; the fix is the predicate.

---

## Design — three atomic, separately-committed, separately-re-baselined steps

Implemented and committed in dependency order. Each step is independently bisectable.

### Step 1 — Box2D-faithful sleep test (the bug fix; narrow re-baseline)

**1a. Per-body `maxExtent` (cached).** Add `m_maxExtent` to the PhysicsWorld SoA (resized alongside `m_sleepTimer`, `PhysicsWorld.cpp:196`). Compute it in `AddBody` inside the existing fixture loop (`PhysicsWorld.cpp:313-393`, where COM/mass are computed) as:
```
maxExtent = max over fixtures of ( max over core verts of |vert - localCenter| + shape.radius )
```
This is Box2D's `sim->maxExtent`. Every Arcane shape is N core verts + a scalar radius (`Shapes.hpp:7-11`: circle 1 vert+r, capsule 2 verts+r, aabb 4 corners+0, polygon N verts+0), so this is exact for all kinds. Expose `MaxExtentSlot(slot)`. Bodies with no fixtures / zero extent → 0 (harmless: their sleepVel is then pure `|v|`).

**1b. Per-body `sleepThreshold` (cached).** Add the per-body SoA array `m_sleepThreshold[slot]` (resized with `m_sleepTimer`). Set in `AddBody`: `def.sleepThreshold >= 0 ? def.sleepThreshold : m_sleepThresholdDefault` (per-body override, else the world default). Expose `SleepThresholdSlot(slot)`.

**1c. Params.** Add `WorldDef::sleepThreshold` (px/s, default chosen empirically in Validation step 2, expected ~3-5) and `BodyDef::sleepThreshold` (default sentinel `Real(-1)` ⇒ inherit world). PhysicsWorld stores the world default as a scalar member `m_sleepThresholdDefault`, mirrored from `WorldDef::sleepThreshold` in the ctor exactly like the other contact params (`PhysicsWorld.hpp:218-222`). (Distinct names: `m_sleepThreshold[]` = per-body SoA from 1b; `m_sleepThresholdDefault` = world scalar.)

**1d. Sleep predicate (`Island.cpp:64-86`).** Replace
```
if (v2 < kSleepLinVel2 && fabs(wv) < kSleepAngVel)   // accumulate
```
with the combined, extent-weighted, per-body-thresholded test:
```
sleepVel = length(v) + fabs(wv) * world.MaxExtentSlot(i);
if (sleepVel < world.SleepThresholdSlot(i))          // accumulate
```
Keep `kSleepTime` (0.5s) and the island-unit sleep logic unchanged.

**1e. Wake/sleep consistency (`PhysicsWorld.cpp:2349`).** Update `moverIsMoving` (the wake-on-contact predicate in `WakeMoverPair`) to the **same** test so "moving enough to wake a sleeping neighbor" mirrors "not idle enough to sleep":
```
moverIsMoving(s) = ( length(v[s]) + fabs(angVel[s]) * MaxExtentSlot(s) ) >= SleepThresholdSlot(s)
```

**1f. Retire** `kSleepLinVel2` and `kSleepAngVel` (`Island.hpp:93/96`). Keep `kSleepTime`. (`PhysicsPersistentContactTest.cpp:186` references only `kSleepTime` — unaffected.)

**Re-baseline scope (Step 1):** sleep/wake tests only. Solver determinism/byte-identity is untouched (no solver code changes).

### Step 2 — `staticSoftness` for dynamic-vs-static contacts (solver change; broad re-baseline)

In `SoftStep::PrepareContacts` (`SoftStep.cpp:203-221`), compute a second, 2× stiffer soft coefficient and select per contact:
```
contactSoft = MakeSoft(contactHertz, dampingRatio, h);
staticSoft  = MakeSoft(2 * contactHertz, dampingRatio, h);   // Box2D: 2x vs the immovable world
...
const bool vsStatic = /* B is the immovable static world */;
const SoftCoeffs& s = vsStatic ? staticSoft : contactSoft;
cc.biasRate = s.biasRate; cc.massScale = s.massScale; cc.impulseScale = s.impulseScale;
```
**`vsStatic` predicate:** Box2D applies `staticSoftness` only for **static** (not kinematic) contacts. Cleanest faithful option: tag the emitted `ContactConstraint` with a `bodyBIsStatic` bit in `EmitContactConstraints` (static body OR tile span = the immovable static world; kinematic stays on `contactSoft`). Fallback if a flag is undesirable: branch on `cc.invMassB == 0` (treats kinematic like static — slightly stiffer whisk contacts, harmless). The plan picks one; intent = "stiffen contacts against the immovable static world." The 2× is applied to the already-clamped `contactHertz`; `MakeSoft` handles any hertz (no re-clamp, matching Box2D).

**Re-baseline scope (Step 2):** stacking/contact behavioral tests that assert exact positions/penetration against statics will shift — re-bless deliberately (engine-evolution rule). Validate the quality effect on the repro rig (see Validation).

### Step 3 — hertz-cap alignment (trivial; no current behavioral effect)

`SoftStep.cpp:211`: change `Real(0.25) / h` → `Real(0.125) / h` and fix the comment (currently cites `0.25 * substepCount/dt`; Box2D `main` uses `0.125 * inv_h`). No behavioral change at 4 substeps (`min(30, 30)`); corrects the clamp for any future sub-4 substep count.

---

## Validation (repro rig is already set up)

The throwaway `ARCANE_PENPROF` / `ARCANE_PILE_COUNT` instrumentation (findings doc) stays in place through implementation, then is reverted before merge. After each step, run headless `Loom` scene 8:

1. **Sleep fix lands (Step 1):** 300- and 1000-body no-whisk piles drain to `awake≈0` (island sleeps) within a few seconds of settling. Confirm a freshly dropped/thrown body does **not** sleep mid-motion. Confirm the whisk-on scene still never sleeps (correct).
2. **Default tuning:** pick the lowest `WorldDef::sleepThreshold` that reliably sleeps the pile (blockers' combined sleepVel measured ~2.6 px/s; expect default ~3-5 px/s). Record the chosen value + evidence.
3. **staticSoftness (Step 2):** confirm reduced residual jitter / faster blocker drain and shallower/cleaner stacks; no new instability.
4. Re-run `ArcaneTests` (from its exe dir) after each step; re-baseline the intended-to-change tests only.

Commands (from `Arcane/bin/Dist-windows-x86_64-md/Loom/`):
```
ARCANE_SANDBOX_SCENE=8 ARCANE_NO_WHISK=1 ARCANE_PILE_COUNT=300 ARCANE_PENPROF=1 ARCANE_PENPROF_EVERY=300  Loom.exe --frames 6000
```

## Testing

- **New regression test (TDD, written first):** drop a small mixed pile (a few dozen bodies incl. circles) under gravity into a static bowl; assert the island sleeps (all members `!awake`) within N seconds. This is the guard the bug lacked. Include a circle-on-floor case (the rolling blocker class).
- **New unit test:** `maxExtent` correctness for each shape kind (circle, capsule, aabb, polygon, compound) — value = farthest core vert from COM + radius.
- **New unit test:** per-body `sleepThreshold` override beats the world default; sentinel inherits.
- **Re-baseline:** audit existing sleep/stacking tests for exact awake-count / sleep-timing / static-contact-position assertions; re-bless deliberately per step.

## Risks

- **Sleeping too eagerly** (threshold too high) → bodies freeze with visible residual motion. Mitigated by empirical tuning (pick the lowest reliable value) + the "thrown body doesn't sleep" check.
- **staticSoftness instability** at high stiffness → unlikely (Box2D uses exactly 2×); validated on the rig.
- **Determinism:** all changes are index-ordered, no wall-clock, no fast-math; MT solver paths are untouched (sleep + PrepareContacts are serial). Byte-identity is intentionally broken on the sleep predicate (Step 1) and dyn-vs-static solve (Step 2) — re-baseline, don't chase identity (engine-evolution rule).

## Rollout

Branch off `main`. Three commits in order (Step 1 → 2 → 3), each green on `ArcaneTests` with its re-baseline. Revert throwaway instrumentation in a final commit. Then the normal team flow (push branch, let CI go green, merge).

## Out of scope (future follow-ups)

- Scene-8 bowl sizing for 10k / escapee free-fall cap (showcase cosmetics).
- Optional: position-correction-aware sleep gate (Box2D folds `maxDeltaPosition` into sleepVel) — not needed for this fix.
