// Input-seam spec 2026-10-02 s5.5 / s7: a scene carrying a field its type no
// longer reflects still loads, keeps every other value, warns ONCE per type and
// field, and a re-save drops the stale key (Review Focus #4: a scene saved by
// the old build re-opened and re-saved by the new one).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <string>
#include <vector>

TEST_CASE("NoteUnknownField is true once per type and key", "[scene][serialization]")
{
    CHECK(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeA", "legacyKey"));
    CHECK_FALSE(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeA", "legacyKey"));
    CHECK(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeA", "otherKey"));
    CHECK(Arcane::Scene::Detail::NoteUnknownField("Probe::TypeB", "legacyKey"));
}

TEST_CASE("ReflectionJsonReader reports the keys it never consumed", "[scene][serialization]")
{
    Arcane::RigidBody2D body;
    const nlohmann::json fields = { { "mass", 2.5 }, { "legacyKey", 1 } };
    Arcane::ReflectionJsonReader reader(fields);
    // The same walk ComponentRegistry's VisitFields<T> does (the descriptor's
    // visitFields slot): every serializable reflected field, in order.
    const Astra::TypeMeta* meta = Astra::GetMeta(Astra::TypeID<Arcane::RigidBody2D>::Hash());
    REQUIRE(meta != nullptr);
    for (const Astra::FieldInfo& field : meta->fields)
        if (field.IsSerializable())
            reader.Visit(field, &body);
    CHECK_FALSE(reader.HasError());
    CHECK(body.mass == 2.5f);
    REQUIRE(reader.UnconsumedKeys() == std::vector<std::string>{ "legacyKey" });
}

TEST_CASE("an old-build scene with a removed field loads, keeps its values, and re-saves without the key",
          "[scene][serialization]")
{
    Astra::Registry authored;
    Arcane::RegisterSceneComponents(authored);
    Arcane::RegisterPhysicsComponents(authored);
    const Astra::Entity e = authored.CreateEntity();
    Arcane::RigidBody2D rb;
    rb.mass = 3.25f;
    authored.AddComponent<Arcane::RigidBody2D>(e, rb);
    authored.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ e });   // SaveJson walks from the root
    nlohmann::json doc = Arcane::Scene::SaveJson(authored);

    // Simulate the OLD build: a key this build no longer reflects.
    bool injected = false;
    for (auto& entity : doc["entities"])
        if (entity["components"].contains("Arcane::RigidBody2D"))
        {
            entity["components"]["Arcane::RigidBody2D"]["staleFromOldBuild"] = 0.0;
            injected = true;
        }
    REQUIRE(injected);

    Astra::Registry loaded;
    Arcane::RegisterSceneComponents(loaded);
    Arcane::RegisterPhysicsComponents(loaded);
    REQUIRE(Arcane::Scene::LoadJson(loaded, doc));
    float mass = 0.0f;
    loaded.CreateView<Arcane::RigidBody2D>().ForEach([&](Astra::Entity, Arcane::RigidBody2D& b) { mass = b.mass; });
    CHECK(mass == 3.25f);

    const nlohmann::json resaved = Arcane::Scene::SaveJson(loaded);
    bool found = false;
    for (const auto& entity : resaved["entities"])
        if (entity["components"].contains("Arcane::RigidBody2D"))
        {
            found = true;
            CHECK_FALSE(entity["components"]["Arcane::RigidBody2D"].contains("staleFromOldBuild"));
        }
    CHECK(found);
}
