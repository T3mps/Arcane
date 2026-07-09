# Physics Box2D-Parity Ledger (2026-07-08)

The closeout adjudication of the recorded Box2D v3.1.1 divergences (section B of
`2026-07-06-physics-arc-closeout.md`). Each divergence is marked **FIXED** (Arcane
now matches Box2D) or **ACCEPTED-AS-DIVERGENCE** (a deliberate, justified departure).
The vendored oracle is `ThirdParty/box2d-3.1.1/` (file:line cites below).

Engine philosophy that governs these calls: byte-identity to Box2D is a tripwire,
not a goal (see the `engine-evolves-not-frozen` memory). Where Box2D's choice is
strictly better we adopt it; where a Box2D choice is neutral-or-worse for an
authored-content game engine, we may diverge **with a written rationale here**.

| ID | Divergence | Disposition |
|----|------------|-------------|
| B1 | Kinematic/pinned bodies not speed-clamped | ACCEPTED-AS-DIVERGENCE |
| B2 | Gravity/damping ordering (gravity damped) | FIXED |
| B3 | Joints in island split/merge linkage | ACCEPTED (already handled + tested) |
| B4 | Sensors in island linkage | ACCEPTED (already handled + tested) |
| B5 | CCD multithreading | ACCEPTED-AS-DIVERGENCE |
| B6 | 24-color constraint-coloring parity | ACCEPTED-AS-DIVERGENCE |
| B7 | Dynamics on tile-spans vs the no-sleeping-dynamic invariant | FIXED (+ test hardened) |

---

## B1 — Kinematic/pinned bodies not speed-clamped — ACCEPTED-AS-DIVERGENCE

**Box2D behavior (verified).** `b2IntegrateVelocitiesTask` (`src/solver.c:65-129`)
runs over the whole awake solver set — and a non-static body (dynamic **or**
kinematic) is placed in `b2_awakeSet` with its own `b2BodyState`
(`src/body.c:206-263`, `src/body.c:1643`), `awakeBodyCount = awakeSet->bodySims.count`
(`src/solver.c:1207`). The linear-speed clamp (`src/solver.c:107-114`) has **no
body-type / invMass guard**, so Box2D clamps kinematic velocities to
`maxLinearVelocity` (default 400) exactly like dynamics. The audit's B1 premise is
therefore correct: Arcane's kinematic exemption is a genuine divergence.

**Arcane behavior.** The clamp lives in `SoftStep::IntegrateVelocitiesRange`
(`Solver/SoftStep.cpp`), which iterates the awake-*dynamic* set only and additionally
`continue`s a pinned dynamic (`InvMassSlot <= 0`) before the clamp. Kinematics
integrate in `PhysicsWorld::Step` stage 1 (`PhysicsWorld.cpp:1727-1744`) with no
clamp. Net: kinematic and pinned-dynamic velocities are never speed-capped.

**Decision: ACCEPTED-AS-DIVERGENCE.** Rationale:
1. The `maxLinearVelocity` clamp is a **numerical-stability guardrail for
   force-integrated dynamics** — it bounds a body whose velocity blew up from a
   large force / tiny mass / contact explosion so it cannot integrate across the
   world in one step and wreck the solver. A kinematic body is **never
   force-integrated**; its velocity is authored directly and never grows from
   solver dynamics. The guardrail guards a failure mode kinematics cannot have.
2. Arcane already has the **correct** mechanism for fast kinematics — the CCD
   bullet sweep (`PhysicsWorld::BulletSweep`, TOI clamp vs statics). Fast authored
   movers are handled where they should be, not by silently rewriting the author's
   velocity.
3. Push injected into a dynamic by a fast kinematic contact is separately bounded
   by `ContactPushMaxVelocity` (the contact-solve push cap), so the one remaining
   "fast kinematic hurts a dynamic" concern is already mitigated without the clamp.
4. Silently capping authored kinematic motion at 400 m/s is a footgun for gameplay
   scripting (a dash / elevator / projectile authored faster would run slow with no
   error). A past author reached the same conclusion deliberately and documented it
   at `PhysicsQueryRotationTest.cpp` (a rotated-extent CCD test that drives a
   kinematic bullet at 1200 m/s on purpose).

**If a future need arises** (e.g. a kinematic plate injecting excessive push into
dynamics), the mitigation is to tune `ContactPushMaxVelocity` or add an *opt-in*
per-body speed cap — **not** to clamp all kinematics, which would break authored
fast movers.

**Code touched.** None (engine unchanged). The ad-hoc exemption comment in
`PhysicsQueryRotationTest.cpp` was reframed to reference this ledger entry.

---

## B2 — Gravity/damping ordering — FIXED

**Box2D behavior.** `b2IntegrateVelocitiesTask` (`src/solver.c:102-106`) damps the
OLD velocity and adds the **undamped** velocity delta afterward:
`v = linearVelocityDelta + linearDamping * v_old`, where
`linearVelocityDelta = h*invMass*force + h*gravityScale*gravity`. Gravity is NOT
scaled by the damping factor.

**Arcane behavior (before).** `SoftStep::IntegrateVelocitiesRange` computed
`v = (v_old + g*h) * f` with `f = 1/(1 + d*h)` — i.e. the gravity delta was damped
along with the velocity.

**Fix.** Reordered to Box2D's form: damp `v_old` first, then add the undamped
`g*h`. `SoftStep.cpp`:
```
float vx = m_bodyState[i].vx;               // was: + g.x*h
...
if (d > 0) { vx *= f; vy *= f; wv *= f; }   // damp OLD velocity
vx += g.x*h; vy += g.y*h;                    // undamped gravity delta AFTER
```
(Arcane has no external force/torque accumulators, so the linear delta is gravity
only; angular velocity gets damping only, matching Box2D's zero-torque case.)

**Byte-identity.** For `d == 0` (the default; the overwhelming majority of bodies)
the damping branch is skipped and both forms reduce to `v = v_old + g*h` —
identical. The behavior differs only for bodies with `linearDamping > 0`.

**Re-baseline surface (smaller than the audit anticipated: ONE test).** Only
`PhysicsDynamicsTest` "linear damping decays velocity" asserts absolute damped
velocities, so only it moved:
- Per-sub-step reference recurrence `refV = (refV + g*h)*fh` -> `refV = refV*fh + g*h`
  (the closed form of the new algorithm; the tight `Approx` at the loop still holds
  because the engine does exactly this per sub-step).
- Discrete terminal `g/damp` -> `g/damp + g*h`. Derivation: the fixed point of
  `v = v*f + g*h` is `v(1-f) = g*h`; with `1-f = d*h*f` this gives `v = g/(d*f) =
  g/d + g*h` (the old form's fixed point was exactly `g/d`, the continuous terminal;
  the parity fix shifts it up by one sub-step of undamped gravity). Both values
  derived from the algorithm, not read off engine output.

Every other damped case is unaffected: `PhysicsComponentsTest` only round-trips the
`linearDamping` field; `PhysicsDynamics` run-twice determinism compares two runs of
the same engine (bit-identical either ordering); `PhysicsJointsTest` (linDamp 1.5)
asserts the settled joint EQUILIBRIUM (pendulum at rod length), which is
ordering-invariant. Full `[physics]` 30639/279 green, assertion count unchanged.

**Adjacent observation (NOT addressed by B2, on record).** Arcane has no
`angularDamping` field; the single linear-damping factor `f` is applied to angular
velocity too. Box2D keeps a separate `angularDamping`. This is a pre-existing model
simplification, not an ordering bug, and out of B2's scope — revisit only if a body
needs distinct angular damping (a `BodyDef.angularDamping` feature, not a parity fix).

---

## B3 — Joints in island split/merge linkage — ACCEPTED (already handled + tested)

**GAP A** from the Box2D-v3 MT-parity program: are joints treated as island edges in
BOTH merge and split, so a jointed construct sleeps/wakes as one unit and a contact
removal never wrongly splits a still-jointed pair? Verified on main: they are.
- MERGE: `AddJoint` unions the two dynamic endpoints' islands (PhysicsWorld.cpp:1282);
  `RemoveJoint` / body-destroy wakes + marks the surviving endpoint's island a split
  candidate (:1330, :1227).
- SPLIT: `SplitIsland`'s connected-component union-find walks BOTH contact edges AND
  `m_joints` (PhysicsWorld.cpp:3590-3614) — a jointed dyn-dyn pair stays in one
  component even when it shares no touching contact.
- TESTED: `PhysicsJointSleepTest` (3 cases, 28 assertions, green): jointed chain sleeps
  as a unit; impulse on one member wakes the whole island; removing a joint splits the
  island and the pieces sleep independently.

**Decision: ACCEPTED — no behavioral gap.** The serial split-linkage work already
satisfied GAP A; joints are full island edges in both directions and verified.
Island split/merge is a serial pass (not on the MT solve path), so there is no
MT-specific joint-linkage gap. No code change.

---

## B4 — Sensors in island linkage — ACCEPTED (already handled + tested)

**GAP B** from the MT-parity program: sensors must be event-only and must NEVER act as
island edges (a sensor contact transmits no force — Box2D excludes sensors from
islands). Verified on main: sensor dyn-dyn pairs carry `solverRelevant == false`
(PhysicsWorld.cpp:2427), and the island merge/split signals `kNpStarted`/`kNpStopped`
are set ONLY for `solverRelevant` contacts (PhysicsWorld.cpp:2554-2561), so a sensor
pair never enters `m_pendingMerges` and never marks a split candidate.
- TESTED: `PhysicsSensorIslandTest` — "sensor dyn-dyn pair does not merge islands" +
  the non-sensor regression counterpart ("does merge").

**Decision: ACCEPTED — no behavioral gap.** Sensors are correctly excluded from island
linkage (matching Box2D), verified by a dedicated test pair. No code change.

---

## B5 — CCD multithreading — ACCEPTED-AS-DIVERGENCE

Box2D parallelizes its continuous-collision (bullet) pass; Arcane's `BulletSweep` is
serial. **Decision: ACCEPTED.** The discrete sweep runs only for `isBullet` bodies (a
tiny minority — ordinary fast dynamics are handled inline by the speculative contact
margin, not the discrete sweep), so it is a negligible fraction of step time;
parallelizing it is an unproven-value optimization. Revisit only if a game consumer
profiles a bullet-heavy scene where the serial sweep dominates.

---

## B6 — 24-color constraint-coloring parity — ACCEPTED-AS-DIVERGENCE

Box2D uses a fixed 24-color graph-coloring scheme; Arcane uses a persistent per-contact
coloring (`kColorCount` colors + a scalar overflow tail). **Decision: ACCEPTED (value
unproven).** The color scheme only governs the batch granularity of the MT contact
solve, which is inherently L3-bandwidth-bound and breaks even only at ~10k bodies
(ledger C7 / the solver-MT findings). The exact color count is an internal parallelism
detail with no correctness or behavioral impact and no measured benefit at current
scales. Matching Box2D's 24-color scheme is a mechanical change we would adopt only if
a profiled MT-solve workload showed a coloring-limited bottleneck.

---

## B7 — Dynamics on tile-spans vs the no-sleeping-dynamic invariant — FIXED (real state fix, already on main) + regression test hardened

**The invariant.** `EmitContactConstraints` (PhysicsWorld.cpp:3146-3155) requires that
no emitted contact constraint references a sleeping dynamic: it emits only for an
AWAKE bodyA (the :3062 gate) and asserts (Debug) that neither A nor a body B is a
sleeping dynamic. The solver's `SyncIn` relies on this to safely skip sleeping
dynamics.

**Root cause of the trip (diagnosed).** A dynamic that settles and sleeps resting
PURELY on tile spans (a span is a virtual fixture, not a body, so it anchors no
island) becomes a SINGLETON sleeping island. When a still-awake, NEAR-IDLE neighbour
later drifts into speculative contact, the touch-begin queues a cross-awake island
merge. `WakeMoverPair`'s `moverIsMoving` gate (PhysicsWorld.cpp:2343-2350) declines to
wake the sleeper because the incoming body is below `sleepThreshold` (checked in
stage 2, before the solver integrates gravity), so `MergeIslands` would graft the
sleeping singleton into the awake island: a MIXED island (awake A + sleeping B), and
the very next emit trips the assert (aborts in Debug). Contact orientation makes the
awake body bodyA (lower slot) and the sleeper bodyB (PhysicsWorld.cpp:2412), so the
emit passes the awake-A gate and the sleeping-B assert fires.

**Disposition: FIXED via a REAL STATE FIX (not a relax), already on main.**
Commit `e4a1168c` added, at the pending-merge apply loop (PhysicsWorld.cpp:3014-3018):
before `MergeIslands`, if the two sides differ in awake state, `WakeIsland` the
sleeping side. This restores Box2D's "island is uniformly awake" invariant so a
cross-awake merge always ends awake (the correct fix, NOT a relaxation of the assert).
A begin-touch always involves at least one moving/awake body, so it never over-wakes a
legitimately all-asleep pair. The invariant holds suite-wide on main (Debug
`[physics]` green with asserts active).

**Regression coverage hardened (this closeout).** The original regression test
(`PhysicsIslandWakeMergeTest`, a 140-body emergent pile) had gone VACUOUS after the
MKS scene conversion (sleepThreshold 8 px/s to 0.05 m/s, /100 geometry, g to 10)
shifted its settling so it no longer reproduces the near-idle-toucher window:
verified: with the wake guard commented out, the emergent test still passed. A vacuous
regression test gives false confidence, so a DETERMINISTIC tripwire was added
(`PhysicsIslandWakeMerge: cross-awake merge wakes a span-sleeping singleton`): settle
two boxes to sleep as separate span singletons (waker = lower slot = bodyA), then
`Wake` + teleport the waker to just overlap the still-sleeping sleeper at zero
velocity, and step once. It is load-bearing in BOTH build modes:
- Debug: the engine assert fires inside `Step()` (aborts) when the guard is removed.
- Release/any: the test's own `REQUIRE(IsAwake(sleeper))` fails (VERIFIED: a Release
  build with the guard disabled fails exactly there, sleeper stays asleep in the mixed
  island).
The emergent pile test is retained + relabelled as a realistic-scene sanity check.

**Code touched.** Engine: none (the fix `e4a1168c` was already on main). Test:
`PhysicsIslandWakeMergeTest.cpp` (new deterministic tripwire + honest relabel of the
emergent case). `[physics]` 30639/279 to 30649/280 (+1 case).
