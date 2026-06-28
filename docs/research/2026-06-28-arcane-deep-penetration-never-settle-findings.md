# Arcane physics: "deep interpenetration + never settles" — Box2D v3 source-level findings

**Date:** 2026-06-28
**Investigator:** deep source comparison + measured reproduction (systematic-debugging discipline)
**Status:** ROOT-CAUSED. One real bug (never-settle) + one non-bug (deep-pen is synthetic). Fix-or-shelve recommendation at the end.
**Branch baseline:** `main` @ `bad2303a` (D1 solver-MT + D2 broadphase-MT merged). The throwaway instrumentation used for this report is on a scratch tree and must be reverted (see "Throwaway instrumentation" below).

---

## TL;DR / verdict

The reported symptom was *"bodies interpenetrate deeply under compaction AND a dense pile never settles/sleeps."* These are **two different things** with **two different answers**:

1. **Never-settle: REAL engine bug. Reproduces at fully realistic scale (300 bodies).** Root cause = **island-unit sleep + sleep velocity thresholds that are too strict for the gravity-900 sandbox world** (the *angular* gate 0.05 rad/s is the actual blocker). A resting pile coalesces into one island; 1-2 boundary bodies (typically circles with a residual ~0.08 rad/s roll, just over the 0.05 rad/s gate) jitter forever at the threshold margin and veto sleep for the *entire* island. Measured: 290 / 292 bodies dead-idle for **38 seconds straight**, island still awake. **Recommend fixing** (it is an engine-quality defect, even though the shipping game won't stack hundreds of resting bodies).

2. **Deep interpenetration: NOT a solver bug — it is synthetic.** At realistic scale (300-1000 bodies, no whisk) penetration is **sub-pixel to ~1.3 px mean** (max 7-11 px) and **uniform with depth — never bottom-concentrated**. The deep overlap the user saw belongs to the *as-shipped scene 8 extreme*: a 1064 px/s kinematic "whisk" that compacts bodies faster than the (correct) 300 px/s push-out can recover, **by design** ("the pile NEVER settles" is the scene's stated goal), plus a bowl that is far too small to hold 10 000 bodies so they overflow and free-fall. **Recommend shelving** the deep-pen concern as a synthetic-stress artifact.

**The leading hypothesis going in — "push-out / sleep constants were mis-scaled from Box2D meters to pixels, making recovery glacial" — is REFUTED for push-out and PARTIALLY CONFIRMED for sleep.** Push-out (`ContactPushMaxVelocity = 300 px/s`) is actually *more* aggressive than the Box2D-equivalent (~270 px/s), and penetration recovery is excellent. The *sleep* thresholds, however, are genuinely too strict for the gravity-900 world (see the diff table), which is the never-settle root cause.

---

## Why this was never caught

The physics unit tests run the dynamics at `gravityY = 400` (`WorldDef` comment, `PhysicsWorld.hpp:197`). The **sandbox runs at `gravityY = 900`** (`Sandbox.cpp:44 kGravityY = 900.0f`). The sleep thresholds are **fixed constants** that do not scale with gravity. At 400 gravity (≈40 px/m) the fixed 2 px/s linear gate happens to equal Box2D's 0.05 m/s; at 900 gravity (≈90 px/m) the residual contact jitter is ~2.25× larger while the gate is unchanged, so it now sits *below* the jitter floor and the pile never sleeps. Tests pass; the showcase doesn't sleep.

---

## Scale convention used in this report

Box2D v3 is MKS (meters). Arcane's world is pixels. To compare, anchor the scale on **gravity**: sandbox `gravityY = 900 px/s²` vs Box2D default `-10 m/s²` ⇒ **≈ 90 px/m**. Sanity check: bodies are 18-32 px half-extent ⇒ 36-64 px diameter ⇒ 0.4-0.7 m at 90 px/m — i.e. crate-sized, exactly Box2D's canonical 0.5-1 m box. (At the *test* gravity of 400, the scale is ≈40 px/m.)

---

## Box2D v3 ⇄ Arcane constant diff table

All Box2D values fetched verbatim from `erincatto/box2d@main` today. Arcane values cited with file:line. "Box2D px-equiv" = Box2D meter value × 90 px/m.

| Quantity | Box2D v3 (source) | Box2D px-equiv @90px/m | Arcane (file:line) | Verdict |
|---|---|---|---|---|
| gravity | `-10 m/s²` (`types.c b2DefaultWorldDef`) | 900 px/s² | `900` (`Sandbox.cpp:44`) | **match (defines scale)** |
| push-out clamp | `contactSpeed = 3.0 m/s` (`types.c`) | 270 px/s | `contactPushMaxVelocity = 300` (`PhysicsWorld.hpp:222`) | OK — Arcane slightly *more* aggressive. **Refutes "glacial push-out".** |
| contact hertz | `30 Hz` (`types.c`) | 30 Hz | `contactHertz = 30` (`PhysicsWorld.hpp:219`) | match |
| contact damping ratio | `10.0` (`types.c`) | 10.0 | `contactDampingRatio = 10` (`PhysicsWorld.hpp:220`) | match |
| restitution threshold | `1.0 m/s` (`types.c`) | 90 px/s | `restitutionThreshold = 20` (`PhysicsWorld.hpp:221`) | OK — Arcane suppresses bounce *more* (helps settling) |
| linear slop | `B2_LINEAR_SLOP = 0.005 m` (`constants.h`) | 0.45 px | `kLinearSlop = 0.005` (`PhysicsTypes.hpp:145`) **but UNUSED** (grep: only the definition exists) | n/a — red herring; not fed to solver/collide |
| speculative distance | `4 × slop = 0.02 m` (`constants.h`) | 1.8 px (fixed) | velocity-scaled `max(kSkin, |v|·dt)`, floor `kSkin = 0.01 px` (`PhysicsWorld.cpp:2683-2695`, `PhysicsTypes.hpp:150`) | minor — Arcane's *resting* speculative window (0.01 px) is far smaller than Box2D's fixed 1.8 px, but velocity-scaling covers fast bodies; not implicated in the bug |
| hertz cap | `min(hertz, 0.125·inv_h)` (`physics_world.c`) | — | `min(hertz, 0.25/h)` (`SoftStep.cpp:211`) | **latent drift** (2× looser); no effect at the default 4 substeps (both clamp to 30) |
| static-contact softness | `staticSoftness = b2MakeSoft(2·hertz, …)` for dyn-vs-static (`physics_world.c`, `contact_solver.c`) | 60 Hz | **none** — one `contactSoft` for all (`SoftStep.cpp:212-221`) | **GAP** — Arcane's pile foundation is half as stiff as Box2D's |
| **sleep linear threshold** | combined `\|v\| + \|w\|·maxExtent < sleepThreshold = 0.05 m/s` (`solver.c b2FinalizeBodiesTask`, `types.c b2DefaultBodyDef`) | **4.5 px/s** | separate `\|v\|² < kSleepLinVel2 = 4` ⇒ **2 px/s** (`Island.hpp:93`, `Island.cpp:78`) | **TOO STRICT (~2.25×)** |
| **sleep angular threshold** | folded into the *same* 0.05 m/s budget via `\|w\|·maxExtent` (no separate angular gate) | for a 32 px body, ≈0.14 rad/s | separate `\|w\| < kSleepAngVel = 0.05` rad/s (`Island.hpp:96`, `Island.cpp:78`) | **TOO STRICT (~2.8×) and decoupled** — this is the *actual* blocker |
| time-to-sleep | `B2_TIME_TO_SLEEP = 0.5 s` (`constants.h`) | 0.5 s | `kSleepTime = 0.5` (`Island.hpp:100`, `Island.cpp:111`) | match |
| substeps | 4 (recommended) | 4 | `substepCount = 4` (`PhysicsWorld.hpp:218`) | match |

### Algorithm comparison (what matches)

The TGS-soft core is a **faithful port** — verified line-by-line, no defects found:

- **`b2MakeSoft`** (`SoftCoeffs.hpp:39-50`) is byte-identical to Box2D's `solver.h` (`omega=2π·hertz; a1=2ζ+h·omega; a2=h·omega·a1; a3=1/(1+a2); {omega/a1, a2·a3, a3}`).
- **Soft-contact bias branch** (`ContactConstraintSimd.hpp:805-828`, scalar twin `SoftStep.cpp:617-628`): `s>0 ⇒ bias=s·invH, scale 1/0`; `s≤0 & useBias ⇒ bias=max(biasRate·s, -maxBiasVel), soft scales`; relax ⇒ `bias=0, scale 1/0`. Identical to Box2D `contact_solver.c b2SolveContactsTask`.
- **Substep stage order** (`SoftStep.cpp` `BuildStages` + driver comment 950-968): integrate-vel → warm-start → solve(bias) → integrate-pos → relax(no bias), then once: restitution → store. Identical to Box2D `solver.h b2SolverStageType`.
- Effective masses, anchors, `baseSeparation = manifoldSep − dot(rB−rA, n)`, warm-start, friction Coulomb clamp, restitution-after-substeps, semi-implicit integrate with Padé damping `1/(1+h·c)` — all match.

**Conclusion: the solver math is correct.** The bug is in the *sleep* layer, not the solve.

---

## Reproduction evidence (measured)

Method: throwaway `ARCANE_PENPROF` instrumentation in `PhysicsWorld::Step` (after `EmitContactConstraints`, pre-solve) dumping, per N steps: awake count, contact penetration distribution (`pen = max(0, -baseSeparation)`), penetration vs Y-band, and over awake dynamics — velocity/sleep-timer distribution + island structure (count, largest, non-idle "blockers" in the largest island). Scene 8 with `ARCANE_NO_WHISK=1`, `ARCANE_PILE_COUNT=<n>`, Dist build, headless `Loom --frames`. (Headless frames ≠ steps; the RunLoop accumulator advances ~1 step per ~5 wall-clock frames.)

### 300 bodies, no whisk — the clean signal (Run H, ~40 s sim)
```
step=1200 (20s) awake=292  idle=291  blockers=1  maxLin=1.996 maxAng=0.0769 maxTimer=18.4  islands=1 maxIsland=292
step=2400 (40s) awake=292  idle=290  blockers=2  maxLin=2.049 maxAng=0.099  maxTimer=38.4  islands=1 maxIsland=292
meanPen ≈ 0.46 px, maxPen ≈ 6.7 px, deep(>8px)=0   (penetration is excellent)
```
**Interpretation:** the whole pile is ONE island. 290 of 292 bodies are dead-still (meanLin 0.13 px/s) and have been idle for **38 seconds**, yet the island never sleeps because **1-2 bodies jitter right at the gate** — and the gate they trip is **angular** (`maxAng 0.077-0.099 rad/s` vs the 0.05 rad/s threshold), consistent with circles retaining a slow residual roll that friction cannot fully kill. Island-unit sleep ⇒ those 1-2 veto all 292, permanently.

### 1000 bodies, no whisk (Run I, ~60 s sim)
```
step=1200 (20s) awake=992 idle=863 blockers=129 maxLin=20.7 maxAng=2.03 maxTimer=17.0 islands=1 maxIsland=992
step=2400 (40s) awake=992 idle=910 blockers=82  maxLin=12.1 maxAng=0.84 maxTimer=37.0
step=3600 (60s) awake=992 idle=954 blockers=38  maxLin=7.96 maxAng=0.38 maxTimer=57.0
meanPen ≈ 1.25 px, maxPen ≈ 11.5 px, deep≈7  (still shallow)
```
**Interpretation:** a deeper pile relaxes more slowly (blockers drain 129→82→38) but is on the same trajectory toward the irreducible 1-2-jitterer plateau; never sleeps; penetration stays shallow.

### 1000 bodies, WHISK ON — control (Run J)
```
step=600  awake=992 idle=2 fastLin=987 maxLin=1770 meanLin=229 maxAng=41 blockers=949 maxPen=48 deep=301
```
**Interpretation:** the whisk pins everything awake and *drives* the deeper penetration (mean body speed 229 px/s, max 1770 px/s). This is the as-shipped scene-8 behavior and is **by design** ("the pile NEVER settles").

### 10 000 bodies, no whisk (Run K) — the "deep compaction" test
```
contained pile: meanPen ≈ 4 px, uniform across Y-bands (NOT bottom-concentrated), maxPen ≈ 27 px
sleep stats:    meanLin grows 2935 → 45416 px/s over ~66 s (linear, ≈ g)
```
**Interpretation:** the growing velocity is **not a solver instability** — it is bodies *overflowing the undersized bowl* (10 000 × ~50 px bodies need ~14 000 px of fill height; the walls are only 2 000 px tall) and **free-falling into the void**, where v = g·t grows linearly (rate ≈ 900 px/s² = gravity). The *contained* pile penetration stays ~4 px and uniform. **No bottom-concentrated compression at any scale ⇒ the iterative-solver "pressure-propagation" hypothesis is refuted.**

---

## Root-cause statement

**Never-settle (real bug):** `Island::UpdateSleep` (`Island.cpp:64-141`) sleeps an island only when *every* awake member has been idle past `kSleepTime`, where "idle" is the **separate, gravity-unaware** predicate `v² < kSleepLinVel2 (=4 ⇒ 2 px/s) AND |w| < kSleepAngVel (=0.05 rad/s)` (`Island.cpp:78`, constants `Island.hpp:93/96`). In the gravity-900 world, a settled pile retains a small residual jitter — dominated by slow rolling of round bodies at ~0.08 rad/s — that exceeds the **angular** gate. A large pile is one island, so a single perpetual jitterer keeps hundreds of fully-idle bodies awake indefinitely. Box2D avoids this with a **combined, extent-weighted budget** `|v| + |w|·maxExtent < sleepThreshold (0.05 m/s ≈ 4.5 px/s)` (`solver.c b2FinalizeBodiesTask`), which both (a) is more forgiving for slow rotation (weights `w` by body size) and (b) is correctly scaled. Arcane's separate gates are ~2.25× (linear) / ~2.8× (angular) too strict at this gravity.

**Deep interpenetration (not a bug):** does not occur at realistic scale. The observed deep overlap is the scene-8 whisk (a 1064 px/s kinematic stirrer compacting bodies faster than the correct 300 px/s push-out can recover — by design) plus bowl overflow at 10 k. Penetration is never bottom-concentrated, so it is not a stacking/pressure-propagation limit.

This lines up with the engine's prior note ("SoftStep deep-embed residual grows with depth") but reframes it: the residual that "grows with depth" is **velocity jitter that blocks sleep**, not penetration depth.

---

## Fix-or-shelve recommendation

### Deep interpenetration → SHELVE
It is a synthetic-stress artifact (violent whisk + an overpacked, undersized bowl). The contained pile already resolves to sub-pixel/shallow penetration with a correct, slightly-aggressive push-out. No solver change warranted. Optional cosmetic follow-ups for the showcase only: size the scene-8 bowl to actually hold its body count, and/or cap escapee free-fall — neither is a physics-core fix.

### Never-settle → FIX (recommended), low-risk, Box2D-faithful
Adopt Box2D's sleep velocity test in `Island::UpdateSleep`. The principled change (not a magic-number band-aid):

1. **Replace the two separate gates with Box2D's combined extent-weighted budget:**
   `sleepVel = |v| + |w| · maxExtent(body)` compared against a single `sleepThreshold`.
   This directly fixes the angular-roll blocker (a 0.08 rad/s roll on a 32 px body contributes 0.08·32 ≈ 2.6 px/s, well under a 4.5 px/s budget) and self-scales with body size.
2. **Set `sleepThreshold` to the Box2D-equivalent for the world** (≈ `0.05 × pixelsPerMeter`; at the sandbox's 90 px/m that is ~4.5 px/s). Cleanest: make it a `WorldDef`/`BodyDef` field defaulting to a value derived from gravity scale, rather than a hard-coded `Island.hpp` constant, so it tracks the world (this is exactly the test-vs-sandbox gravity mismatch that hid the bug).

This is an intentional behavioral change to a non-byte-identical path, which the engine-evolution rule explicitly allows — **re-baseline the sleep-related tests deliberately**. Watch for: any test asserting exact awake/asleep counts or exact sleep-step timing (those are the ones to re-bless); the *solver* determinism tests are untouched (this changes only the sleep predicate).

A secondary, optional quality improvement that reduces the residual jitter at the source — **add Box2D's `staticSoftness = MakeSoft(2·contactHertz, …)` for dynamic-vs-static contacts** in `SoftStep::PrepareContacts` (branch on `cc.invMassB == 0`). This stiffens the pile foundation, lowering boundary jitter and improving stack quality generally. It is *not* required to fix never-settle (the sleep-test fix alone is sufficient and necessary) but is the right next step for pile fidelity and would also help the whisk scene look less mushy.

**Game-impact caveat:** the shipping game (4v4 turn-based combat on a 10×10 grid; iso overworld) does not stack hundreds of resting dynamic bodies, so this bug has ~zero gameplay impact today. It is an **engine-quality** fix (Arcane is positioned as a best-in-class engine; a resting pile that never sleeps is a real correctness/perf defect and a bad look in any physics demo). Prioritize accordingly.

### Suggested path if fixing
`brainstorming` → short `spec` (sleep-test redesign + threshold-as-world-param) → `writing-plans` → subagent-driven TDD, with deliberate re-baselining of sleep tests per the engine-evolution rule. Keep the static-softness improvement as a separate, optional follow-up change.

---

## Throwaway instrumentation to revert (NOT for merge)

- `PhysicsWorld.cpp`: the two `#include` lines marked `THROWAWAY (ARCANE_PENPROF)` and the `ARCANE_PENPROF` block inserted after `EmitContactConstraints` (the per-step penetration + sleep/island dump).
- `Scenes.cpp`: the `ARCANE_PILE_COUNT` override in `BuildStressTest` (marked `THROWAWAY`).
- `ARCANE_NO_WHISK` already existed in `Scenes.cpp` (not throwaway).

These are env-gated and zero-cost when off, but should be removed before any merge of a fix.
