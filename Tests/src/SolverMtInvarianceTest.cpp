#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <cstdint>

using namespace Arcane::Physics;

TEST_CASE("PhysicsWorld accepts an executor and steps with it (serial default)", "[physics][solvermt]")
{
    PhysicsWorld w{};
    Arcane::SerialTaskExecutor serial;
    w.SetExecutor(&serial);                 // explicit serial
    REQUIRE(w.Executor() == &serial);       // always-non-null invariant: returns the injected executor
    w.Step(1.0f / 60.0f);                   // must not crash; serial path unchanged
    SUCCEED("stepped with an injected executor");

    PhysicsWorld w2{};
    w2.SetExecutor(nullptr);                // null -> falls back to the world's serial default
    REQUIRE(w2.Executor() != nullptr);      // always-non-null invariant: serial fallback, never null
    w2.Step(1.0f / 60.0f);
    SUCCEED("stepped with null executor (serial fallback)");
}

namespace
{
    // Deterministic pile: a static floor + 200 dynamic boxes in a 10-wide column.
    // Gravity pulls in +Y; floor at y=5 (half-extent 0.5), boxes spawn at
    // negative Y (above the floor) -- same convention as PhysicsAwakeSetTest.
    // Returns (x, y, angle, vx, vy) per body after `steps`. Within-color-parallel
    // == serial => byte-identical across executors/thread counts.
    std::vector<float> RunPile(Arcane::ITaskExecutor* exec, int steps)
    {
        WorldDef wd;
        wd.gravityY = Real(400);
        PhysicsWorld w(wd);
        w.SetExecutor(exec);

        // Static floor: half-extents 20 x 0.5 at origin.
        {
            BodyDef fd;
            fd.type     = BodyType::Static;
            fd.position = Vec2(Real(0), Real(5));
            fd.shape    = MakeAabb(Real(20), Real(0.5));
            w.AddBody(fd);
        }

        // 200 dynamic boxes in a 10-column grid, spawned above the floor.
        // Column index c = i % 10  -> x in [-4.5 .. 4.5] step 1.0
        // Row index r    = i / 10  -> y = -1 - r*1.2 (above the floor)
        std::vector<BodyHandle> bodies;
        bodies.reserve(200);
        for (int i = 0; i < 200; ++i)
        {
            const int c = i % 10;
            const int r = i / 10;
            BodyDef bd;
            bd.type     = BodyType::Dynamic;
            bd.position = Vec2(static_cast<Real>(c - 5) * Real(1.0),
                               Real(-1) - static_cast<Real>(r) * Real(1.2));
            bd.shape    = MakeAabb(Real(0.4), Real(0.4));
            bd.density       = Real(1);
            bd.friction      = Real(0.3);
            bd.fixedRotation = true; // AABB shapes require fixedRotation (axis-aligned by definition)
            bodies.push_back(w.AddBody(bd));
        }

        for (int s = 0; s < steps; ++s)
        {
            w.Step(Real(1) / Real(60));
        }

        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(bodies.size()) * 5u);
        for (auto h : bodies)
        {
            const Vec2 p = w.Position(h);
            const Vec2 v = w.Velocity(h);
            out.push_back(static_cast<float>(p.x));
            out.push_back(static_cast<float>(p.y));
            out.push_back(static_cast<float>(w.GetAngle(h)));
            out.push_back(static_cast<float>(v.x));
            out.push_back(static_cast<float>(v.y));
        }
        return out;
    }
} // namespace

TEST_CASE("solver thread-count invariance: serial == enki(1) == enki(N)", "[physics][determinism][solvermt]")
{
    Arcane::SerialTaskExecutor serial;
    Arcane::JobSystem one(1);
    Arcane::JobSystem many(0);

    const auto a = RunPile(&serial,          120);
    const auto b = RunPile(one.TaskExecutor(), 120);
    const auto c = RunPile(many.TaskExecutor(), 120);

    REQUIRE(many.TaskExecutor()->WorkerCount() >= 1);
    REQUIRE(a.size() == b.size());
    REQUIRE(a == b);
    REQUIRE(a == c);
}
