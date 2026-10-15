// Every cvar type through the console and --set (settings spec 2026-10-03
// s4.1, s13 "a round-trip of all types ... through the console and --set").
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <limits>
#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    struct TypeCase
    {
        const char* name;
        CVarType type;
        CVarValue def;
        const char* text;      // what a person types
        CVarValue expect;      // what publishes
        const char* printed;   // what the console prints back
    };
}

TEST_CASE("console and --set round-trip all thirteen cvar types", "[cvar]")
{
    const std::vector<std::string> modes{ "Off", "Low", "High" };
    const TypeCase cases[] = {
        { "t.bool", CVarType::Bool, CVarValue::Bool(false), "true", CVarValue::Bool(true), "true" },
        { "t.i32", CVarType::Int32, CVarValue::Int32(0), "-7", CVarValue::Int32(-7), "-7" },
        { "t.u32", CVarType::UInt32, CVarValue::UInt32(0), "4000000000", CVarValue::UInt32(4000000000u), "4000000000" },
        { "t.i64", CVarType::Int64, CVarValue::Int64(0), "-9000000000", CVarValue::Int64(-9000000000LL), "-9000000000" },
        { "t.u64", CVarType::UInt64, CVarValue::UInt64(0), "18446744073709551615",
          CVarValue::UInt64(std::numeric_limits<std::uint64_t>::max()), "18446744073709551615" },
        { "t.f32", CVarType::Float32, CVarValue::Float32(0.0f), "1.5", CVarValue::Float32(1.5f), "1.500000" },
        { "t.f64", CVarType::Float64, CVarValue::Float64(0.0), "2.25", CVarValue::Float64(2.25), "2.250000" },
        { "t.str", CVarType::String, CVarValue::String(""), "hello", CVarValue::String("hello"), "hello" },
        { "t.color", CVarType::Color, CVarValue::Color(CVarColor{}), "#FF8800FF",
          CVarValue::Color(*CVarColorFromHex("#FF8800FF")), "#FF8800FF" },
        { "t.vec2", CVarType::Vec2, CVarValue::Vec2(CVarVec2{}), "1 2.5", CVarValue::Vec2(CVarVec2{ 1.0f, 2.5f }), "1 2.5" },
        { "t.vec3", CVarType::Vec3, CVarValue::Vec3(CVarVec3{}), "1,2,3", CVarValue::Vec3(CVarVec3{ 1.0f, 2.0f, 3.0f }), "1 2 3" },
        { "t.vec4", CVarType::Vec4, CVarValue::Vec4(CVarVec4{}), "[0.5 0.25 0 1]",
          CVarValue::Vec4(CVarVec4{ 0.5f, 0.25f, 0.0f, 1.0f }), "0.5 0.25 0 1" },
        { "t.enum", CVarType::Enum, CVarValue::Enum(0), "High", CVarValue::Enum(2), "High" },
    };
    for (const TypeCase& c : cases)
    {
        INFO(c.name);
        CVarDesc desc{ .name = c.name, .type = c.type, .defaultValue = c.def, .help = "round-trip probe", .module = "test" };
        if (c.type == CVarType::Enum) desc.enumNames = modes;

        CVarRegistry console;
        REQUIRE_FALSE(console.Register(desc).IsStale());
        const ExecResult set = console.Execute(std::string(c.name) + " " + c.text, CVarContext::Editor);
        INFO(set.text);
        REQUIRE(set.ok);
        console.Publish();
        CHECK(*console.Get(console.Find(c.name)) == c.expect);
        CHECK(console.Execute(c.name, CVarContext::Editor).text == std::string(c.name) + " = " + c.printed);

        CVarRegistry cli;
        REQUIRE_FALSE(cli.Register(desc).IsStale());
        ApplyCVarCommandLine(cli, { std::string(c.name) + "=" + c.text }, CVarContext::Editor);
        cli.Publish();
        CHECK(*cli.Get(cli.Find(c.name)) == c.expect);
        CHECK(cli.Explain(c.name)->setBy == SetBy::CommandLine);
    }
}

TEST_CASE("the console refuses a wrong shape with the parser's reason, and names Enums everywhere", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ .name = "t.vec3", .type = CVarType::Vec3, .defaultValue = CVarValue::Vec3(CVarVec3{}),
                                         .help = "shape probe", .module = "test" }).IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ .name = "t.mode", .type = CVarType::Enum, .defaultValue = CVarValue::Enum(1),
                                         .help = "mode probe", .module = "test", .enumNames = { "Off", "Low", "High" } }).IsStale());

    const ExecResult shortVec = reg.Execute("t.vec3 1 2", CVarContext::Editor);
    CHECK_FALSE(shortVec.ok);
    CHECK(shortVec.text == "expected 3 floats");
    const ExecResult badMode = reg.Execute("t.mode Medium", CVarContext::Editor);
    CHECK_FALSE(badMode.ok);
    CHECK(badMode.text == "expected one of: Off, Low, High");
    CHECK(reg.Execute("t.mode 2", CVarContext::Editor).ok);           // an in-range ordinal is accepted
    reg.Publish();
    CHECK(reg.Execute("t.mode", CVarContext::Editor).text == "t.mode = High");

    const ExecResult explained = reg.Execute("cvar_explain t.mode", CVarContext::Editor);
    REQUIRE(explained.ok);
    CHECK(explained.text.find("t.mode = High [Console]") != std::string::npos);
    CHECK(explained.text.find("Default = Low") != std::string::npos);

    const ExecResult listed = reg.Execute("cvarlist", CVarContext::Editor);
    CHECK(listed.text.find("t.mode (enum) mode probe") != std::string::npos);
    CHECK(listed.text.find("t.vec3 (vec3) shape probe") != std::string::npos);
}

// Review Focus 3 (plan 2026-10-03): nan / inf / 1.5abc through the float
// parser would publish a value that never compares equal (callbacks on every
// Publish, Modified and the reset arrow disagreeing after a reload). The
// console and --set refuse them for Float32 and Float64 with the parser's
// v1 reason; the finite forms a person types still parse.
TEST_CASE("the console and --set refuse non-finite and trailing-garbage floats", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ .name = "t.f32", .type = CVarType::Float32, .defaultValue = CVarValue::Float32(1.0f),
                                         .help = "float probe", .module = "test" }).IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ .name = "t.f64", .type = CVarType::Float64, .defaultValue = CVarValue::Float64(1.0),
                                         .help = "double probe", .module = "test" }).IsStale());

    for (const char* bad : { "nan", "inf", "-inf", "1.5abc", "abc", "1.5 2" })
    {
        INFO(bad);
        const ExecResult f32 = reg.Execute(std::string("t.f32 ") + bad, CVarContext::Editor);
        CHECK_FALSE(f32.ok);
        CHECK(f32.text == "expected float");
        const ExecResult f64 = reg.Execute(std::string("t.f64 ") + bad, CVarContext::Editor);
        CHECK_FALSE(f64.ok);
        CHECK(f64.text == "expected double");
    }
    ApplyCVarCommandLine(reg, { "t.f32=nan", "t.f64=2.5x" }, CVarContext::Editor);
    reg.Publish();
    CHECK(*reg.Get(reg.Find("t.f32")) == CVarValue::Float32(1.0f));   // the defaults stand
    CHECK(*reg.Get(reg.Find("t.f64")) == CVarValue::Float64(1.0));
    CHECK(reg.Explain("t.f32")->setBy == SetBy::Default);
    CHECK(reg.Explain("t.f64")->setBy == SetBy::Default);

    // The finite spellings a person types: exponent, sign, a bare fraction.
    REQUIRE(reg.Execute("t.f32 1e3", CVarContext::Editor).ok);
    REQUIRE(reg.Execute("t.f64 -2.5e-1", CVarContext::Editor).ok);
    reg.Publish();
    CHECK(*reg.Get(reg.Find("t.f32")) == CVarValue::Float32(1000.0f));
    CHECK(*reg.Get(reg.Find("t.f64")) == CVarValue::Float64(-0.25));
    REQUIRE(reg.Execute("t.f32 .5", CVarContext::Editor).ok);
    reg.Publish();
    CHECK(*reg.Get(reg.Find("t.f32")) == CVarValue::Float32(0.5f));
}
