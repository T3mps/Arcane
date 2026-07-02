// Narrowphase MT == serial byte-identity guard.
//
// Runs the SAME churning mixed-shape pile with a 1-worker (serial) executor
// and a K-worker (all-cores) executor; asserts that final body position +
// angle are bit-for-bit identical. At this task (Task 4) the narrowphase
// update pass is still serial -- both runs exercise the existing D1 solver-MT
// and D2 broadphase-MT paths -- so the test establishes the baseline contract
// and will guard Task 5's narrowphase parallelisation once it lands.
//
// Executor-injection pattern mirrors SolverMtInvarianceTest exactly.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp> // GridPassability (span-path MT scene)
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <cstdint>
#include <vector>

using namespace Arcane::Physics;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // Build a churning mixed pile: a static floor + 120 dynamic bodies of
    // mixed shape (circle / box / capsule), spawned in a spread-out region
    // above the floor.  The variety of shapes and spawn heights ensures that
    // every step creates, destroys, and updates many contacts -- exercising all
    // narrowphase state paths (new / existing / lost).  Returns the handles of
    // the dynamic bodies so their final state can be read back via
    // Position(h) / GetAngle(h).
    std::vector<BodyHandle> BuildChurn(PhysicsWorld& w)
    {
        // Static floor: wide box at y=300 (gravity is +Y so bodies fall down).
        {
            BodyDef fd;
            fd.type     = BodyType::Static;
            fd.position = Vec2(Real(0), Real(300));
            fd.shape    = MakeAabb(Real(400), Real(20));
            fd.friction = Real(0.6);
            w.AddBody(fd);
        }

        // LCG for deterministic spawn positions.
        std::uint32_t seed = 12345u;
        auto rnd = [&](Real a, Real b) -> Real
        {
            seed = seed * 1664525u + 1013904223u;
            return a + (b - a) * Real((seed >> 8) & 0xFFFF) / Real(65535);
        };

        std::vector<BodyHandle> handles;
        handles.reserve(120);

        for (int i = 0; i < 120; ++i)
        {
            BodyDef d;
            d.type     = BodyType::Dynamic;
            d.density  = Real(1);
            d.friction = Real(0.4);
            d.position = Vec2(rnd(Real(-300), Real(300)),
                              rnd(Real(-200), Real(260)));

            if (i % 3 == 0)
            {
                // Circle -- can freely rotate.
                d.shape = MakeCircle(rnd(Real(6), Real(12)));
            }
            else if (i % 3 == 1)
            {
                // Box -- fixedRotation because AABB shapes are axis-aligned.
                d.shape         = MakeAabb(Real(8), Real(8));
                d.fixedRotation = true;
            }
            else
            {
                // Capsule -- can freely rotate.
                d.shape = MakeCapsule(Real(10), Real(5));
            }

            handles.push_back(w.AddBody(d));
        }

        return handles;
    }

    // Build a create-heavy scene: 140 dynamic bodies rain onto a row of 12
    // static blocks, continuously forming new dynamic-vs-static contact pairs
    // every step.  Wider spread + more bodies than BuildChurn emphasises the
    // parallel CREATE detect path (Task 4 guard).  Returns handles of the
    // dynamic bodies for capture.
    std::vector<BodyHandle> BuildCreateHeavy(PhysicsWorld& w)
    {
        // Row of 12 static blocks -- the landing field.
        // Blocks span x in [-330, 330], y=280 (gravity +Y, so fall down).
        for (int s = 0; s < 12; ++s)
        {
            BodyDef st;
            st.type     = BodyType::Static;
            st.position = Vec2(Real(-330 + s * 60), Real(280));
            st.shape    = MakeAabb(Real(26), Real(12));
            w.AddBody(st);
        }

        // LCG for deterministic spawn positions (different seed from BuildChurn).
        std::uint32_t seed = 99991u;
        auto rnd = [&](Real a, Real b) -> Real
        {
            seed = seed * 1664525u + 1013904223u;
            return a + (b - a) * Real((seed >> 8) & 0xFFFF) / Real(65535);
        };

        std::vector<BodyHandle> handles;
        handles.reserve(140);

        for (int j = 0; j < 140; ++j)
        {
            BodyDef d;
            d.type     = BodyType::Dynamic;
            d.density  = Real(1);
            d.friction = Real(0.4);
            d.position = Vec2(rnd(Real(-330), Real(330)),
                              rnd(Real(-260), Real(220)));

            if (j % 3 == 0)
            {
                d.shape = MakeCircle(rnd(Real(6), Real(11)));
            }
            else if (j % 3 == 1)
            {
                d.shape         = MakeAabb(Real(8), Real(8));
                d.fixedRotation = true;
            }
            else
            {
                d.shape = MakeCapsule(Real(10), Real(5));
            }

            handles.push_back(w.AddBody(d));
        }

        return handles;
    }

    // Build the scene via `build`, inject `exec`, step 200 frames at 1/60 s,
    // and return a flat vector of (pos.x, pos.y, angle) per dynamic-body handle.
    // This is the byte-identity oracle: same scene, different executor.
    template<typename Builder>
    std::vector<Real> RunCapture(Arcane::ITaskExecutor* exec, Builder&& build)
    {
        WorldDef wd;
        wd.gravityY = Real(400);
        wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
        wd.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
        wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
        wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
        wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
        PhysicsWorld w(wd);
        w.SetExecutor(exec);

        const std::vector<BodyHandle> handles = build(w);

        for (int k = 0; k < 200; ++k)
        {
            w.Step(kStep);
        }

        std::vector<Real> out;
        out.reserve(handles.size() * 3u);
        for (auto h : handles)
        {
            const Vec2 p = w.Position(h);
            out.push_back(p.x);
            out.push_back(p.y);
            out.push_back(w.GetAngle(h));
        }
        return out;
    }

    // Span-path grid geometry (shared by RunCaptureSpans + its sanity bound).
    // A 64x64 grid of 20px cells (world span [0,1280]x[0,1280], tileOrigin 0,0)
    // shaped into a solid BOWL: a THICK full-width floor (rows 40..43 -> a merged
    // span at world y [800,880], top face y=800) plus solid side walls (cols 0..1
    // and 62..63, rows 24..43). The bowl CONTAINS the falling bodies: a 1-tile
    // (20px) span can be squeezed through by a dense agitated non-sleeping pile,
    // and a single-row floor lets edge bodies roll off into the void -- a 4-tile
    // (80px) floor cannot be tunnelled and the walls stop edge escape, so every
    // resting body sits on the merged tile SPANS, step after step.
    constexpr int  kSpanGridW       = 64;
    constexpr int  kSpanGridH       = 64;
    constexpr Real kSpanCellSize    = Real(20);
    constexpr int  kSpanFloorRow    = 40;                    // top solid floor row
    constexpr int  kSpanFloorRowEnd = 43;                    // thick floor: rows 40..43
    constexpr int  kSpanWallRowTop  = 24;                    // walls span rows 24..43
    constexpr Real kSpanFloorTop    = Real(kSpanFloorRow) * kSpanCellSize; // 800

    // Rain 140 mixed-shape dynamic bodies INSIDE the bowl, above the floor. They
    // fall (gravity +y) onto the merged tile span, generating tile-SPAN contacts
    // every step. Spawn x in [120,1160] keeps every body clear of the side walls
    // (inner faces x=40 and x=1240); spawn y in [520,720] is well ABOVE the floor
    // top (800) so they fall onto it. 140 bodies / kCreateGrain(16) ~= 9 chunks,
    // so the parallel detect fans across many workers and the per-worker span
    // buffers are non-empty on multiple workers (the merge under test).
    std::vector<BodyHandle> BuildSpanRain(PhysicsWorld& w)
    {
        std::uint32_t seed = 0xC0FFEEu;
        auto rnd = [&](Real a, Real b) -> Real
        {
            seed = seed * 1664525u + 1013904223u;
            return a + (b - a) * Real((seed >> 8) & 0xFFFF) / Real(65535);
        };

        std::vector<BodyHandle> handles;
        handles.reserve(140);

        for (int j = 0; j < 140; ++j)
        {
            BodyDef d;
            d.type     = BodyType::Dynamic;
            d.density  = Real(1);
            d.friction = Real(0.4);
            d.position = Vec2(rnd(Real(120), Real(1160)),
                              rnd(Real(520), Real(720)));

            if (j % 3 == 0)
            {
                d.shape = MakeCircle(rnd(Real(6), Real(11)));
            }
            else if (j % 3 == 1)
            {
                d.shape         = MakeAabb(Real(8), Real(8));
                d.fixedRotation = true;
            }
            else
            {
                d.shape = MakeCapsule(Real(10), Real(5));
            }

            handles.push_back(w.AddBody(d));
        }

        return handles;
    }

    // Like RunCapture, but builds a passability-backed world so the TILE-SPAN
    // create path runs. StaticCandidates only fills the per-worker span buffers
    // when m_tileGrid != nullptr, i.e. when WorldDef.passability is set; without
    // it the span-merge MT path (per-worker span Collide -> concat -> stable_sort
    // by awakeIndex) is never exercised under multiple workers. The grid is owned
    // here (passability is a borrowed pointer) and must outlive the world.
    std::vector<Real> RunCaptureSpans(Arcane::ITaskExecutor* exec)
    {
        GridPassability grid(kSpanGridW, kSpanGridH);
        // Thick full-width floor (rows 40..43).
        for (int cy = kSpanFloorRow; cy <= kSpanFloorRowEnd; ++cy)
        {
            for (int cx = 0; cx < kSpanGridW; ++cx)
            {
                grid.SetSolid(cx, cy, true);
            }
        }
        // Side walls (cols 0..1 and 62..63, rows 24..43) -> a closed bowl, so the
        // dense agitated pile cannot escape sideways off the floor edges.
        for (int cy = kSpanWallRowTop; cy <= kSpanFloorRowEnd; ++cy)
        {
            grid.SetSolid(0, cy, true);
            grid.SetSolid(1, cy, true);
            grid.SetSolid(kSpanGridW - 2, cy, true);
            grid.SetSolid(kSpanGridW - 1, cy, true);
        }

        WorldDef wd;
        wd.gravityY     = Real(400);
        wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
        wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
        wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
        wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
        wd.passability  = &grid;
        wd.tileCellSize = kSpanCellSize;
        wd.tileOrigin   = Vec2(Real(0), Real(0));
        // Disable sleep (threshold 0 -> the |v|+|w|*ext < threshold idle test is
        // never satisfied). Two reasons, both about KEEPING the span-create-MT
        // path under test: (1) every body stays in the awake-set every step, so
        // StaticCandidates runs for all of them and the per-worker span buffers
        // are populated across multiple workers for all 200 steps (not just the
        // brief fall-and-settle window). (2) It keeps this scene focused on the
        // create-phase narrowphase MT contract; deep dynamic piles resting purely
        // on tile spans (no static anchor BODY) otherwise stress the orthogonal
        // Phase-B "no sleeping dynamic in the solver feed" island invariant, which
        // is SERIAL sleep/island logic identical across executors -- not a
        // create-MT divergence, and out of scope for this guard.
        wd.sleepThreshold = Real(0);
        PhysicsWorld w(wd);
        w.SetExecutor(exec);

        const std::vector<BodyHandle> handles = BuildSpanRain(w);

        for (int k = 0; k < 200; ++k)
        {
            w.Step(kStep);
        }

        std::vector<Real> out;
        out.reserve(handles.size() * 3u);
        for (auto h : handles)
        {
            const Vec2 p = w.Position(h);
            out.push_back(p.x);
            out.push_back(p.y);
            out.push_back(w.GetAngle(h));
        }
        return out;
    }
} // namespace

TEST_CASE("Narrowphase MT == serial: state bit-identical", "[physics][mt]")
{
    Arcane::SerialTaskExecutor serial;
    Arcane::JobSystem           one(1);
    Arcane::JobSystem           many(0); // 0 = all cores

    auto* manyEx = many.TaskExecutor();

    // Surface the worker-count signal before any capture run so that a
    // single-core machine's "MT path not truly exercised" warning appears
    // in the run preamble rather than after a misleading green comparison.
    INFO("workers (many) = " << manyEx->WorkerCount());
    if (manyEx->WorkerCount() <= 1u)
    {
        WARN("single worker: MT thief path not exercised this run");
    }

    // Capture with three executor configurations.
    const std::vector<Real> a = RunCapture(&serial,             BuildChurn);
    const std::vector<Real> b = RunCapture(one.TaskExecutor(),  BuildChurn);
    const std::vector<Real> c = RunCapture(manyEx,              BuildChurn);

    // Sizes must match before element-wise comparison.
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());

    // Exact (bit-for-bit) equality -- byte-identity is the contract.
    REQUIRE(a == b);
    REQUIRE(a == c);
}

// Span-path coverage: the other create-MT cases build static BODIES (no tile
// grid), so StaticCandidates returns NO spans and the per-worker span-merge path
// (m_spanEntriesW concat -> stable_sort by awakeIndex -> m_spanContacts) is never
// exercised under multiple workers. This case builds a passability-backed world
// (a wide solid tile row) so falling bodies generate tile SPANS every step,
// driving the span-merge MT path, and asserts it is byte-identical serial vs
// all-cores.
TEST_CASE("Narrowphase span-path create MT == serial: state bit-identical",
          "[physics][mt]")
{
    Arcane::SerialTaskExecutor serial;
    Arcane::JobSystem           one(1);
    Arcane::JobSystem           many(0); // 0 = all cores

    auto* manyEx = many.TaskExecutor();

    INFO("workers (many) = " << manyEx->WorkerCount());
    if (manyEx->WorkerCount() <= 1u)
    {
        WARN("single worker: MT span-merge path not exercised this run");
    }

    // Capture with three executor configurations (serial / 1-worker / all-cores).
    const std::vector<Real> a = RunCaptureSpans(&serial);
    const std::vector<Real> b = RunCaptureSpans(one.TaskExecutor());
    const std::vector<Real> c = RunCaptureSpans(manyEx);

    // Sizes must match before element-wise comparison.
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());

    // Sanity: spans were actually GENERATED and effective. If the span path were
    // empty (no tile grid / no span merge), the bodies would tunnel through the
    // solid row and keep falling far past the floor; resting on the merged span
    // keeps every body's center at/above the floor top (smaller y is higher, +y
    // is down). A generous bound below the floor top proves containment without
    // pinning exact pile geometry. (Index 3*i+1 is each body's y.)
    REQUIRE(!a.empty());
    bool sawBoundedY = false;
    for (std::size_t i = 1; i < a.size(); i += 3u)
    {
        REQUIRE(a[i] < kSpanFloorTop + Real(60)); // never sank through the span
        sawBoundedY = true;
    }
    REQUIRE(sawBoundedY);

    // Exact (bit-for-bit) equality -- byte-identity is the contract.
    REQUIRE(a == b);
    REQUIRE(a == c);
}

TEST_CASE("Narrowphase create MT == serial: state bit-identical", "[physics][mt]")
{
    Arcane::SerialTaskExecutor serial;
    Arcane::JobSystem           one(1);
    Arcane::JobSystem           many(0); // 0 = all cores

    auto* manyEx = many.TaskExecutor();

    INFO("workers (many) = " << manyEx->WorkerCount());
    if (manyEx->WorkerCount() <= 1u)
    {
        WARN("single worker: MT thief path not exercised this run");
    }

    // Capture with three executor configurations (serial / 1-worker / all-cores).
    const std::vector<Real> a = RunCapture(&serial,             BuildCreateHeavy);
    const std::vector<Real> b = RunCapture(one.TaskExecutor(),  BuildCreateHeavy);
    const std::vector<Real> c = RunCapture(manyEx,              BuildCreateHeavy);

    // Sizes must match before element-wise comparison.
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());

    // Exact (bit-for-bit) equality -- byte-identity is the contract.
    REQUIRE(a == b);
    REQUIRE(a == c);
}
