// The S6 identical-default helpers (Helpers/SettingsSweep.hpp): the sweep's
// per-task proofs read a cvar's DECLARED default, so a config rung that
// overrides the published value cannot hide a changed default.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/CVarTestDesc.hpp"
#include "Helpers/SettingsSweep.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <limits>
#include <stdexcept>

using namespace Arcane;
using namespace Arcane::Test;

TEST_CASE("sweep helpers: RegisteredDefault reads the declared default, not the published value", "[sweep]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Register(Desc("test.sweep.helperFloat", CVarValue::Float32(0.25f), Audience::Game,
                                           CVarFlags::None, "sweep-helper-test"));
    REQUIRE_FALSE(h.IsStale());
    CHECK(reg.Set(h, CVarValue::Float32(0.5f), SetBy::User, "sweep-helper-test") == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsFloat32() == 0.5f);                 // the User rung won the published value

    RequireDefault("test.sweep.helperFloat", CVarValue::Float32(0.25f));
    CHECK(RegisteredDefault("test.sweep.helperFloat").AsFloat32() == 0.25f);

    reg.UnregisterModule("sweep-helper-test");
    reg.Publish();
}

TEST_CASE("sweep helpers: SameBits compares representations, not values", "[sweep]")
{
    CHECK(SameBits(0.25f, 0.25f));
    CHECK_FALSE(SameBits(0.0f, -0.0f));                       // == says equal; the bits differ
    CHECK(SameBits(1.0 / 3.0, 1.0 / 3.0));
    CHECK_FALSE(SameBits(0.1, static_cast<double>(0.1f)));    // a float literal widened is not the double
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(SameBits(nan, nan));                                // == says unequal; the bits agree
}

// S6-GATE (the controller's sweep-hygiene ruling): a sweep case whose REQUIRE
// fails mid-way unwinds by exception; its RAII guard must still revert the
// Code rung and publish, so later random-order cases see the defaults.
TEST_CASE("sweep hygiene: a case that throws mid-way leaves the registry at its defaults", "[sweep]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle steps = reg.Find("sim.maxStepsPerFrame");
    const CVarHandle delta = reg.Find("sim.maxFrameDeltaSeconds");
    REQUIRE_FALSE(steps.IsStale());
    REQUIRE_FALSE(delta.IsStale());
    const CVarValue stepsDefault = Test::RegisteredDefault("sim.maxStepsPerFrame");
    const CVarValue deltaDefault = Test::RegisteredDefault("sim.maxFrameDeltaSeconds");

    CHECK_THROWS_AS([&] {
        const Test::ScopedCodeLayer layer;
        REQUIRE(reg.Set(steps, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
        REQUIRE(reg.Set(delta, CVarValue::Float64(0.5), SetBy::Code) == SetResult::Applied);
        reg.PublishImmediate();
        REQUIRE(*reg.Get(steps) == CVarValue::Int32(1));
        throw std::runtime_error("a failed REQUIRE mid-case");
    }(), std::runtime_error);
    CHECK_FALSE(reg.RungValue("sim.maxStepsPerFrame", SetBy::Code).has_value());
    CHECK_FALSE(reg.RungValue("sim.maxFrameDeltaSeconds", SetBy::Code).has_value());
    CHECK(*reg.Get(steps) == stepsDefault);   // published, not just cleared
    CHECK(*reg.Get(delta) == deltaDefault);

    CHECK_THROWS_AS([&] {
        const Test::ScopedCodeRung one("sim.maxStepsPerFrame", CVarValue::Int32(2));
        throw std::runtime_error("a failed REQUIRE mid-case");
    }(), std::runtime_error);
    CHECK_FALSE(reg.RungValue("sim.maxStepsPerFrame", SetBy::Code).has_value());
    CHECK(*reg.Get(steps) == stepsDefault);
}
