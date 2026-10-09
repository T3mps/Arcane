// PhysicsFrameEventsTest.cpp -- [physics][events]: the per-frame window across
// 0, 1 and N fixed steps, and its clearing (spec 2026-10-08 s7.2). Real Runtime +
// RunLoop; EnsurePhysics mints a +Y-UP world (default gravity -9.81).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Sim/Time.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <string>
#include <vector>

namespace
{
    constexpr double kFixed = 1.0 / 60.0;

    void AddBody(Astra::Registry& reg, Manifold2D::Physics::BodyType type, glm::vec2 pos, float hw, float hh,
                 Arcane::Guid guid = {})
    {
        if (guid.IsNil()) guid = Arcane::Guid::Generate();
        Astra::Entity e = reg.CreateEntity();
        Arcane::Identity id; id.id = guid; reg.AddComponent<Arcane::Identity>(e, id);
        Arcane::Transform t; t.position = glm::vec3(pos, 0.0f); reg.AddComponent<Arcane::Transform>(e, t);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        Arcane::RigidBody2D rb; rb.type = type;
        // R8: a dynamic AABB asserts fixedRotation. Every dynamic body here is an Aabb.
        if (type == Manifold2D::Physics::BodyType::Dynamic)
            rb.fixedRotation = true;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);
        Arcane::Fixture f; f.kind = Manifold2D::Physics::ShapeKind::Aabb; f.halfW = hw; f.halfH = hh;
        Arcane::Collider2D c; c.fixtures.push_back(f); reg.AddComponent<Arcane::Collider2D>(e, c);
        reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});
    }

    // Ground top at y = 0; a crate whose bottom face starts 0.01 m above it, so it
    // touches within the first step or two.
    void Scene(Arcane::Runtime& rt)
    {
        AddBody(rt.Registry(), Manifold2D::Physics::BodyType::Static,  { 0.0f, -0.5f }, 10.0f, 0.5f);
        AddBody(rt.Registry(), Manifold2D::Physics::BodyType::Dynamic, { 0.0f, 0.51f }, 0.5f, 0.5f);
        rt.EnsurePhysics();
    }

    // Four crates, spaced in x, dropped from different heights so their landings
    // fall on different steps. Gaps are the distance from the ground top to the
    // crate's bottom face (half-height 0.25). Two pairs sit inside a 3-step
    // frame of the 2,3,2,3 pattern (steps 3-5 and steps 8-10).
    void DropScene(Arcane::Runtime& rt)
    {
        Astra::Registry& reg = rt.Registry();
        AddBody(reg, Manifold2D::Physics::BodyType::Static, { 0.0f, -0.5f }, 10.0f, 0.5f, Arcane::Guid{ 1, 1 });
        const struct { float x; float gap; Arcane::Guid guid; } crates[] = {
            { -4.5f, 0.010f, Arcane::Guid{ 2, 1 } },
            { -1.5f, 0.032f, Arcane::Guid{ 3, 1 } },
            {  1.5f, 0.090f, Arcane::Guid{ 4, 1 } },
            {  4.5f, 0.140f, Arcane::Guid{ 5, 1 } },
        };
        for (const auto& c : crates)
            AddBody(reg, Manifold2D::Physics::BodyType::Dynamic, { c.x, c.gap + 0.25f }, 0.25f, 0.25f, c.guid);
        rt.EnsurePhysics();
        rt.Loop().SetMaxStepsPerFrame(8);   // the 2/3-step frames must not hit the spiral cap
    }

    const Arcane::PhysicsResource& Res(Arcane::Runtime& rt) { return *rt.Registry().GetResource<Arcane::PhysicsResource>(); }

    std::uint64_t FixedStep(Arcane::Runtime& rt)
    {
        if (const Arcane::Time* t = rt.Registry().GetResource<Arcane::Time>()) return t->fixedStep;
        return 0;
    }

    bool SameSide(const Arcane::ContactSide2D& a, const Arcane::ContactSide2D& b)
    {
        return a.entity == b.entity && a.guid == b.guid && a.fixture == b.fixture;
    }

    bool SameVec(glm::vec2 a, glm::vec2 b) { return a.x == b.x && a.y == b.y; }

    // Owned copy of one window. The spans from StepEvents/FrameEvents die at the
    // next step, so a zero-step check has to keep the bytes itself.
    struct EventCopy
    {
        std::vector<Arcane::ContactBegin2D> contactBegin;
        std::vector<Arcane::ContactEnd2D>   contactEnd;
        std::vector<Arcane::ContactHit2D>   contactHit;
        std::vector<Arcane::SensorBegin2D>  sensorBegin;
        std::vector<Arcane::SensorEnd2D>    sensorEnd;

        bool operator==(const EventCopy& o) const
        {
            if (contactBegin.size() != o.contactBegin.size() || contactEnd.size() != o.contactEnd.size()
                || contactHit.size() != o.contactHit.size() || sensorBegin.size() != o.sensorBegin.size()
                || sensorEnd.size() != o.sensorEnd.size())
                return false;
            for (std::size_t i = 0; i < contactBegin.size(); ++i)
                if (!SameSide(contactBegin[i].a, o.contactBegin[i].a) || !SameSide(contactBegin[i].b, o.contactBegin[i].b))
                    return false;
            for (std::size_t i = 0; i < contactEnd.size(); ++i)
                if (!SameSide(contactEnd[i].a, o.contactEnd[i].a) || !SameSide(contactEnd[i].b, o.contactEnd[i].b))
                    return false;
            for (std::size_t i = 0; i < contactHit.size(); ++i)
            {
                const Arcane::ContactHit2D& x = contactHit[i];
                const Arcane::ContactHit2D& y = o.contactHit[i];
                if (!SameSide(x.a, y.a) || !SameSide(x.b, y.b) || !SameVec(x.point, y.point)
                    || !SameVec(x.normal, y.normal) || x.approachSpeed != y.approachSpeed)
                    return false;
            }
            for (std::size_t i = 0; i < sensorBegin.size(); ++i)
                if (!SameSide(sensorBegin[i].sensor, o.sensorBegin[i].sensor)
                    || !SameSide(sensorBegin[i].visitor, o.sensorBegin[i].visitor))
                    return false;
            for (std::size_t i = 0; i < sensorEnd.size(); ++i)
                if (!SameSide(sensorEnd[i].sensor, o.sensorEnd[i].sensor)
                    || !SameSide(sensorEnd[i].visitor, o.sensorEnd[i].visitor))
                    return false;
            return true;
        }
    };

    EventCopy CopyEvents(const Arcane::PhysicsEvents2D& e)
    {
        return {
            { e.contactBegin.begin(), e.contactBegin.end() },
            { e.contactEnd.begin(), e.contactEnd.end() },
            { e.contactHit.begin(), e.contactHit.end() },
            { e.sensorBegin.begin(), e.sensorBegin.end() },
            { e.sensorEnd.begin(), e.sensorEnd.end() },
        };
    }

    void RequireFrameEmpty(const Arcane::PhysicsEvents2D& e)
    {
        REQUIRE(e.contactBegin.empty());
        REQUIRE(e.contactEnd.empty());
        REQUIRE(e.contactHit.empty());
        REQUIRE(e.sensorBegin.empty());
        REQUIRE(e.sensorEnd.empty());
    }

    // Oracle identity: GUID + fixture. Entity ids belong to one registry.
    struct SideId
    {
        Arcane::Guid guid{};
        std::uint32_t fixture = 0;
        bool operator==(const SideId& o) const { return guid == o.guid && fixture == o.fixture; }
    };
    struct PairId
    {
        SideId a, b;
        bool operator==(const PairId& o) const { return a == o.a && b == o.b; }
    };

    SideId IdOf(const Arcane::ContactSide2D& s) { return { s.guid, s.fixture }; }

    struct PairLog
    {
        std::vector<PairId> begin;
        std::vector<PairId> end;
    };

    void AppendPairs(PairLog& log, const Arcane::PhysicsEvents2D& e)
    {
        for (const Arcane::ContactBegin2D& ev : e.contactBegin) log.begin.push_back({ IdOf(ev.a), IdOf(ev.b) });
        for (const Arcane::ContactEnd2D& ev : e.contactEnd)     log.end.push_back({ IdOf(ev.a), IdOf(ev.b) });
    }
}

TEST_CASE("FrameEvents gathers every step of a frame and empties on a zero-step frame", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    std::size_t total = 0;
    for (int f = 0; f < 30 && total == 0; ++f)
    {
        rt.Loop().Advance(2.5 * kFixed);              // two fixed steps per frame
        total = Res(rt).FrameEvents().contactBegin.size();
        CHECK(total <= 1);
    }
    REQUIRE(total == 1);                              // the landing, seen in its frame
    const EventCopy stepBefore = CopyEvents(Res(rt).StepEvents());
    rt.Loop().Advance(0.0);                           // zero fixed steps
    RequireFrameEmpty(Res(rt).FrameEvents());
    REQUIRE(CopyEvents(Res(rt).StepEvents()) == stepBefore);
}

TEST_CASE("FrameEvents matches the per-step oracle in order", "[physics][events]")
{
    // Runtime A: one fixed step per frame. Concatenated StepEvents is the
    // per-step sequence. Runtime B runs the same scene in alternating 2-step
    // and 3-step frames. 2.5 fixed steps does not alternate in double (the
    // leftover is just under one step, so two frames in a row take 2). Passing
    // an integer multiple of the loop's own fixed dt does. Determinism makes
    // A's sequence the oracle for B's FrameEvents.
    constexpr int kSteps = 40;                        // 8 * (2 + 3)
    Arcane::Runtime oracleRt(Arcane::Test::Process());
    DropScene(oracleRt);
    const double step = 1.0 / oracleRt.Loop().FixedHz();
    PairLog oracle;
    std::vector<int> beginsPerStep;
    std::vector<char> stepHadEvents;
    for (int s = 0; s < kSteps; ++s)
    {
        oracleRt.Loop().Advance(step);
        const Arcane::PhysicsEvents2D ev = Res(oracleRt).StepEvents();
        beginsPerStep.push_back(static_cast<int>(ev.contactBegin.size()));
        stepHadEvents.push_back(!ev.contactBegin.empty() || !ev.contactEnd.empty() ? 1 : 0);
        AppendPairs(oracle, ev);
    }
    REQUIRE(FixedStep(oracleRt) == static_cast<std::uint64_t>(kSteps));
    REQUIRE(oracle.begin.size() >= 4);                // one landing per crate

    Arcane::Runtime frameRt(Arcane::Test::Process());
    DropScene(frameRt);
    const double frameStep = 1.0 / frameRt.Loop().FixedHz();
    PairLog frames;
    int multiStepFrames = 0;
    std::size_t cursor = 0;
    for (int f = 0; f < 16; ++f)
    {
        const int expect = (f % 2 == 0) ? 2 : 3;
        const std::uint64_t before = FixedStep(frameRt);
        frameRt.Loop().Advance(expect * frameStep);
        const std::uint64_t taken = FixedStep(frameRt) - before;
        REQUIRE(taken == static_cast<std::uint64_t>(expect));
        const Arcane::PhysicsEvents2D ev = Res(frameRt).FrameEvents();
        int stepsWithEvents = 0;
        for (std::uint64_t i = 0; i < taken; ++i)
        {
            const std::size_t step = cursor + static_cast<std::size_t>(i);
            REQUIRE(step < stepHadEvents.size());
            if (stepHadEvents[step]) ++stepsWithEvents;
        }
        if (stepsWithEvents > 1) ++multiStepFrames;
        cursor += static_cast<std::size_t>(taken);
        AppendPairs(frames, ev);
    }
    REQUIRE(FixedStep(frameRt) == static_cast<std::uint64_t>(kSteps));
    REQUIRE(cursor == beginsPerStep.size());
    std::string shape;
    for (std::size_t i = 0; i < beginsPerStep.size(); ++i)
        if (beginsPerStep[i] > 0)
            shape += std::to_string(i + 1) + ":" + std::to_string(beginsPerStep[i]) + " ";
    INFO("per-step contact begins: " << shape);
    REQUIRE(multiStepFrames >= 1);                    // one frame held landings from two steps
    REQUIRE(frames.begin == oracle.begin);
    REQUIRE(frames.end == oracle.end);
}

TEST_CASE("Re-minting and restoring clear both windows", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    for (int f = 0; f < 30; ++f) rt.Loop().Advance(kFixed);
    auto bytes = rt.SnapshotRegistry();                          // RuntimeTest's RestoreRegistry path
    REQUIRE(bytes.IsOk());
    REQUIRE(rt.RestoreRegistry(*bytes.GetValue()));
    CHECK(rt.Registry().GetResource<Arcane::PhysicsResource>() == nullptr);   // windows gone with the world
    rt.EnsurePhysics();
    CHECK(Res(rt).StepEvents().contactBegin.empty());
    CHECK(Res(rt).FrameEvents().contactBegin.empty());

    // The hook is host policy on the RunLoop Rebind keeps. A stepping frame
    // after the restore still clears on the next zero-step frame.
    std::size_t produced = 0;
    for (int f = 0; f < 30 && produced == 0; ++f)
    {
        rt.Loop().Advance(kFixed);
        produced = Res(rt).FrameEvents().contactBegin.size();
    }
    REQUIRE(produced >= 1);
    rt.Loop().Advance(0.0);
    RequireFrameEmpty(Res(rt).FrameEvents());

    // Gravity edit on the scene root re-mints (RuntimeTest's PhysicsSettings
    // pattern). The replacement resource's windows start empty.
    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    Arcane::PhysicsSettings ps; ps.gravity = glm::vec2(0.0f, 2.0f);
    reg.AddComponent<Arcane::PhysicsSettings>(root, ps);
    const Manifold2D::Physics::PhysicsWorld* before = Res(rt).world.get();
    rt.EnsurePhysics();
    CHECK(Res(rt).world.get() != before);
    CHECK(Res(rt).StepEvents().contactBegin.empty());
    CHECK(Res(rt).FrameEvents().contactBegin.empty());
}

TEST_CASE("the frame hook tolerates a registry without PhysicsResource", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Loop().Advance(kFixed);                        // no EnsurePhysics: the hook finds nothing
    rt.ResetPhysics();
    rt.Loop().Advance(kFixed);
    SUCCEED();
}
