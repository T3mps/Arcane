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
| B2 | Gravity/damping ordering (gravity damped) | — |
| B3 | Joints in island split/merge linkage | — |
| B4 | Sensors in island linkage | — |
| B5 | CCD multithreading | — |
| B6 | 24-color constraint-coloring parity | — |
| B7 | Dynamics on tile-spans vs the no-sleeping-dynamic invariant | — |

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
