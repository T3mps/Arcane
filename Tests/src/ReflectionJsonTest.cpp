// The reflection->JSON bridge drives the Astra 3.2 visitFields seam to round-trip
// a component through nlohmann::json, with no format knowledge in Astra. glm math
// types serialize as arrays; AliasName recovers a renamed field.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <glm/glm.hpp>
#include <Json.hpp>

using Catch::Approx;

namespace
{
    struct Widget
    {
        int       count = 0;
        float     ratio = 0.0f;
        glm::vec2 offset{0.0f, 0.0f};
        glm::vec4 color{0.0f, 0.0f, 0.0f, 0.0f};
    };

    struct Versioned { int amount = 0; };
}

ASTRA_REFLECT_TYPE(Widget)
    ASTRA_REFLECT_FIELD(Widget, count)
    ASTRA_REFLECT_FIELD(Widget, ratio)
    ASTRA_REFLECT_FIELD(Widget, offset)
    ASTRA_REFLECT_FIELD(Widget, color)
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_TYPE(Versioned)
    ASTRA_REFLECT_FIELD(Versioned, amount)
        ASTRA_REFLECT_ATTR(AliasName, "value")   // renamed from "value"
ASTRA_END_REFLECT_TYPE()

TEST_CASE("component round-trips through JSON via the visitFields seam", "[json]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Widget>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Widget>::Value());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->visitFields != nullptr);

    Widget a;
    a.count = 7; a.ratio = 1.25f; a.offset = glm::vec2(3, 4); a.color = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);

    nlohmann::json j;
    Arcane::ReflectionJsonWriter writer(j);
    desc->visitFields(&a, writer);

    CHECK(j["count"].get<int>() == 7);
    CHECK(j["offset"].is_array());
    CHECK(j["offset"][1].get<float>() == Approx(4.0f));

    Widget b;
    Arcane::ReflectionJsonReader reader(j);
    desc->visitFields(&b, reader);
    CHECK(b.count == 7);
    CHECK(b.ratio == Approx(1.25f));
    CHECK(b.offset.x == Approx(3.0f));
    CHECK(b.color.w == Approx(1.0f));
}

TEST_CASE("AliasName recovers a value written under the old field name", "[json]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Versioned>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Versioned>::Value());
    REQUIRE(desc != nullptr);

    nlohmann::json j;
    j["value"] = 55;   // legacy key

    Versioned v;
    Arcane::ReflectionJsonReader reader(j);
    desc->visitFields(&v, reader);
    CHECK(v.amount == 55);
}
