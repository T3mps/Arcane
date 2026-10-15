// Fuzz regressions for the reflection JSON reader's integer fields
// (fuzz/scene_fuzz.cpp, inputs in fuzz/regressions/scene). A JSON number the
// field cannot hold -- a double past the integer range (nlohmann stores an
// integer literal beyond 2^64 as one), or a negative into an unsigned field --
// is Malformed and latches, instead of a float->integer conversion out of
// range (undefined behaviour) or a silent wrap.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <cstdint>
#include <string>

namespace
{
    // Reads `idJson` into a fresh Identity's `id` (a Guid: uint64 hi/lo) through
    // the reflection reader. Returns whether the reader latched.
    bool IdentityIdLatches(const nlohmann::json& idJson, Arcane::Identity& out)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::Identity>();
        REQUIRE(meta != nullptr);
        nlohmann::json j;
        j["id"] = idJson;
        j["name"] = "n";
        Arcane::ReflectionJsonReader reader(j);
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.IsSerializable())
                reader.Visit(f, &out);
        return reader.HasError();
    }
}

TEST_CASE("ReflectionJson reader refuses an integer field value it cannot hold", "[serialization][negative][reflection][fuzz]")
{
    Arcane::Identity e;

    SECTION("a literal past 2^64 (parsed as a double) into uint64")
    {
        CHECK(IdentityIdLatches(nlohmann::json::parse(R"({"hi": 35089691696507535956, "lo": 1})"), e));
    }
    SECTION("a double past 2^64 into uint64")
    {
        CHECK(IdentityIdLatches(nlohmann::json{ { "hi", 1.0e20 }, { "lo", 1 } }, e));
    }
    SECTION("a negative into uint64")
    {
        CHECK(IdentityIdLatches(nlohmann::json{ { "hi", 1 }, { "lo", -1 } }, e));
        CHECK(IdentityIdLatches(nlohmann::json{ { "hi", 1 }, { "lo", -0.5e1 } }, e));
    }
    SECTION("in-range values still read, including the uint64 maximum")
    {
        CHECK_FALSE(IdentityIdLatches(nlohmann::json{ { "hi", 18446744073709551615ull }, { "lo", 2 } }, e));
        CHECK(e.id.hi == 18446744073709551615ull);
        CHECK(e.id.lo == 2);
    }
}

TEST_CASE("Scene LoadJson refuses the minimized fuzz document cleanly", "[serialization][negative][scene][fuzz]")
{
    // fuzz/regressions/scene/identity-id-out-of-range-double, verbatim.
    const nlohmann::json doc = nlohmann::json::parse(
        R"({"entities":[{"components":{":Cr":{},"Arcane::Identity":{"id":{"hi":35089691696507535956},"n":[2]}},"t":0}],"id":"10d8ff0645c247f2-90cbab7ac29cb51d","version":6})");

    Astra::Registry reg;
    Arcane::RegisterSceneComponents(reg);
    CHECK_FALSE(Arcane::Scene::LoadJson(reg, doc));
}
