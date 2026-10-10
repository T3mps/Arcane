// Settings arc S6-9: the grounded query (physics.ground.*), the parallel solver
// switch (physics.parallelSolver) and the 2D view's depth range
// (render.ortho2D.depthRange). Every default is the pre-sweep literal, bit for
// bit; the existing [physics2d] grounded tests and [camera] tests are the
// behavioural proof that nothing moved.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Config/Bindings/Physics2DBinding.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Scene/PhysicsQuerySettings.hpp>
#include <Arcane/Scene/RenderViewSettings.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <glm/glm.hpp>

using namespace Arcane;

TEST_CASE("sweep: physics query and ortho depth defaults are the pre-sweep literals", "[sweep][physics-query]")
{
    CHECK(Test::SameBits(Arcane::PhysicsGroundSettings2D{}.minNormalY, 0.5f));
    CHECK(Test::SameBits(Arcane::PhysicsGroundSettings2D{}.probeDistance, 0.05f));
    CHECK(Test::SameBits(static_cast<float>(Manifold2D::Physics::Real(Arcane::PhysicsGroundSettings2D{}.probeDistance)),
                         static_cast<float>(Manifold2D::Physics::Real(0.05))));   // Real is float: same bits as before
    CHECK_FALSE(Arcane::PhysicsWorldSettings2D{}.parallelSolver);
    CHECK(Test::SameBits(RenderOrtho2DSettings{}.depthRange, 1000.0f));
    Test::RequireDefault("physics.ground.minNormalY", CVarValue::Float32(0.5f));
    Test::RequireDefault("physics.parallelSolver", CVarValue::Bool(false));
    CHECK(HasFlag(CVarRegistry::Get().Explain("physics.ground.probeDistance")->flags, CVarFlags::Deterministic));
}

TEST_CASE("sweep: physics.ground / render.ortho2D descriptors match the inventory rows", "[sweep][physics-query]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    Test::RequireDefault("physics.ground.probeDistance", CVarValue::Float32(0.05f));
    Test::RequireDefault("render.ortho2D.depthRange", CVarValue::Float32(1000.0f));

    for (const char* name : { "physics.ground.minNormalY", "physics.ground.probeDistance" })
    {
        INFO(name);
        const auto e = reg.Explain(name);
        REQUIRE(e);
        CHECK(e->audience == Audience::Game);
        CHECK(e->scope == SettingScope::Project);
        CHECK(e->apply == ApplyMode::Live);
        CHECK(HasFlag(e->flags, CVarFlags::Deterministic));
    }

    if (Test::InThisBuild("physics.parallelSolver"))   // Dev: compiled out of Dist
    {
        const auto solver = reg.Explain("physics.parallelSolver");
        REQUIRE(solver);
        CHECK(solver->apply == ApplyMode::NextWorld);
        CHECK(HasFlag(solver->flags, CVarFlags::Dev));
        CHECK_FALSE(HasFlag(solver->flags, CVarFlags::Deterministic));
    }

    if (Test::InThisBuild("render.ortho2D.depthRange"))   // Dev: compiled out of Dist
    {
        const auto depth = reg.Explain("render.ortho2D.depthRange");
        REQUIRE(depth);
        CHECK(depth->audience == Audience::Game);
        CHECK(depth->scope == SettingScope::Project);
        CHECK(depth->apply == ApplyMode::Live);
        CHECK(HasFlag(depth->flags, CVarFlags::Dev));
    }
}

TEST_CASE("sweep: the 2D view's depth is render.ortho2D.depthRange, read live", "[sweep][physics-query][camera]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    const glm::vec2  center{ 3.0f, -2.0f };
    const glm::uvec2 viewport{ 800u, 600u };

    // At the default the 2D view is the old Orthographic(..., -1000, 1000), bit for bit.
    const ViewTransform before = Ortho2DView(center, 5.0f, viewport);
    const ViewTransform legacy = ViewTransform::Orthographic(center, 5.0f, viewport, -1000.0f, 1000.0f);
    CHECK(before.projection == legacy.projection);
    CHECK(before.view == legacy.view);

    Test::SkipIfCompiledOut("render.ortho2D.depthRange");
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("render.ortho2D.depthRange");
    struct Restore
    {
        CVarHandle handle;
        ~Restore()
        {
            CVarRegistry::Get().ClearRung(handle, SetBy::Code);
            CVarRegistry::Get().PublishImmediate();
        }
    } restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Float32(50.0f), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();

    const ViewTransform after = Ortho2DView(center, 5.0f, viewport);
    CHECK(after.projection == ViewTransform::Orthographic(center, 5.0f, viewport, -50.0f, 50.0f).projection);
    CHECK(after.projection != legacy.projection);
}
