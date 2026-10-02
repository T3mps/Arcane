// The reflection facade (input-seam spec 2026-10-02 s6.1): ARCANE_REFLECT_*
// produce the same metadata as ASTRA_REFLECT_*, every Astra attribute has an
// Arcane::Attr alias (Review Focus #5), and ARCANE_CHANGE_TRACKED spells
// Astra's change-tracking member.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Reflection.hpp>

#include <Astra/Reflection/MetaRegistry.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "Helpers/ReferenceProjectDir.hpp"

namespace FacadeProbe
{
    struct Probe
    {
        ARCANE_CHANGE_TRACKED
        float speed = 1.0f;
        float angle = 0.0f;
        bool  transient = false;
    };

    ARCANE_REFLECT_TYPE(Probe)
        ARCANE_REFLECT_FIELD(Probe, speed)
            ARCANE_REFLECT_ATTR(Range, 0.0f, 10.0f)
            ARCANE_REFLECT_ATTR(Tooltip, "metres per second")
            ARCANE_REFLECT_ATTR(Category, "Motion")
            ARCANE_REFLECT_ATTR(DragSpeed, 0.1f)
            ARCANE_REFLECT_ATTR(Precision, 2)
        ARCANE_REFLECT_FIELD(Probe, angle)
            ARCANE_REFLECT_ATTR(AngleFormat, Arcane::Attr::AngleFormat::Unit::Degrees)
        ARCANE_REFLECT_FIELD(Probe, transient)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
    ARCANE_END_REFLECT_TYPE()

    // The other spellings: a type-level attribute with the _END terminator, and
    // both enum terminators over every value macro.
    struct Tagged
    {
        int count = 0;
    };

    ARCANE_REFLECT_TYPE(Tagged)
        ARCANE_REFLECT_TYPE_ATTR(Category, "Probes")
        ARCANE_REFLECT_FIELD(Tagged, count)
            ARCANE_REFLECT_ATTR(ReadOnly)
    ARCANE_REFLECT_TYPE_END()

    enum class Mode : int { Idle, Walk, Run };

    ARCANE_REFLECT_ENUM(Mode)
        ARCANE_REFLECT_ENUM_VALUE(Mode, Idle)
        ARCANE_REFLECT_ENUM_VALUE_NAMED(Mode, Walk, "Walking")
        ARCANE_REFLECT_ENUM_VALUE_FULL(Mode, Run, "Running", "Fastest gait")
    ARCANE_END_REFLECT_ENUM()

    enum class Mask : int { None = 0, A = 1, B = 2 };

    ARCANE_REFLECT_ENUM(Mask)
        ARCANE_REFLECT_ENUM_FLAGS()
        ARCANE_REFLECT_ENUM_VALUE(Mask, A)
        ARCANE_REFLECT_ENUM_VALUE(Mask, B)
    ARCANE_REFLECT_ENUM_END()
}

TEST_CASE("ARCANE_REFLECT_* register the same metadata ASTRA_REFLECT_* would", "[facade][reflection]")
{
    STATIC_REQUIRE(FacadeProbe::Probe::AstraChangeTracked);
    const Astra::TypeMeta* meta = Astra::GetMeta(Astra::TypeID<FacadeProbe::Probe>::Hash());
    REQUIRE(meta);
    REQUIRE(meta->fields.size() == 3);
    const Astra::FieldInfo& speed = meta->fields[0];
    CHECK(speed.name == "speed");
    CHECK(speed.HasAttribute<Astra::Range>());
    CHECK(speed.HasAttribute<Astra::Tooltip>());
    CHECK(speed.HasAttribute<Astra::Category>());
    CHECK(speed.HasAttribute<Astra::DragSpeed>());
    CHECK(speed.HasAttribute<Astra::Precision>());
    const Astra::FieldInfo& angle = meta->fields[1];
    CHECK(angle.name == "angle");
    const Astra::AngleFormat* angleFormat = angle.GetAttribute<Astra::AngleFormat>();
    REQUIRE(angleFormat);
    CHECK(angleFormat->unit == Astra::AngleFormat::Unit::Degrees);
    const Astra::FieldInfo& transient = meta->fields[2];
    CHECK(transient.name == "transient");
    CHECK(transient.HasAttribute<Astra::Hidden>());
    CHECK(transient.HasAttribute<Astra::Serializable>());
}

TEST_CASE("ARCANE_REFLECT_TYPE_ATTR and the enum macros forward to Astra's", "[facade][reflection]")
{
    const Astra::TypeMeta* tagged = Astra::GetMeta(Astra::TypeID<FacadeProbe::Tagged>::Hash());
    REQUIRE(tagged);
    CHECK(tagged->HasAttribute<Astra::Category>());
    REQUIRE(tagged->fields.size() == 1);
    CHECK(tagged->fields[0].HasAttribute<Astra::ReadOnly>());

    const Astra::TypeMeta* mode = Astra::GetMeta(Astra::TypeID<FacadeProbe::Mode>::Hash());
    REQUIRE(mode);
    REQUIRE(mode->enumInfo);
    REQUIRE(mode->enumInfo->values.size() == 3);
    CHECK(mode->enumInfo->values[0].name == "Idle");
    CHECK(mode->enumInfo->values[1].displayName == "Walking");
    CHECK(mode->enumInfo->values[2].displayName == "Running");
    CHECK(mode->enumInfo->values[2].description == "Fastest gait");
    CHECK_FALSE(mode->enumInfo->isFlags);

    const Astra::TypeMeta* mask = Astra::GetMeta(Astra::TypeID<FacadeProbe::Mask>::Hash());
    REQUIRE(mask);
    REQUIRE(mask->enumInfo);
    CHECK(mask->enumInfo->isFlags);
    CHECK(mask->enumInfo->values.size() == 2);
}

// Review Focus #5: Astra grows an attribute, the facade misses it, and a game
// author's ARCANE_REFLECT_ATTR fails to compile. The vendored Attribute.hpp is
// the list; Arcane/Reflection.hpp must alias every entry.
TEST_CASE("every Astra reflection attribute has an Arcane::Attr alias", "[facade][reflection]")
{
    const std::filesystem::path root = Arcane::Test::FindReferenceProjectDir().parent_path();
    auto slurp = [](const std::filesystem::path& p)
    {
        std::ifstream in(p);
        REQUIRE(in);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    };
    const std::string attributes = slurp(root / "ThirdParty/Astra/include/Astra/Reflection/Attribute.hpp");
    const std::string facade     = slurp(root / "ArcaneCore/src/Arcane/Reflection.hpp");

    // Anchored to a line that starts with the declaration, so the doc comment's
    // " * struct MyAttribute : AttributeBase<MyAttribute>" example is not a match.
    std::set<std::string> astra;
    const std::regex decl(R"(\n[ \t]*struct\s+(\w+)\s*:\s*AttributeBase<)");
    for (std::sregex_iterator it(attributes.begin(), attributes.end(), decl), end; it != end; ++it)
        astra.insert((*it)[1].str());
    REQUIRE(astra.size() >= 15);

    for (const std::string& name : astra)
    {
        INFO("Astra attribute without an Arcane::Attr alias: " << name);
        CHECK(facade.find("using ::Astra::" + name + ";") != std::string::npos);
    }
}
