// PhysicsFrameEventsTest.cpp -- [physics][events]: the per-frame window across
// 0, 1 and N fixed steps, and its clearing (spec 2026-10-08 s7.2). Real Runtime +
// RunLoop; EnsurePhysics mints a +Y-UP world (default gravity -9.81).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    constexpr double kFixed = 1.0 / 60.0;

    void AddBody(Astra::Registry& reg, Manifold2D::Physics::BodyType type, glm::vec2 pos, float hw, float hh)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::Identity id; id.id = Arcane::Guid::Generate(); reg.AddComponent<Arcane::Identity>(e, id);
        Arcane::Transform t; t.position = glm::vec3(pos, 0.0f); reg.AddComponent<Arcane::Transform>(e, t);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        Arcane::RigidBody2D rb; rb.type = type;
        // R8: a dynamic AABB asserts fixedRotation. The dynamic crate is an Aabb.
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

    const Arcane::PhysicsResource& Res(Arcane::Runtime& rt) { return *rt.Registry().GetResource<Arcane::PhysicsResource>(); }
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
    rt.Loop().Advance(0.0);                           // zero fixed steps
    CHECK(Res(rt).FrameEvents().contactBegin.empty());
    CHECK(Res(rt).StepEvents().contactBegin.size() <= 1);   // unchanged by the zero-step frame
}

TEST_CASE("The frame window keeps both steps' events in step order", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    for (int f = 0; f < 10; ++f) rt.Loop().Advance(kFixed);       // landed and touching
    REQUIRE(Res(rt).StepEvents().contactBegin.empty());           // the Begin is in the past
    // Teleport the crate away: the next multi-step frame holds its End.
    auto& reg = rt.Registry();
    reg.CreateView<Arcane::RigidBody2D, Arcane::Transform>().ForEach([&](Astra::Entity e, Arcane::RigidBody2D& rb, Arcane::Transform&)
    {
        if (rb.type == Manifold2D::Physics::BodyType::Dynamic)
            Res(rt).world->SetPosition(Res(rt).entityToBody.at(e), Manifold2D::Physics::Vec2(0.0f, 50.0f));
    });
    rt.Loop().Advance(2.5 * kFixed);
    CHECK(Res(rt).FrameEvents().contactEnd.size() == 1);
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
