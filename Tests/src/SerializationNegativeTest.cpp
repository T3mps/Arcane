// E02-5: serialization negative-input suite. Every corrupt / truncated /
// wrong-typed / missing-key / version-mismatch input must return a clean error
// result -- no throw, no crash -- across all three serialization surfaces:
// the snapshot frame, the resource section, scene JSON, and the reflection
// reader. Happy-path round-trips live next to their subjects; this file is the
// adversarial-input coverage.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>
#include <Arcane/Serialization/RegistrySnapshot.hpp>
#include <Arcane/Serialization/ResourceSerialization.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <glm/glm.hpp>
#include <Json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using Catch::Approx;
namespace SR = Arcane::Serialization;

// ---------------------------------------------------------------------------
// Snapshot frame (ParseSnapshot)
// ---------------------------------------------------------------------------

TEST_CASE("ParseSnapshot rejects malformed frames with a clean error", "[serialization][negative]")
{
    SECTION("empty buffer -> CorruptedData")
    {
        std::vector<std::byte> empty;
        auto r = SR::ParseSnapshot(empty);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::CorruptedData);
    }

    SECTION("shorter than the header -> CorruptedData")
    {
        std::vector<std::byte> b(SR::kSnapshotHeaderSize - 1, std::byte{0});
        auto r = SR::ParseSnapshot(b);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::CorruptedData);
    }

    SECTION("wrong magic -> InvalidMagic")
    {
        std::vector<std::byte> b(SR::kSnapshotHeaderSize, std::byte{0xEE});
        auto r = SR::ParseSnapshot(b);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::InvalidMagic);
    }

    SECTION("wrong version -> UnsupportedVersion")
    {
        std::vector<std::byte> b;
        Astra::BinaryWriter w(b);
        w(SR::kSnapshotMagic);
        w(static_cast<uint16_t>(SR::kSnapshotVersion + 999));
        w(static_cast<uint32_t>(0));
        auto r = SR::ParseSnapshot(b);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::UnsupportedVersion);
    }

    SECTION("registryLen exceeds the buffer -> CorruptedData")
    {
        std::vector<std::byte> b;
        Astra::BinaryWriter w(b);
        w(SR::kSnapshotMagic);
        w(SR::kSnapshotVersion);
        w(static_cast<uint32_t>(1000));   // claims 1000 bytes; none follow
        auto r = SR::ParseSnapshot(b);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::CorruptedData);
    }
}

TEST_CASE("RestoreRegistry rejects corrupt/garbage input without crashing", "[serialization][negative][runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());

    SECTION("empty buffer")
    {
        std::vector<std::byte> empty;
        CHECK_FALSE(rt.RestoreRegistry(empty));
    }

    SECTION("garbage bytes")
    {
        std::vector<std::byte> g(64, std::byte{0xAB});
        CHECK_FALSE(rt.RestoreRegistry(g));
    }

    SECTION("valid frame wrapping a corrupt registry blob")
    {
        std::vector<std::byte> b;
        Astra::BinaryWriter w(b);
        w(SR::kSnapshotMagic);
        w(SR::kSnapshotVersion);
        w(static_cast<uint32_t>(8));            // regLen = 8
        for (int i = 0; i < 8; ++i) w(static_cast<uint8_t>(i));   // 8 bytes of junk
        CHECK_FALSE(rt.RestoreRegistry(b));     // Registry::Load rejects the blob
    }
}

// ---------------------------------------------------------------------------
// Resource section (ReadResourceSection)
// ---------------------------------------------------------------------------

TEST_CASE("ReadResourceSection handles corrupt sections and unknown types", "[serialization][negative]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    const SR::ResourceSerializerRegistry& set = SR::SerializableResources();

    SECTION("count claims an entry that isn't there -> CorruptedData")
    {
        std::vector<std::byte> b;
        Astra::BinaryWriter w(b);
        w(static_cast<uint32_t>(1));   // count = 1, but no entry bytes follow
        auto r = SR::ReadResourceSection(reg, b, set);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::CorruptedData);
    }

    SECTION("bodyLen exceeds remaining bytes -> CorruptedData")
    {
        std::vector<std::byte> b;
        Astra::BinaryWriter w(b);
        w(static_cast<uint32_t>(1));
        w(static_cast<uint64_t>(12345));   // some hash
        w(static_cast<uint32_t>(9999));    // bodyLen far beyond the buffer
        auto r = SR::ReadResourceSection(reg, b, set);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::CorruptedData);
    }

    SECTION("unknown resource type is skipped (forward-compat), not an error")
    {
        std::vector<std::byte> b;
        Astra::BinaryWriter w(b);
        w(static_cast<uint32_t>(1));
        w(static_cast<uint64_t>(0xDEADBEEFULL));   // hash no codec claims
        w(static_cast<uint32_t>(4));               // bodyLen = 4
        w(static_cast<uint32_t>(42));              // 4 bytes of opaque body
        auto r = SR::ReadResourceSection(reg, b, set);
        CHECK(r.IsOk());
    }
}

// ---------------------------------------------------------------------------
// Scene JSON (LoadJson / LoadBinary)
// ---------------------------------------------------------------------------

namespace
{
    std::unique_ptr<Astra::Registry> FreshSceneReg(std::shared_ptr<Astra::ComponentRegistry>& keepAlive)
    {
        keepAlive = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(keepAlive);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }
}

TEST_CASE("Scene LoadJson rejects malformed/mis-versioned documents without throwing", "[serialization][negative][scene]")
{
    std::shared_ptr<Astra::ComponentRegistry> keep;

    SECTION("missing version key")
    {
        auto reg = FreshSceneReg(keep);
        const nlohmann::json doc = nlohmann::json::parse(R"({"entities": []})");
        bool res = true;
        CHECK_NOTHROW(res = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(res);
    }

    SECTION("version has the wrong JSON type")
    {
        auto reg = FreshSceneReg(keep);
        const nlohmann::json doc = nlohmann::json::parse(R"({"version": "two", "entities": []})");
        bool res = true;
        CHECK_NOTHROW(res = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(res);
    }

    SECTION("missing entities key")
    {
        auto reg = FreshSceneReg(keep);
        const nlohmann::json doc = nlohmann::json::parse(R"({"version": 2})");
        bool res = true;
        CHECK_NOTHROW(res = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(res);
    }

    SECTION("unknown component type is skipped; the scene still loads")
    {
        auto reg = FreshSceneReg(keep);
        nlohmann::json entry;
        entry["components"]["Totally::Unknown::Type"] = nlohmann::json::object();
        entry["parent"] = -1;
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ entry });

        bool res = false;
        CHECK_NOTHROW(res = Arcane::Scene::LoadJson(*reg, doc));
        CHECK(res);   // structurally valid -> loads; the unknown component is dropped

        int locals = 0;
        reg->CreateView<Arcane::LocalTransform>().ForEach(
            [&](Astra::Entity, Arcane::LocalTransform&) { ++locals; });
        CHECK(locals == 0);
    }
}

TEST_CASE("Scene LoadBinary rejects a corrupt file with an error result", "[serialization][negative][scene]")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "arcane_corrupt_scene.bin";
    {
        std::ofstream f(path, std::ios::binary);
        const char junk[] = "this is definitely not a valid Astra registry blob";
        f.write(junk, sizeof(junk));
    }

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);

    auto loaded = Arcane::Scene::LoadBinary(path, components);
    CHECK(loaded.IsErr());   // clean error, no throw

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// Reflection reader (wrong-typed leaf data is tolerated, never thrown)
// ---------------------------------------------------------------------------

TEST_CASE("ReflectionJson reader tolerates wrong-typed leaf data without throwing", "[serialization][negative][reflection]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::LocalTransform>();
    REQUIRE(meta != nullptr);

    nlohmann::json j;
    j["position"] = "not-an-array";    // vec2 field fed a string
    j["rotation"] = "not-a-number";    // float field fed a string

    Arcane::LocalTransform lt;
    lt.position = glm::vec2(9.0f, 9.0f);
    lt.rotation = 5.0f;

    Arcane::ReflectionJsonReader reader(j);
    CHECK_NOTHROW([&]
    {
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.IsSerializable())
                reader.Visit(f, &lt);
    }());

    // Wrong DATA for a supported field TYPE is not an "unsupported type" error;
    // the guarded reader leaves the field at its prior value.
    CHECK_FALSE(reader.HasError());
    CHECK(lt.position.x == Approx(9.0f));
    CHECK(lt.rotation == Approx(5.0f));
}
