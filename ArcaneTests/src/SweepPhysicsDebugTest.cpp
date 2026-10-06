// Settings arc S6-10: the physics debug overlay's look and toggles are the
// debug.physics.* cvars. PhysicsDebugDrawOptions inherits the four settings
// blocks, so a default-constructed options block still carries the pre-sweep
// literals bit for bit; MakePhysicsDebugDrawOptions() carries the published
// values. The [physics-debug] rich/transform/capsule tests are the behavioural
// proof that the drawing did not move.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>

#include <string>

using namespace Arcane;

TEST_CASE("sweep: physics debug defaults are the pre-sweep literals", "[sweep][physics-debug]")
{
    const PhysicsDebugDrawOptions o{};
    CHECK(Test::SameBits(o.lineThickness, 1.0f));
    CHECK(o.contacts); CHECK_FALSE(o.aabbs); CHECK(o.velocities); CHECK(o.comMarkers); CHECK(o.orientations);
    CHECK(Test::SameBits(o.contactMarkerSize, 0.03f));
    CHECK(Test::SameBits(o.velocityScale, 0.15f));
    CHECK(Test::SameBits(o.velocityMinSpeed, 0.05f));
    CHECK(Test::SameBits(o.comMarkerSize, 0.05f));
    CHECK(Test::SameBits(o.orientationTickLen, 0.18f));
    Test::RequireDefault("debug.physics.lineThickness", CVarValue::Float32(1.0f));
    Test::RequireDefault("debug.physics.draw.aabbs", CVarValue::Bool(false));
}

TEST_CASE("sweep: MakePhysicsDebugDrawOptions carries the published values", "[sweep][physics-debug]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("debug.physics.draw.aabbs"), CVarValue::Bool(true), SetBy::Code);
    reg.PublishImmediate();
    CHECK(MakePhysicsDebugDrawOptions().aabbs);
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}

TEST_CASE("sweep: physics debug palette, trace and arrow defaults are the pre-sweep literals", "[sweep][physics-debug]")
{
    const PhysicsDebugDrawOptions o{};
    CHECK(Test::SameBits(o.manifoldNormalLength, 20.0f));   // "world units" (pre-metres; converted as is)
    CHECK(Test::SameBits(o.manifoldPointPx, 3.0f));
    CHECK(Test::SameBits(o.normalLength, 28.0f));
    CHECK(Test::SameBits(o.emphasis, 1.0f));
    CHECK(Test::SameBits(o.traceLineThickness, 1.5f));
    CHECK(o.kinematic     == CVarColor{ 0.2f, 1.0f, 0.4f, 1.0f });
    CHECK(o.staticBody    == CVarColor{ 0.4f, 0.7f, 1.0f, 1.0f });   // cvar debug.physics.color.static
    CHECK(o.contact       == CVarColor{ 1.0f, 0.2f, 1.0f, 1.0f });
    CHECK(o.treeFat       == CVarColor{ 0.30f, 0.90f, 1.00f, 0.25f });
    CHECK(o.residencyGrid == CVarColor{ 1.00f, 0.70f, 0.20f, 0.35f });
    CHECK(o.subject       == CVarColor{ 1.00f, 0.95f, 0.35f, 1.0f });
    CHECK(o.island7       == CVarColor{ 0.85f, 0.85f, 0.85f, 1.0f });
    CHECK(o.narrowphase0  == CVarColor{ 0.70f, 0.70f, 0.70f, 1.0f });   // Separated / unknown kind
    CHECK(o.narrowphase6  == CVarColor{ 0.80f, 0.45f, 1.00f, 1.0f });   // Mpr
    Test::RequireDefault("debug.physics.color.static", CVarValue::Color({ 0.4f, 0.7f, 1.0f, 1.0f }));
    Test::RequireDefault("debug.physics.color.island0", CVarValue::Color({ 1.0f, 0.35f, 0.35f, 1.0f }));
    Test::RequireDefault("debug.physics.trace.lineThickness", CVarValue::Float32(1.5f));
    Test::RequireDefault("debug.physics.trace.normalLength", CVarValue::Float32(28.0f));
    Test::RequireDefault("debug.physics.manifoldNormalLength", CVarValue::Float32(20.0f));
    Test::RequireDefault("debug.physics.velocityMinSpeed", CVarValue::Float32(0.05f));
}

TEST_CASE("sweep: debug.physics.* descriptors are Game Dev, per-project preferences, Live", "[sweep][physics-debug]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    for (const char* name : { "debug.physics.lineThickness", "debug.physics.draw.contacts",
                              "debug.physics.color.kinematic", "debug.physics.color.narrowphase3",
                              "debug.physics.trace.emphasis" })
    {
        INFO(name);
        const auto d = reg.Explain(name);
        REQUIRE(d);
        CHECK(d->audience == Audience::Game);
        CHECK(d->scope == SettingScope::PreferencesProject);
        CHECK(d->apply == ApplyMode::Live);
        CHECK(HasFlag(d->flags, CVarFlags::Dev));
        CHECK(HasFlag(d->flags, CVarFlags::Archive));
        CHECK_FALSE(HasFlag(d->flags, CVarFlags::Deterministic));
        CHECK_FALSE(d->help.empty());
    }
}
