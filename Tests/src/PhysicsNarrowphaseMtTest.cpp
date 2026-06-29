// Narrowphase MT == serial byte-identity guard.
//
// Runs the SAME churning mixed-shape pile with a 1-worker (serial) executor
// and a K-worker (all-cores) executor; asserts that final body position +
// angle are bit-for-bit identical. At this task (Task 4) the narrowphase
// update pass is still serial — both runs exercise the existing D1 solver-MT
// and D2 broadphase-MT paths — so the test establishes the baseline contract
// and will guard Task 5's narrowphase parallelisation once it lands.
//
// Executor-injection pattern mirrors SolverMtInvarianceTest exactly.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
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
    // every step creates, destroys, and updates many contacts — exercising all
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
                // Circle — can freely rotate.
                d.shape = MakeCircle(rnd(Real(6), Real(12)));
            }
            else if (i % 3 == 1)
            {
                // Box — fixedRotation because AABB shapes are axis-aligned.
                d.shape        = MakeAabb(Real(8), Real(8));
                d.fixedRotation = true;
            }
            else
            {
                // Capsule — can freely rotate.
                d.shape = MakeCapsule(Real(10), Real(5));
            }

            handles.push_back(w.AddBody(d));
        }

        return handles;
    }

    // Build the scene, inject `exec`, step 200 frames at 1/60 s, and return a
    // flat vector of (pos.x, pos.y, angle) per dynamic-body handle.
    // This is the byte-identity oracle: same scene, different executor.
    std::vector<Real> RunCapture(Arcane::ITaskExecutor* exec)
    {
        WorldDef wd;
        wd.gravityY = Real(400);
        PhysicsWorld w(wd);
        w.SetExecutor(exec);

        const std::vector<BodyHandle> handles = BuildChurn(w);

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

    // Capture with three executor configurations.
    const std::vector<Real> a = RunCapture(&serial);
    const std::vector<Real> b = RunCapture(one.TaskExecutor());
    const std::vector<Real> c = RunCapture(many.TaskExecutor());

    INFO("workers (many) = " << many.TaskExecutor()->WorkerCount());
    REQUIRE(many.TaskExecutor()->WorkerCount() >= 1u);
    if (many.TaskExecutor()->WorkerCount() <= 1u)
    {
        WARN("single worker: MT thief path not exercised this run");
    }

    // Sizes must match before element-wise comparison.
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());

    // Exact (bit-for-bit) equality — byte-identity is the contract.
    REQUIRE(a == b);
    REQUIRE(a == c);
}
