// The S1 value types (settings spec 2026-10-03 s4.1): Color and Vec2/3/4 gain
// storage, Enum is an Int32 ordinal with its own type tag, and Clamp works per
// component against same-typed bounds.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarTypes.hpp>

using namespace Arcane;

TEST_CASE("cvar Color/Vec/Enum values store, compare, and keep their ordinals", "[cvar]")
{
    CHECK(static_cast<int>(CVarType::Color) == 8);
    CHECK(static_cast<int>(CVarType::Vec4) == 11);
    CHECK(static_cast<int>(CVarType::Enum) == 12);

    const CVarValue c = CVarValue::Color(CVarColor{ 0.25f, 0.5f, 0.75f, 1.0f });
    REQUIRE(c.type == CVarType::Color);
    CHECK(c.AsColor() == CVarColor{ 0.25f, 0.5f, 0.75f, 1.0f });
    CHECK(c == CVarValue::Color(CVarColor{ 0.25f, 0.5f, 0.75f, 1.0f }));
    CHECK_FALSE(c == CVarValue::Color(CVarColor{ 0.25f, 0.5f, 0.75f, 0.5f }));
    CHECK(CVarColor{}.a == 1.0f);                                   // an opaque default

    CHECK(CVarValue::Vec2(CVarVec2{ 1.0f, 2.0f }).AsVec2() == CVarVec2{ 1.0f, 2.0f });
    CHECK(CVarValue::Vec3(CVarVec3{ 1.0f, 2.0f, 3.0f }).AsVec3() == CVarVec3{ 1.0f, 2.0f, 3.0f });
    CHECK(CVarValue::Vec4(CVarVec4{ 1.0f, 2.0f, 3.0f, 4.0f }).AsVec4() == CVarVec4{ 1.0f, 2.0f, 3.0f, 4.0f });
    CHECK_FALSE(CVarValue::Vec2(CVarVec2{}) == CVarValue::Vec3(CVarVec3{}));   // the tag differs

    const CVarValue e = CVarValue::Enum(2);
    CHECK(e.type == CVarType::Enum);
    CHECK(e.AsEnum() == 2);
    CHECK_FALSE(e == CVarValue::Int32(2));                          // same storage, different type
}

TEST_CASE("Clamp works per component for Vec and Color and leaves Enum alone", "[cvar]")
{
    const CVarValue v2 = Clamp(CVarValue::Vec2(CVarVec2{ -5.0f, 50.0f }),
                               CVarValue::Vec2(CVarVec2{ 0.0f, 0.0f }), CVarValue::Vec2(CVarVec2{ 10.0f, 10.0f }));
    CHECK(v2.AsVec2() == CVarVec2{ 0.0f, 10.0f });

    const CVarValue v4 = Clamp(CVarValue::Vec4(CVarVec4{ 1.0f, -1.0f, 9.0f, 0.5f }),
                               CVarValue::Vec4(CVarVec4{ 0.0f, 0.0f, 0.0f, 0.0f }), CVarValue::Vec4(CVarVec4{ 2.0f, 2.0f, 2.0f, 2.0f }));
    CHECK(v4.AsVec4() == CVarVec4{ 1.0f, 0.0f, 2.0f, 0.5f });

    const CVarValue col = Clamp(CVarValue::Color(CVarColor{ 1.5f, -0.5f, 0.5f, 2.0f }),
                                CVarValue::Color(CVarColor{ 0.0f, 0.0f, 0.0f, 0.0f }), CVarValue::Color(CVarColor{ 1.0f, 1.0f, 1.0f, 1.0f }));
    CHECK(col.AsColor() == CVarColor{ 1.0f, 0.0f, 0.5f, 1.0f });

    // A mismatched bound is ignored (v1 rule), and Enum is never range-clamped.
    CHECK(Clamp(CVarValue::Vec3(CVarVec3{ 5.0f, 5.0f, 5.0f }), CVarValue::Int32(0), CVarValue::Int32(1)).AsVec3()
          == CVarVec3{ 5.0f, 5.0f, 5.0f });
    CHECK(Clamp(CVarValue::Enum(7), CVarValue::Enum(0), CVarValue::Enum(1)).AsEnum() == 7);
}
