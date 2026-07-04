// Physics M6 P3.4: full-pipeline determinism replay (CCD + multi-joint + angle hash).
//
// =============================================================================
// COVERAGE MAP — P3.4 FULL-PIPELINE REPLAY
// =============================================================================
//
// This file is the P3.4 determinism gate explicitly deferred by P2.6.
// See the DEFERRED TO PHASE 3 note in PhysicsPhase2HarnessTest.cpp (lines 23-37
// and 50-56) for what this file must add over the P2.6 dynamics replay:
//
//   What P2.6 covered (PhysicsPhase2HarnessTest.cpp):
//     * Gravity + impulses + kinematic stirrer + 1 DistanceJoint, 240 steps.
//     * Run-twice identical hash (SoftStep self-check).
//     * Cross-broadphase (Tree/Hash/Sap) identical hashes.
//     * Hash formula: floor(x*1000) + floor(y*1000)*7 over 4 dynamic balls.
//
//   What P3.4 ADDS (this file):
//     * Bullet/CCD bodies: one bullet-kinematic + one bullet-dynamic aimed at a
//       thin static wall (exercises the P3.1 BulletSweep clamp).
//     * Multiple joint types: 3 distinct joints — Revolute, Weld, Prismatic —
//       between dynamic bodies. Their constrained trajectories enter the hash.
//     * Angle in the hash: floor(angle*1000)*13 per body per step. Dynamics +
//       joints + CCD all rotate bodies; angle is new state worth gating.
//     * Freely-rotating polygon box (v2, T9): one Dynamic MakePolygon box
//       (fixedRotation=false) released at a NONZERO angle above the floor.
//       Gravity + the v2 rotation-aware narrowphase + compound-COM contact
//       solver rotate it as it settles flat (the T5 "box settles flat" path).
//       Its position + angle enter the hash, so the determinism gate now also
//       covers the rotation-aware contact generation, not just joints/CCD.
//     * Anti-tunnel sub-assert: proves the CCD path is actually exercised (the
//       bullet body does NOT pass through the thin wall).
//     * Determinism: SoftStep run-twice identical.
//
// =============================================================================
// SCENE (single rich scene exercising the full Step pipeline)
// =============================================================================
//
// * gravityY = 300 (matching the P2.6 harness convention).
// * Static floor: AABB at (200, 250) half-extents (300, 12).
// * Thin static wall: AABB at (350, 100) half-extents (1, 80). "Thin" (2 units
//   wide) — a bullet body fired right at it would tunnel without CCD.
// * 3 dynamic bodies (d0, d1, d2): circle r=8, restitution=0.3, staggered.
//   Scripted impulses at step 60 and step 120.
// * 1 kinematic mover: constant authored velocity (+X), hashed for position.
// * 3 joints between dynamic bodies:
//     Revolute (static anchor above d0 ↔ d0): pendulum — angle evolves.
//     Weld    (d1 ↔ d2): rigid pair — relative angle locked.
//     Prismatic (static rail anchor ↔ d2): slide along +X only.
// * Bullet kinematic mover: starts left of the wall, fires right at it.
//   Flagged bullet=true → BulletSweep clamp catches it at the wall face.
// * Bullet dynamic body: also aimed at the wall, bullet=true (GJK-TOI backup).
//
// HASH FORMULA (extends P2.6):
//   h = (h * 31 + floor(x*1000) + floor(y*1000)*7 + floor(angle*1000)*13) % 2^48
// Hashed bodies: d0, d1, d2, kinematic mover, bullet-kinematic, bullet-dynamic,
//   rotating polygon box (appended last so the index order stays deterministic).
// Hash range: steps 1..240.
//
// ASSERTS:
//   1. Anti-tunnel: bullet bodies did NOT pass through the thin wall (CCD gate).
//   2. Run-twice identical (SoftStep): same hash both runs; hash != 0.
//
// PRESENTATION-FREE + C++20-clean. namespace Arcane::Physics helpers.
// Mirror structure + doc-comment style of PhysicsPhase2HarnessTest.cpp.

#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Joints/Joints.hpp>

using namespace Arcane::Physics;

namespace Arcane { namespace Physics { namespace P34Determinism {

    // -------------------------------------------------------------------------
    // Scene constants.
    // -------------------------------------------------------------------------
    constexpr Real kDt    = Real(1) / Real(60);
    constexpr int  kSteps = 240;

    // Thin static wall geometry. Wall span: x[kWallX - kWallHW, kWallX + kWallHW].
    constexpr Real kWallX  = Real(350);
    constexpr Real kWallHW = Real(1);   // half-width  → wall 2 units wide
    constexpr Real kWallHH = Real(80);  // half-height
    constexpr Real kWallCY = Real(100); // wall centre y

    // Bullet bodies start here, aimed right at the wall.
    constexpr Real kBulletStartX = Real(100);
    // Speed: one-step displacement = kBulletSpeed * kDt >> wall width.
    // kBulletSpeed * (1/60) = 300 units — clears the 2-unit wall many times.
    constexpr Real kBulletSpeed = Real(300) / kDt;

    // Probe radius for bullet bodies.
    constexpr Real kProbeR = Real(2);

    // Rotating polygon box (v2, T9). A Dynamic AABB shape asserts fixedRotation
    // (PhysicsWorld.cpp:690), so a freely-rotating box MUST be a MakePolygon.
    // CCW corners (BL, BR, TR, TL) of a half-extent hw x hh box.
    static Shape BoxPolygon(Real hw, Real hh)
    {
        return MakePolygon({
            Vec2(-hw, -hh),
            Vec2( hw, -hh),
            Vec2( hw,  hh),
            Vec2(-hw,  hh),
        });
    }

    // Initial drop angle for the rotating box (radians). NONZERO so gravity +
    // the rotation-aware contact solver rotate it toward flat as it settles.
    constexpr Real kBoxInitAngle = Real(0.6);

    // -------------------------------------------------------------------------
    // Hash helper (P3.4 extension of the P2.6 formula).
    // Extended: adds the angle term (floor(angle*1000)*13).
    //
    //   h = (h*31 + floor(x*1000) + floor(y*1000)*7 + floor(angle*1000)*13) % 2^48
    //
    // std::floor on Real gives signed truncation-toward-neginf, matching
    // Lua's math.floor. The modulo keeps the value in [0, 2^48).
    // -------------------------------------------------------------------------
    constexpr std::uint64_t kHashMod = (std::uint64_t(1) << 48);

    static std::uint64_t HashStep(std::uint64_t h, Real x, Real y, Real angle)
    {
        const auto ix     = static_cast<std::int64_t>(std::floor(x     * Real(1000)));
        const auto iy     = static_cast<std::int64_t>(std::floor(y     * Real(1000)));
        const auto ia     = static_cast<std::int64_t>(std::floor(angle * Real(1000)));
        const auto contrib = static_cast<std::uint64_t>(
            ix + iy * std::int64_t(7) + ia * std::int64_t(13));
        return (h * std::uint64_t(31) + contrib) % kHashMod;
    }

    // -------------------------------------------------------------------------
    // Output struct: bullet positions after the full run (for the anti-tunnel
    // assert, which is evaluated once outside the hash loop).
    // -------------------------------------------------------------------------
    struct BulletResult
    {
        Vec2 bulletKinPos{ Real(0), Real(0) };
        Vec2 bulletDynPos{ Real(0), Real(0) };
    };

    // -------------------------------------------------------------------------
    // RunScene: build + step the full-pipeline scene for kSteps steps.
    //
    // Returns the accumulated 64-bit hash over all hashed bodies (d0/d1/d2,
    // kinematic mover, bullet-kinematic, bullet-dynamic) across steps 1..kSteps.
    //
    // If outBullets is non-null, final positions of the two bullet bodies are
    // written to it so the caller can assert the anti-tunnel invariant.
    //
    // Params:
    //   outBullets  -- optional: receives final bullet body positions.
    // -------------------------------------------------------------------------
    static std::uint64_t RunScene(BulletResult* outBullets = nullptr)
    {
        // ---- World -----------------------------------------------------------
        WorldDef wdef;
        wdef.gravityY    = Real(300);           // matching the P2.6 harness
        wdef.broadphase  = BroadphaseKind::Tree;
        wdef.hashCellSize = Real(64);
        wdef.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
        wdef.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
        wdef.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
        wdef.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS

        PhysicsWorld w(wdef);

        // ---- Static floor (wide, centred at (200, 250)) ---------------------
        {
            BodyDef fd;
            fd.type     = BodyType::Static;
            fd.position = Vec2(Real(200), Real(250));
            fd.shape    = MakeAabb(Real(300), Real(12));
            w.AddBody(fd);
        }

        // ---- Thin static wall (bullet target; CCD path) ---------------------
        {
            BodyDef wd;
            wd.type     = BodyType::Static;
            wd.position = Vec2(kWallX, kWallCY);
            wd.shape    = MakeAabb(kWallHW, kWallHH);
            w.AddBody(wd);
        }

        // ---- Static anchor for Revolute joint (above d0) --------------------
        BodyHandle revAnchor;
        {
            BodyDef ad;
            ad.type     = BodyType::Static;
            ad.position = Vec2(Real(80), Real(50));
            ad.shape    = MakeCircle(Real(3));
            revAnchor   = w.AddBody(ad);
        }

        // ---- Static rail anchor for Prismatic joint (beside d2) -------------
        BodyHandle railAnchor;
        {
            BodyDef ad;
            ad.type     = BodyType::Static;
            ad.position = Vec2(Real(200), Real(80));
            ad.shape    = MakeCircle(Real(3));
            railAnchor  = w.AddBody(ad);
        }

        // ---- 3 dynamic bodies (d0, d1, d2): circle r=8, restitution=0.3 ----
        // d0 hangs below revAnchor for the pendulum.
        // d1 and d2 are welded together; d2 also attached to the prismatic rail.
        BodyHandle dyn[3];
        {
            const Vec2 positions[3] = {
                Vec2(Real(80),  Real(100)), // d0: below revAnchor
                Vec2(Real(150), Real(60)),  // d1: weld partner A
                Vec2(Real(200), Real(80)),  // d2: weld partner B + prismatic slider
            };
            for (int i = 0; i < 3; ++i)
            {
                BodyDef bd;
                bd.type        = BodyType::Dynamic;
                bd.position    = positions[i];
                bd.shape       = MakeCircle(Real(8));
                bd.density     = Real(1);
                bd.restitution = Real(0.3f);
                bd.friction    = Real(0.4f);
                dyn[i]         = w.AddBody(bd);
            }
        }

        // ---- Kinematic stirrer sweeping right --------------------------------
        BodyHandle kin;
        {
            BodyDef kd;
            kd.type     = BodyType::Kinematic;
            kd.position = Vec2(Real(0), Real(220));
            kd.shape    = MakeCircle(Real(10));
            kin = w.AddBody(kd);
            w.SetVelocity(kin, Vec2(Real(60), Real(0)));
        }

        // ---- Bullet kinematic: fires at the thin wall -----------------------
        // bullet=true → the BulletSweep stage in Step clamps it to TOI.
        BodyHandle bulletKin;
        {
            BodyDef bd;
            bd.type     = BodyType::Kinematic;
            bd.position = Vec2(kBulletStartX, kWallCY);
            bd.shape    = MakeCircle(kProbeR);
            bd.bullet   = true;
            bulletKin   = w.AddBody(bd);
            w.SetVelocity(bulletKin, Vec2(kBulletSpeed, Real(0)));
        }

        // ---- Bullet dynamic: also fires at the thin wall --------------------
        // bullet=true → speculative contacts + GJK-TOI backup both apply.
        BodyHandle bulletDyn;
        {
            BodyDef bd;
            bd.type     = BodyType::Dynamic;
            bd.position = Vec2(kBulletStartX, kWallCY + Real(6));
            bd.shape    = MakeCircle(kProbeR);
            bd.density  = Real(1);
            bd.bullet   = true;
            bulletDyn   = w.AddBody(bd);
            w.SetVelocity(bulletDyn, Vec2(kBulletSpeed, Real(0)));
        }

        // ---- Rotating polygon box (v2, T9): angled drop onto the floor ------
        // A Dynamic MakePolygon box (NOT MakeAabb -- a dynamic AABB asserts
        // fixedRotation) released at kBoxInitAngle above the static floor
        // (floor centre y=250, half-height 12 -> top surface ~238). Gravity
        // pulls it down; the v2 rotation-aware narrowphase + compound-COM
        // contact solver rotate it toward flat as it settles (T5 behavior).
        // fixedRotation=false so its angle is free to evolve and is hashed.
        BodyHandle box;
        {
            BodyDef bd;
            bd.type          = BodyType::Dynamic;
            bd.position      = Vec2(Real(200), Real(180)); // above the floor
            bd.shape         = BoxPolygon(Real(10), Real(10));
            bd.density       = Real(1);
            bd.restitution   = Real(0.1f);
            bd.friction      = Real(0.4f);
            bd.fixedRotation = false; // free to spin/settle
            box = w.AddBody(bd);
            // Release at a nonzero tilt so settling rotates it (SetAngle writes
            // the live m_angle the narrowphase transform reads).
            w.SetAngle(box, kBoxInitAngle);
        }

        // ---- Deep-overlap round body (v2 EPA/MPR): spawned OVERLAPPING a
        // static block so its first contact is a DEEP overlap, exercising
        // Collide's EPA deep-round cell (MPR fallback) in the full pipeline.
        // The f64->f32 EPA/MPR paths are deterministic; this body's position +
        // angle enter the hash LAST (stable index order), so the determinism
        // gate now also covers the deep-overlap recovery path. (This adds a body
        // to the scene -> it DELIBERATELY changes the hash value; the gate is
        // run-twice IDENTITY + != 0, not a pinned constant.)
        BodyHandle deepBlock; // static block the round body is buried in
        {
            BodyDef bd;
            bd.type     = BodyType::Static;
            bd.position = Vec2(Real(60), Real(180)); // off to the side, clear of the floor stack
            bd.shape    = MakeAabb(Real(20), Real(20));
            deepBlock   = w.AddBody(bd);
        }
        (void)deepBlock;
        BodyHandle deepBall;
        {
            BodyDef bd;
            bd.type        = BodyType::Dynamic;
            bd.position    = Vec2(Real(60), Real(174)); // 6 above the block centre, DEEP inside
            bd.shape       = MakeCircle(Real(5));
            bd.density     = Real(1);
            bd.restitution = Real(0.1f);
            bd.friction    = Real(0.4f);
            deepBall       = w.AddBody(bd);
        }

        // ---- Joint 1: Revolute — d0 as pendulum from revAnchor --------------
        // d0 starts offset from the anchor; gravity swings it down.
        // The body's angle evolves continuously → angle term is non-trivial.
        {
            JointDef jd;
            jd.kind   = JointKind::Revolute;
            jd.a      = revAnchor;
            jd.b      = dyn[0];
            jd.anchor = Vec2(Real(80), Real(50)); // pivot at static anchor centre
            w.AddJoint(jd);
        }

        // ---- Joint 2: Weld — d1 and d2 locked rigidly together --------------
        // The weld holds relative position + angle; the locked pair falls as a
        // unit. Their shared angle enters the hash once per body per step.
        {
            JointDef jd;
            jd.kind   = JointKind::Weld;
            jd.a      = dyn[1];
            jd.b      = dyn[2];
            jd.anchor = w.Position(dyn[1]); // weld point at d1's creation pos
            w.AddJoint(jd);
        }

        // ---- Joint 3: Prismatic — d2 slides along +X from railAnchor --------
        // Gravity is +Y (perpendicular to the +X axis); the perp constraint
        // must hold y. An impulse at step 60 pushes d2 along +X.
        {
            JointDef jd;
            jd.kind = JointKind::Prismatic;
            jd.a    = railAnchor;
            jd.b    = dyn[2];
            jd.axis = Vec2(Real(1), Real(0)); // horizontal slide axis
            w.AddJoint(jd);
        }

        // ---- Scripted run (steps 1..kSteps) ---------------------------------
        std::uint64_t hash = 0;
        for (int step = 1; step <= kSteps; ++step)
        {
            // Scripted impulses (mirror P2.6 step-60/step-120 pattern).
            if (step == 60)
            {
                // Kick d0 upward (pendulum gets energy).
                w.ApplyImpulse(dyn[0], Vec2(Real(0),    Real(-300)));
                // Kick d2 along the prismatic axis (+X) so it slides.
                w.ApplyImpulse(dyn[2], Vec2(Real(2000), Real(0)));
            }
            if (step == 120)
            {
                // Kick d1 to inject more motion into the weld pair.
                w.ApplyImpulse(dyn[1], Vec2(Real(80), Real(-120)));
            }

            w.Step(kDt);

            // -- Accumulate hash over all hashed bodies (stable index order) --

            // Dynamic bodies d0, d1, d2: position + angle.
            for (int i = 0; i < 3; ++i)
            {
                const Vec2 p = w.Position(dyn[i]);
                const Real a = w.GetAngle(dyn[i]);
                hash = HashStep(hash, p.x, p.y, a);
            }

            // Kinematic mover: position + angle (returns 0; uniform with other bodies).
            {
                const Vec2 p = w.Position(kin);
                const Real a = w.GetAngle(kin);
                hash = HashStep(hash, p.x, p.y, a);
            }

            // Bullet-kinematic: position + angle (post-CCD clamp on step 1).
            {
                const Vec2 p = w.Position(bulletKin);
                const Real a = w.GetAngle(bulletKin);
                hash = HashStep(hash, p.x, p.y, a);
            }

            // Bullet-dynamic: position + angle (speculative contacts keep it
            // on the near side; angle may change from contact torques).
            {
                const Vec2 p = w.Position(bulletDyn);
                const Real a = w.GetAngle(bulletDyn);
                hash = HashStep(hash, p.x, p.y, a);
            }

            // Rotating polygon box: position + angle (appended LAST so the
            // index order stays deterministic). Its angle evolves from the
            // angled drop + rotation-aware contact solve toward flat -- this
            // is the v2 narrowphase/compound-COM coverage this body adds.
            {
                const Vec2 p = w.Position(box);
                const Real a = w.GetAngle(box);
                hash = HashStep(hash, p.x, p.y, a);
            }

            // Deep-overlap round body: position + angle (appended AFTER the box
            // so the index order stays deterministic). Spawned buried in a
            // static block, its first contact takes the EPA deep-round cell; the
            // f64->f32 EPA/MPR recovery path is deterministic, so its evolving
            // position + angle gate that path's determinism. (Adding this body
            // intentionally re-baselines the hash VALUE; run-twice identity + !=0
            // is the gate, not a pinned constant.)
            {
                const Vec2 p = w.Position(deepBall);
                const Real a = w.GetAngle(deepBall);
                hash = HashStep(hash, p.x, p.y, a);
            }
        }

        // Write bullet final positions for the anti-tunnel assert.
        if (outBullets)
        {
            outBullets->bulletKinPos = w.Position(bulletKin);
            outBullets->bulletDynPos = w.Position(bulletDyn);
        }

        return hash;
    }

} } } // namespace Arcane::Physics::P34Determinism

using namespace Arcane::Physics::P34Determinism;

// ---------------------------------------------------------------------------
// 1. Anti-tunnel sub-assert: prove the CCD path is actually exercised.
//    Both bullet bodies (kinematic + dynamic) must NOT tunnel through the thin
//    static wall. This is a focused sub-assert outside the hash loop so the
//    test reader can see clearly what is being validated.
//
//    Wall near face: kWallX - kWallHW = 349.
//    Without CCD, one-step displacement = kBulletSpeed * kDt = 300 units,
//    landing the bullet at x ~ 400 — well past the wall.
//    With CCD (TOI clamp for kinematic; speculative margin for dynamic), the
//    bullet centre stops at or before the near face. Because the bullet shape
//    has radius kProbeR = 2, the actual stopped centre will be approximately
//    kWallNearX - kProbeR (= 347); the <= kWallNearX bound has generous margin
//    while still rejecting any half-tunnel (centre past x = 349 = wall centre
//    would still be < kWallX = 350 but that corner is already excluded here).
// ---------------------------------------------------------------------------

// Wall near face constant (near side of the 2-unit-wide wall).
constexpr Real kWallNearX = kWallX - kWallHW; // = 349

TEST_CASE("P3.4 full-pipeline determinism: bullet bodies do not tunnel (CCD gate)",
          "[physics][determinism]")
{
    BulletResult bullets{};
    RunScene(&bullets);

    // Bullet-kinematic: BulletSweep clamps to TOI → centre at or before near face.
    // A half-tunnel (centre at wall centre x=350) or full tunnel (x>351) both fail.
    // Empirical stopped centre: x ≈ 346.7 (≈ kWallNearX - kProbeR, CCD skin margin).
    REQUIRE(bullets.bulletKinPos.x <= kWallNearX);

    // Bullet-dynamic: speculative contacts + GJK-TOI → same invariant.
    // Empirical stopped centre: x ≈ 268.3 (zeroed on first contact step, held far back).
    REQUIRE(bullets.bulletDynPos.x <= kWallNearX);
}

// ---------------------------------------------------------------------------
// 2. Run-twice identical (SoftStep, DynamicTree broadphase).
//    Full pipeline: dynamics + CCD bullets + Revolute/Weld/Prismatic joints +
//    angle in hash. Same binary, same input → identical final hash.
// ---------------------------------------------------------------------------

TEST_CASE("P3.4 full-pipeline determinism: run-twice identical (SoftStep)",
          "[physics][determinism]")
{
    const std::uint64_t h1 = RunScene();
    const std::uint64_t h2 = RunScene();
    REQUIRE(h1 == h2);
    // Sanity: hash must be non-zero (the scene exercised non-trivial state).
    REQUIRE(h1 != 0u);
}
