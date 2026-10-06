// The S6 identical-default helpers (Helpers/SettingsSweep.hpp): the sweep's
// per-task proofs read a cvar's DECLARED default, so a config rung that
// overrides the published value cannot hide a changed default.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/CVarTestDesc.hpp"
#include "Helpers/SettingsSweep.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <limits>

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
