// E02-5: serialization negative-input suite. Every corrupt / truncated /
// wrong-typed / missing-key / version-mismatch input must return a clean error
// result -- no throw, no crash -- across all three serialization surfaces:
// the snapshot frame, the resource section, scene JSON, and the reflection
// reader. Happy-path round-trips live next to their subjects; this file is the
// adversarial-input coverage.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Base/Log.hpp>
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
#include <glm/gtc/quaternion.hpp>
#include <Json.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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

    // Review finding (Fix 3): LoadSceneRoot used to install whatever raw entity
    // value the (well-framed) body carried, without checking it resolves in the
    // target registry. A well-framed section carrying a bogus/dangling entity
    // id must be rejected as corrupt data instead of silently installing an
    // invalid SceneRoot.
    SECTION("SceneRoot codec rejects a bogus entity that does not resolve -> CorruptedData")
    {
        std::vector<std::byte> b;
        Astra::BinaryWriter w(b);
        w(static_cast<uint32_t>(1));                                    // count = 1
        w(Astra::TypeID<Arcane::SceneRoot>::Hash());                    // SceneRoot's real typeHash
        w(static_cast<uint32_t>(sizeof(uint64_t)));                     // bodyLen = 8
        w(static_cast<uint64_t>(0xDEADBEEFULL));                        // bogus entity raw value;
                                                                         // reg has zero entities, so it
                                                                         // cannot resolve to anything valid
        auto r = SR::ReadResourceSection(reg, b, set);
        REQUIRE(r.IsErr());
        CHECK(*r.GetError() == Astra::SerializationError::CorruptedData);
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

    // Log-capture idiom (mirrors MosaicDiagnosticsTest.cpp's AttachCapture):
    // attach a callback sink to the engine logger so the "scene load skipped a
    // component" WARN can be asserted on directly instead of desk-verified only.
    std::shared_ptr<spdlog::sinks::callback_sink_mt> AttachLogCapture(std::string& out)
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&out](const spdlog::details::log_msg& m) { out.assign(m.payload.data(), m.payload.size()); });
        Arcane::Log::Engine()->sinks().push_back(cb);
        return cb;
    }
    void DetachLogCapture(const std::shared_ptr<spdlog::sinks::callback_sink_mt>& cb)
    {
        auto& sinks = Arcane::Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), cb), sinks.end());
    }

    // Reflected, but deliberately never registered as a component on any
    // registry in this file -- exercises AddComponentResult::
    // SkippedUnregistered (the "reflected but not registered -- plugin not
    // loaded?" branch), as opposed to a wholly-unknown type name.
    struct ReflectedNotRegistered { int value = 0; };
}
namespace
{
    ASTRA_REFLECT_TYPE(ReflectedNotRegistered)
    ASTRA_REFLECT_FIELD(ReflectedNotRegistered, value)
    ASTRA_END_REFLECT_TYPE()
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
        // Version built from the constant: a hardcoded number that a later
        // bump strands would make this section refuse for the WRONG reason
        // and stop testing the missing-entities gate at all.
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        bool res = true;
        CHECK_NOTHROW(res = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(res);
    }

    // Regression coverage for the entity-rename incident (2026-07-27,
    // EntityInfo->Identity): a scene JSON carrying an orphaned/bogus component
    // key used to be dropped with NO trace anywhere -- LoadJson tolerated it
    // (by design, forward-compat) but nothing logged it, so the loss was
    // invisible until the scene was re-saved and the data was gone for good.
    // These two sections prove both halves of the fix: the scene still loads
    // (a valid sibling component on the SAME entity survives untouched), and
    // the skip is now surfaced through the engine logger.
    SECTION("unknown component type is skipped; the scene still loads; the skip is logged")
    {
        auto reg = FreshSceneReg(keep);
        const std::string transformName(Astra::GetMeta<Arcane::Transform>()->typeName);

        nlohmann::json entry;
        entry["components"]["Totally::Unknown::Type"] = nlohmann::json::object();
        entry["components"][transformName] = nlohmann::json::object();   // valid sibling component
        entry["parent"] = -1;
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ entry });

        std::string captured;
        auto cb = AttachLogCapture(captured);
        bool res = false;
        CHECK_NOTHROW(res = Arcane::Scene::LoadJson(*reg, doc));
        DetachLogCapture(cb);

        CHECK(res);   // structurally valid -> loads; the unknown component is dropped

        // The valid sibling component on the same entity is intact -- the skip
        // does not take the rest of the entity down with it.
        int locals = 0;
        reg->CreateView<Arcane::Transform>().ForEach(
            [&](Astra::Entity, Arcane::Transform&) { ++locals; });
        CHECK(locals == 1);

        // The skip was surfaced through the engine logger, naming the unknown
        // type and warning about the consequence of re-saving.
        CHECK(captured.find("Totally::Unknown::Type") != std::string::npos);
        CHECK(captured.find("unknown component") != std::string::npos);
        CHECK(captured.find("permanently") != std::string::npos);
    }

    SECTION("reflected-but-unregistered component is skipped, distinctly from an unknown type, and logged")
    {
        // Same shape as above, but the key names a type that IS reflected
        // (has a TypeMeta) yet was never registered as a component on this
        // registry -- the "plugin not loaded" case, which AddComponentByTypeName
        // must distinguish from a wholly-unknown type name.
        auto reg = FreshSceneReg(keep);
        const std::string unregName(Astra::GetMeta<ReflectedNotRegistered>()->typeName);

        nlohmann::json entry;
        entry["components"][unregName] = nlohmann::json::object();
        entry["parent"] = -1;
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ entry });

        std::string captured;
        auto cb = AttachLogCapture(captured);
        bool res = false;
        CHECK_NOTHROW(res = Arcane::Scene::LoadJson(*reg, doc));
        DetachLogCapture(cb);

        CHECK(res);   // structurally valid -> loads; the unregistered component is dropped

        CHECK(captured.find(unregName) != std::string::npos);
        CHECK(captured.find("not registered") != std::string::npos);
        CHECK(captured.find("permanently") != std::string::npos);
        // Distinct wording from the unknown-type case above -- proves the two
        // causes are not collapsed into one indistinguishable message.
        CHECK(captured.find("unknown component") == std::string::npos);
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

// Task 3 (F1) split this contract in two. The NOTHROW half is unchanged and is
// the load-bearing one -- nlohmann's get<T> throws type_error on a hand-edited
// file and this engine is exception-free. What changed is the verdict: a key
// that is PRESENT but unreadable now LATCHES instead of reading as absent.
// The case that motivated it is in the report -- a `"position": [x, y]` left
// over from the 2D Transform silently zeroed every entity in a scene.
TEST_CASE("ReflectionJson reader latches wrong-typed leaf data without throwing", "[serialization][negative][reflection]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::Transform>();
    REQUIRE(meta != nullptr);

    nlohmann::json j;
    j["position"] = "not-an-array";    // vec3 field fed a string
    j["rotation"] = "not-a-number";    // quaternion field fed a string

    Arcane::Transform lt;
    lt.position = glm::vec3(9.0f, 9.0f, 0.0f);
    lt.rotation = Arcane::RotationAboutZ(1.0f);

    Arcane::ReflectionJsonReader reader(j);
    CHECK_NOTHROW([&]
    {
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.IsSerializable())
                reader.Visit(f, &lt);
    }());

    // Present-but-unreadable is data loss with the witness in the file: latch,
    // and name the field so the diagnostic points at it.
    CHECK(reader.HasError());
    CHECK(reader.Error().find("position") != std::string::npos);
    // The field itself is still left at its prior value -- the reader never
    // half-writes what it refused to read.
    CHECK(lt.position.x == Approx(9.0f));
    CHECK(Arcane::RotationZ(lt.rotation) == Approx(1.0f).margin(1e-5));
}

// The other half of the same contract, and the one that must NOT change: a
// MISSING key is forward/back compatibility, not data loss.
TEST_CASE("ReflectionJson reader leaves a MISSING key at its default without latching", "[serialization][negative][reflection]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::Transform>();
    REQUIRE(meta != nullptr);

    nlohmann::json j;
    j["position"] = { 1.0, 2.0, 3.0 };   // present and well-formed; rotation/scale absent

    Arcane::Transform lt;
    lt.scale = glm::vec3(7.0f);

    Arcane::ReflectionJsonReader reader(j);
    for (const Astra::FieldInfo& f : meta->fields)
        if (f.IsSerializable())
            reader.Visit(f, &lt);

    CHECK_FALSE(reader.HasError());
    CHECK(lt.position.z == Approx(3.0f));
    CHECK(lt.scale.x == Approx(7.0f));   // absent -> untouched
}

// The quaternion NORM guard (Task 3, F1). glm::mat4_cast assumes a unit
// quaternion and does not normalize, so a stored norm of 2 bakes a uniform 4x
// scale into Transform::ToMatrix's basis: the entity renders at four times its
// authored size with nothing anywhere saying so. The retired float rotation
// could not express an invalid value at all -- this hazard arrived WITH the
// widening, on the hand-edited-file path.
TEST_CASE("ReflectionJson reader guards a non-unit quaternion", "[serialization][negative][reflection]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::Transform>();
    REQUIRE(meta != nullptr);

    const auto readRotation = [&meta](const nlohmann::json& node, Arcane::Transform& out)
    {
        nlohmann::json j;
        j["rotation"] = node;
        Arcane::ReflectionJsonReader reader(j);
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.IsSerializable())
                reader.Visit(f, &out);
        return reader.HasError();
    };

    SECTION("a grossly non-unit quaternion latches instead of baking a scale")
    {
        Arcane::Transform lt;
        // [x, y, z, w] = a norm-2 identity rotation. mat4_cast would produce a
        // basis with every column 4x too long.
        CHECK(readRotation(nlohmann::json{ 0.0, 0.0, 0.0, 2.0 }, lt));
        // Refused, not half-applied: the basis is still unit-length.
        const glm::mat4 m = lt.ToMatrix();
        CHECK(glm::length(glm::vec3(m[0])) == Approx(1.0f));
    }

    SECTION("a zero-length quaternion latches rather than producing NaN")
    {
        Arcane::Transform lt;
        CHECK(readRotation(nlohmann::json{ 0.0, 0.0, 0.0, 0.0 }, lt));
        const glm::mat4 m = lt.ToMatrix();
        CHECK(std::isfinite(m[0][0]));   // never normalized by zero
        CHECK(glm::length(glm::vec3(m[0])) == Approx(1.0f));
    }

    SECTION("a near-unit quaternion is normalized silently, not refused")
    {
        // Two-decimal hand authoring of a 90-degree turn about +Z: norm is off
        // by ~0.4%, which is ordinary and must load. Being generous is safe
        // BECAUSE the reader normalizes what it accepts.
        Arcane::Transform lt;
        CHECK_FALSE(readRotation(nlohmann::json{ 0.0, 0.0, 0.71, 0.71 }, lt));
        CHECK(Arcane::RotationZ(lt.rotation) == Approx(1.5707963f).margin(1e-3));
        const glm::mat4 m = lt.ToMatrix();
        CHECK(glm::length(glm::vec3(m[0])) == Approx(1.0f).margin(1e-5));   // no scale baked in
    }
}
