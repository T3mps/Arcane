// The text forms of cvar values (settings spec 2026-10-03 s4.1, s4.8): the
// sRGB "#RRGGBBAA" colour hex, the console's spelling of every type, and the
// console/--set parser with its refusal reasons.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarFormat.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace Arcane;

TEST_CASE("cvar colour hex: sRGB-encoded #RRGGBBAA, either case in, upper case out, alpha linear", "[cvar]")
{
    CHECK(CVarColorToHex(CVarColor{ 1.0f, 1.0f, 1.0f, 1.0f }) == "#FFFFFFFF");
    CHECK(CVarColorToHex(CVarColor{ 0.0f, 0.0f, 0.0f, 0.0f }) == "#00000000");
    CHECK(CVarColorToHex(CVarColor{ 0.5f, 0.5f, 0.5f, 0.5f }) == "#BCBCBC80");   // linear 0.5 -> sRGB 0.7354 -> 0xBC
    CHECK(CVarColorToHex(CVarColor{ 2.0f, -1.0f, 0.0f, 1.0f }) == "#FF0000FF");  // outside [0,1] clamps

    const auto orange = CVarColorFromHex("#ff8800");
    REQUIRE(orange.has_value());
    CHECK(orange->r == 1.0f);
    CHECK(orange->b == 0.0f);
    CHECK(orange->a == 1.0f);                       // #RRGGBB is opaque
    CHECK(orange->g > 0.24f);                       // sRGB 0x88 decodes to linear ~0.2462
    CHECK(orange->g < 0.25f);
    CHECK(CVarColorToHex(*orange) == "#FF8800FF");

    CHECK_FALSE(CVarColorFromHex("ff8800").has_value());       // the '#' is required
    CHECK_FALSE(CVarColorFromHex("#ff880").has_value());
    CHECK_FALSE(CVarColorFromHex("#gg8800").has_value());
    CHECK_FALSE(CVarColorFromHex("#ff8800ff00").has_value());
}

TEST_CASE("cvar colour hex round-trips every 8-bit channel value exactly", "[cvar]")
{
    for (unsigned b = 0; b < 256; ++b)
    {
        char hex[10];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", b, 255u - b, b, b);
        const auto c = CVarColorFromHex(hex);
        REQUIRE(c.has_value());
        CHECK(CVarColorToHex(*c) == hex);
    }
}

TEST_CASE("FormatCVarValue: the console spelling of every type", "[cvar]")
{
    const std::vector<std::string> modes{ "Off", "Low", "High" };
    CHECK(FormatCVarValue(CVarValue::Bool(true)) == "true");
    CHECK(FormatCVarValue(CVarValue::Int32(-7)) == "-7");
    CHECK(FormatCVarValue(CVarValue::UInt64(std::numeric_limits<std::uint64_t>::max())) == "18446744073709551615");
    CHECK(FormatCVarValue(CVarValue::Float32(1.5f)) == "1.500000");        // v1's std::to_string, unchanged
    CHECK(FormatCVarValue(CVarValue::Float64(2.25)) == "2.250000");
    CHECK(FormatCVarValue(CVarValue::String("a b")) == "a b");
    CHECK(FormatCVarValue(CVarValue::Color(CVarColor{ 1.0f, 1.0f, 1.0f, 1.0f })) == "#FFFFFFFF");
    CHECK(FormatCVarValue(CVarValue::Vec2(CVarVec2{ 0.1f, 2.5f })) == "0.1 2.5");   // shortest round-trip
    CHECK(FormatCVarValue(CVarValue::Vec3(CVarVec3{ 1.0f, 2.0f, 3.0f })) == "1 2 3");
    CHECK(FormatCVarValue(CVarValue::Vec4(CVarVec4{ 0.0f, -0.5f, 1.0f, 2.0f })) == "0 -0.5 1 2");
    CHECK(FormatCVarValue(CVarValue::Enum(2), modes) == "High");
    CHECK(FormatCVarValue(CVarValue::Enum(9), modes) == "9");              // out of range: the digits
    CHECK(std::string(CVarTypeName(CVarType::Float32)) == "float");
    CHECK(std::string(CVarTypeName(CVarType::Color)) == "color");
    CHECK(std::string(CVarTypeName(CVarType::Enum)) == "enum");
}

TEST_CASE("ParseCVarText: v1 types unchanged; Color, Vec and Enum forms; refusals name the shape", "[cvar]")
{
    const std::vector<std::string> modes{ "Off", "Low", "High" };
    std::string error;
    const auto ok = [&](std::string_view text, CVarType type) {
        error.clear();
        const auto v = ParseCVarText(text, type, modes, error);
        REQUIRE(v.has_value());
        return *v;
    };
    const auto refused = [&](std::string_view text, CVarType type) {
        error.clear();
        return !ParseCVarText(text, type, modes, error).has_value();
    };

    CHECK(ok("1", CVarType::Bool) == CVarValue::Bool(true));
    CHECK(ok("false", CVarType::Bool) == CVarValue::Bool(false));
    CHECK(refused("yes", CVarType::Bool));
    CHECK(error == "expected true or false");
    CHECK(ok("-12", CVarType::Int32) == CVarValue::Int32(-12));
    CHECK(refused("x", CVarType::Int32));
    CHECK(error == "expected int32");
    CHECK(ok("2.5", CVarType::Float64) == CVarValue::Float64(2.5));
    CHECK(ok("hello world", CVarType::String) == CVarValue::String("hello world"));

    CHECK(ok("#FF8800FF", CVarType::Color) == CVarValue::Color(*CVarColorFromHex("#FF8800FF")));
    CHECK(ok("0.5 0.25 1", CVarType::Color) == CVarValue::Color(CVarColor{ 0.5f, 0.25f, 1.0f, 1.0f }));   // linear, alpha 1
    CHECK(ok("0.5, 0.25, 1, 0.5", CVarType::Color) == CVarValue::Color(CVarColor{ 0.5f, 0.25f, 1.0f, 0.5f }));
    CHECK(refused("#12", CVarType::Color));
    CHECK(error.find("#RRGGBB") != std::string::npos);

    CHECK(ok("1 2.5", CVarType::Vec2) == CVarValue::Vec2(CVarVec2{ 1.0f, 2.5f }));
    CHECK(ok("[1, 2, 3]", CVarType::Vec3) == CVarValue::Vec3(CVarVec3{ 1.0f, 2.0f, 3.0f }));
    CHECK(ok("1,2,3,4", CVarType::Vec4) == CVarValue::Vec4(CVarVec4{ 1.0f, 2.0f, 3.0f, 4.0f }));
    CHECK(refused("1 2", CVarType::Vec3));
    CHECK(error == "expected 3 floats");
    CHECK(refused("1 nan", CVarType::Vec2));      // non-finite refused
    CHECK(refused("1 2x", CVarType::Vec2));       // a whole field or nothing

    CHECK(ok("High", CVarType::Enum) == CVarValue::Enum(2));
    CHECK(ok("1", CVarType::Enum) == CVarValue::Enum(1));       // an in-range ordinal
    CHECK(refused("high", CVarType::Enum));                      // names are exact
    CHECK(refused("3", CVarType::Enum));
    CHECK(error == "expected one of: Off, Low, High");
    CHECK(CVarEnumOrdinal(modes, "Low") == std::optional<std::int32_t>(1));
    CHECK_FALSE(CVarEnumOrdinal(modes, "Medium").has_value());
}
