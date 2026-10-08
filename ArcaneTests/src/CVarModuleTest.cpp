// Module capture (settings spec s4.3, O1): a registration names its module --
// the innermost CVarModuleScope, else the caller's ARC_MODULE_NAME. [cvar]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarModule.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include "Helpers/CVarTestDesc.hpp"

#include <string_view>

using namespace Arcane;

namespace S1ModuleTest
{
    ARC_CVAR(cvar_moduleProbe, "tests.moduleProbe", std::int32_t, 3,
             .help = "Module-capture probe (CVarModuleTest).");
}

TEST_CASE("module capture: the scope, else the caller's ARC_MODULE_NAME; ArcaneCore names itself", "[cvar]")
{
    CHECK(std::string_view(ARC_MODULE_NAME_STRING) == "ArcaneTests");
    CHECK(CVarRegistry::ScopedModule().empty());
    CHECK(Detail::CallerModule() == "ArcaneTests");
    CHECK(CVarRegistry::CurrentModule() == "ArcaneCore");
    CHECK(CVarRegistry::Get().ModuleOf(S1ModuleTest::cvar_moduleProbe.Handle()) == "ArcaneTests");

    CVarRegistry reg;
    CVarHandle inner, outer;
    {
        const CVarModuleScope a("GameA");
        {
            const CVarModuleScope b("GameB");
            CHECK(Detail::CallerModule() == "GameB");
            inner = reg.Register(Test::Desc("cap.inner", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
        }
        CHECK(CVarRegistry::ScopedModule() == "GameA");
        outer = reg.Register(Test::Desc("cap.outer", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
    }
    const CVarHandle bare = reg.Register(Test::Desc("cap.bare", CVarValue::Int32(0), Audience::Game, CVarFlags::None, {}));
    CHECK(reg.ModuleOf(inner) == "GameB");
    CHECK(reg.ModuleOf(outer) == "GameA");
    CHECK(reg.ModuleOf(bare) == "ArcaneCore");       // Register's own fallback: the module it lives in
    CHECK(reg.ModuleOf(reg.Find("server.cheats")) == "ArcaneCore");
}

TEST_CASE("module capture: a module's callbacks leave with it; built-in commands survive any unload", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("cap.engineKnob", CVarValue::Int32(0), Audience::Game, CVarFlags::None, "ArcaneClient"));
    int fires = 0;
    {
        const CVarModuleScope scope("GameA");
        reg.AddCallback(knob, [](CVarHandle, void* u) { ++*static_cast<int*>(u); }, &fires);
    }
    reg.AddCallback(knob, [](CVarHandle, void* u) { *static_cast<int*>(u) += 100; }, &fires);   // unscoped: the host's own
    reg.UnregisterModule("GameA");
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 100);                              // GameA's callback is gone, the host's stays

    reg.UnregisterModule("ArcaneCore");
    CHECK(reg.Execute("cvarlist", CVarContext::Editor).ok);
    CHECK(reg.Execute("cvar_explain cap.engineKnob", CVarContext::Editor).ok);
}

TEST_CASE("an untagged AddCallback whose fn lies in a published module image is attributed to that module", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("img.knob", CVarValue::Int32(0)));
    int fires = 0;
    CVarRegistry::ChangeFn fn = [](CVarHandle, void* u) { ++*static_cast<int*>(u); };
    reg.RegisterModuleImage("img-mod", reinterpret_cast<const void*>(fn), 16);
    reg.AddCallback(knob, fn, &fires);   // no CVarModuleScope
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 1);
    reg.UnregisterModule("img-mod");
    REQUIRE(reg.Set(knob, CVarValue::Int32(2), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 1);
}

TEST_CASE("UnregisterModuleRange drops callbacks by image address", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("img.rangeKnob", CVarValue::Int32(0)));
    int fires = 0;
    CVarRegistry::ChangeFn fn = [](CVarHandle, void* u) { ++*static_cast<int*>(u); };
    reg.AddCallback(knob, fn, &fires);   // unscoped, unpublished image
    CHECK(reg.UnregisterModuleRange(reinterpret_cast<const void*>(fn), 16) >= 1);
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(fires == 0);
}

TEST_CASE("UnregisterModuleRange does not drop another image's same-named registrations", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle knob = reg.Register(Test::Desc("img.sharedKnob", CVarValue::Int32(0)));
    int firesA = 0;
    int firesB = 0;
    CVarRegistry::ChangeFn fnA = [](CVarHandle, void* u) { ++*static_cast<int*>(u); };
    CVarRegistry::ChangeFn fnB = [](CVarHandle, void* u) { *static_cast<int*>(u) += 10; };
    REQUIRE(fnA != fnB);
    // Size 1 so the two fake images cannot overlap even if the thunks sit next
    // to each other in this test binary.
    reg.RegisterModuleImage("shared-stem", reinterpret_cast<const void*>(fnA), 1);
    reg.RegisterModuleImage("shared-stem", reinterpret_cast<const void*>(fnB), 1);
    reg.AddCallback(knob, fnA, &firesA);
    reg.AddCallback(knob, fnB, &firesB);
    CHECK(reg.UnregisterModuleRange(reinterpret_cast<const void*>(fnA), 1) >= 1);
    REQUIRE(reg.Set(knob, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(firesA == 0);
    CHECK(firesB == 10);
}
