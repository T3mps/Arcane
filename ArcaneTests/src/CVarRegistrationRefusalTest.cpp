// Register's refusals (settings spec 2026-10-03 s4.2, s4.5; O4): a cvar that
// would be half-formed -- no help, a default or bound of the wrong type, an
// empty range, an Enum without usable names -- is refused, not stored.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <string>
#include <string_view>
#include <utility>

using namespace Arcane;

namespace
{
    CVarDesc Desc(std::string_view name, CVarType type, CVarValue def)
    {
        CVarDesc d;
        d.name = name;
        d.type = type;
        d.defaultValue = std::move(def);
        d.help = "refusal probe";
        d.module = "test";
        return d;
    }

    bool Refused(CVarRegistry& reg, const CVarDesc& d, std::string_view why)
    {
        const bool stale = reg.Register(d).IsStale();
        INFO(d.name << ": " << reg.LastError());
        CHECK(reg.LastError().find(why) != std::string::npos);
        CHECK(reg.LastError().find(std::string(d.name)) != std::string::npos);
        return stale && reg.Find(d.name).IsStale();
    }
}

TEST_CASE("Register refuses empty help unless the cvar is Hidden", "[cvar]")
{
    CVarRegistry reg;
    CVarDesc noHelp = Desc("r.noHelp", CVarType::Int32, CVarValue::Int32(1));
    noHelp.help = {};
    CHECK(Refused(reg, noHelp, "help"));
    noHelp.name = "r.hiddenNoHelp";
    noHelp.flags = CVarFlags::Hidden;
    CHECK_FALSE(reg.Register(noHelp).IsStale());
}

TEST_CASE("Register refuses a default or bound of another type, and an empty range", "[cvar]")
{
    CVarRegistry reg;
    CHECK(Refused(reg, Desc("r.wrongDefault", CVarType::Int32, CVarValue::Float32(1.0f)), "default"));   // O4

    CVarDesc wrongBound = Desc("r.wrongBound", CVarType::Int32, CVarValue::Int32(1));
    wrongBound.min = CVarValue::Float32(0.0f);
    CHECK(Refused(reg, wrongBound, "bound"));

    CVarDesc empty = Desc("r.empty", CVarType::Int32, CVarValue::Int32(5));
    empty.min = CVarValue::Int32(10);
    empty.max = CVarValue::Int32(1);
    CHECK(Refused(reg, empty, "min > max"));

    CVarDesc vecEmpty = Desc("r.vecEmpty", CVarType::Vec2, CVarValue::Vec2(CVarVec2{}));
    vecEmpty.min = CVarValue::Vec2(CVarVec2{ 0.0f, 5.0f });
    vecEmpty.max = CVarValue::Vec2(CVarVec2{ 1.0f, 1.0f });
    CHECK(Refused(reg, vecEmpty, "min > max"));                      // per component

    CVarDesc vecOk = Desc("r.vecOk", CVarType::Vec2, CVarValue::Vec2(CVarVec2{}));
    vecOk.min = CVarValue::Vec2(CVarVec2{ -1.0f, -1.0f });
    vecOk.max = CVarValue::Vec2(CVarVec2{ 1.0f, 1.0f });
    CHECK_FALSE(reg.Register(vecOk).IsStale());

    CVarDesc boolRange = Desc("r.boolRange", CVarType::Bool, CVarValue::Bool(false));
    boolRange.max = CVarValue::Bool(true);
    CHECK(Refused(reg, boolRange, "no min/max"));
    CVarDesc textRange = Desc("r.textRange", CVarType::String, CVarValue::String("a"));
    textRange.min = CVarValue::String("a");
    CHECK(Refused(reg, textRange, "no min/max"));
}

TEST_CASE("Register refuses an Enum without usable names, and Set refuses an ordinal outside them", "[cvar]")
{
    CVarRegistry reg;
    CHECK(Refused(reg, Desc("r.enumNoNames", CVarType::Enum, CVarValue::Enum(0)), "no names"));

    CVarDesc dup = Desc("r.enumDup", CVarType::Enum, CVarValue::Enum(0));
    dup.enumNames = { "A", "A" };
    CHECK(Refused(reg, dup, "twice"));
    CVarDesc blank = Desc("r.enumBlank", CVarType::Enum, CVarValue::Enum(0));
    blank.enumNames = { "A", "" };
    CHECK(Refused(reg, blank, "empty Enum name"));
    CVarDesc outside = Desc("r.enumDefault", CVarType::Enum, CVarValue::Enum(3));
    outside.enumNames = { "A", "B" };
    CHECK(Refused(reg, outside, "outside"));
    CVarDesc namesOnInt = Desc("r.namesOnInt", CVarType::Int32, CVarValue::Int32(0));
    namesOnInt.enumNames = { "A" };
    CHECK(Refused(reg, namesOnInt, "Enum names"));
    CVarDesc enumRange = Desc("r.enumRange", CVarType::Enum, CVarValue::Enum(0));
    enumRange.enumNames = { "A", "B" };
    enumRange.min = CVarValue::Enum(0);
    CHECK(Refused(reg, enumRange, "no min/max"));

    CVarDesc modeDesc = Desc("r.mode", CVarType::Enum, CVarValue::Enum(1));
    modeDesc.enumNames = { "Off", "On" };
    const CVarHandle mode = reg.Register(modeDesc);
    REQUIRE_FALSE(mode.IsStale());
    CHECK(reg.Set(mode, CVarValue::Enum(2), SetBy::Code) == SetResult::TypeMismatch);
    CHECK(reg.Set(mode, CVarValue::Enum(-1), SetBy::Code) == SetResult::TypeMismatch);
    CHECK(reg.Set(mode, CVarValue::Int32(0), SetBy::Code) == SetResult::TypeMismatch);
    CHECK(reg.Set(mode, CVarValue::Enum(0), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Get(mode)->AsEnum() == 0);
}

TEST_CASE("every S1 type registers and publishes", "[cvar]")
{
    CVarRegistry reg;
    CHECK_FALSE(reg.Register(Desc("t.color", CVarType::Color, CVarValue::Color(CVarColor{ 1.0f, 0.0f, 0.0f, 1.0f }))).IsStale());
    CHECK_FALSE(reg.Register(Desc("t.vec3", CVarType::Vec3, CVarValue::Vec3(CVarVec3{ 1.0f, 2.0f, 3.0f }))).IsStale());
    CHECK_FALSE(reg.Register(Desc("t.vec4", CVarType::Vec4, CVarValue::Vec4(CVarVec4{}))).IsStale());
    CHECK(reg.Get(reg.Find("t.vec3"))->AsVec3() == CVarVec3{ 1.0f, 2.0f, 3.0f });
    REQUIRE(reg.Set(reg.Find("t.color"), CVarValue::Color(CVarColor{ 0.0f, 1.0f, 0.0f, 1.0f }), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Get(reg.Find("t.color"))->AsColor() == CVarColor{ 0.0f, 1.0f, 0.0f, 1.0f });
}
