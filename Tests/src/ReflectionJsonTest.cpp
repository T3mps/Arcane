// The reflection->JSON bridge drives the Astra 3.2 visitFields seam to round-trip
// a component through nlohmann::json, with no format knowledge in Astra. glm math
// types serialize as arrays; AliasName recovers a renamed field.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Json.hpp>

#include <string>
#include <vector>

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

namespace { enum class Mode { Off, On, Auto }; struct Knob { Mode mode = Mode::Off; int level = 0; }; }

ASTRA_REFLECT_ENUM(Mode)
    ASTRA_REFLECT_ENUM_VALUE(Mode, Off)
    ASTRA_REFLECT_ENUM_VALUE(Mode, On)
    ASTRA_REFLECT_ENUM_VALUE(Mode, Auto)
ASTRA_END_REFLECT_ENUM()

ASTRA_REFLECT_TYPE(Knob)
    ASTRA_REFLECT_FIELD(Knob, mode)
    ASTRA_REFLECT_FIELD(Knob, level)
ASTRA_END_REFLECT_TYPE()

TEST_CASE("enum field round-trips by name through JSON", "[json]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Knob>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Knob>::Value());
    REQUIRE(desc != nullptr);

    Knob a; a.mode = Mode::Auto; a.level = 3;
    nlohmann::json j;
    Arcane::ReflectionJsonWriter writer(j);
    desc->visitFields(&a, writer);
    CHECK(j["mode"].is_string());          // enum serialized by NAME, not {} or a number
    CHECK(j["mode"].get<std::string>() == "Auto");

    Knob b;
    Arcane::ReflectionJsonReader reader(j);
    desc->visitFields(&b, reader);
    CHECK(b.level == 3);
    CHECK(b.mode == Mode::Auto);
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

// E02-3: matrices and quaternions were previously silently dropped (written as
// nothing, read as default). They now round-trip as flat JSON number arrays.
namespace
{
    struct Xform
    {
        glm::mat3 m3{1.0f};
        glm::mat4 m4{1.0f};
        glm::quat q{1.0f, 0.0f, 0.0f, 0.0f};   // (w, x, y, z)
    };
}

ASTRA_REFLECT_TYPE(Xform)
    ASTRA_REFLECT_FIELD(Xform, m3)
    ASTRA_REFLECT_FIELD(Xform, m4)
    ASTRA_REFLECT_FIELD(Xform, q)
ASTRA_END_REFLECT_TYPE()

TEST_CASE("mat3/mat4/quat fields round-trip through JSON", "[json][reflection]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Xform>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Xform>::Value());
    REQUIRE(desc != nullptr);

    Xform a;
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) a.m3[c][r] = static_cast<float>(c * 3 + r + 1);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) a.m4[c][r] = static_cast<float>(c * 4 + r + 1);
    a.q = glm::quat(0.1f, 0.2f, 0.3f, 0.4f);   // (w, x, y, z)

    nlohmann::json j;
    Arcane::ReflectionJsonWriter writer(j);
    desc->visitFields(&a, writer);
    REQUIRE_FALSE(writer.HasError());               // no silent drop
    CHECK(j["m3"].is_array());
    CHECK(j["m3"].size() == 9);
    CHECK(j["m4"].size() == 16);
    CHECK(j["q"].size() == 4);
    CHECK(j["q"][0].get<float>() == Approx(0.2f));   // x, y, z, w order

    Xform b;
    Arcane::ReflectionJsonReader reader(j);
    desc->visitFields(&b, reader);
    REQUIRE_FALSE(reader.HasError());
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r)
        CHECK(b.m3[c][r] == Approx(a.m3[c][r]));
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        CHECK(b.m4[c][r] == Approx(a.m4[c][r]));
    CHECK(b.q.x == Approx(0.2f));
    CHECK(b.q.y == Approx(0.3f));
    CHECK(b.q.z == Approx(0.4f));
    CHECK(b.q.w == Approx(0.1f));
}

// E02-3: a field whose TYPE the bridge cannot represent (a container here) must
// fail loud with a clear diagnostic, not silently drop the field.
namespace
{
    struct HasVector
    {
        int id = 0;
        std::vector<int> data;
    };
}

ASTRA_REFLECT_TYPE(HasVector)
    ASTRA_REFLECT_FIELD(HasVector, id)
    ASTRA_REFLECT_FIELD(HasVector, data)
ASTRA_END_REFLECT_TYPE()

// HasVector cannot be a binary-serializable component (the std::vector<int> field
// has no trivially-copyable path), so drive the visitor straight over its reflected
// TypeMeta -- reflection metadata, not component registration, is all the JSON
// bridge needs.
namespace
{
    template<typename Visitor>
    void VisitMetaFields(const Astra::TypeMeta& meta, void* instance, Visitor& visitor)
    {
        for (const Astra::FieldInfo& f : meta.fields)
            if (f.IsSerializable())
                visitor.Visit(f, instance);
    }
}

TEST_CASE("unsupported field type fails loud instead of silently dropping", "[json][reflection]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<HasVector>();
    REQUIRE(meta != nullptr);

    SECTION("writer latches an error naming the field")
    {
        HasVector a; a.id = 5; a.data = {1, 2, 3};
        nlohmann::json j;
        Arcane::ReflectionJsonWriter writer(j);
        VisitMetaFields(*meta, &a, writer);
        REQUIRE(writer.HasError());
        CHECK(writer.Error().find("data") != std::string::npos);
        CHECK(j["id"].get<int>() == 5);   // the supported field before it still wrote
    }

    SECTION("reader latches an error even when the key is absent")
    {
        // The unsupported TYPE must be reported regardless of whether the JSON
        // carries the key -- otherwise a dropped field masquerades as forward-compat.
        nlohmann::json j; j["id"] = 7;
        HasVector out;
        Arcane::ReflectionJsonReader reader(j);
        VisitMetaFields(*meta, &out, reader);
        REQUIRE(reader.HasError());
        CHECK(reader.Error().find("data") != std::string::npos);
    }
}
