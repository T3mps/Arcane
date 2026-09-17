// Scene JSON v5 -> v6: the +Y flip (F4 plan 1 Task 2, spec
// 2026-09-17-f4-editor-3d-authoring s2). Two claims:
//   1. MigrateV5ToYUp is a PURE document rewrite -- every Y that carries a
//      world direction is negated, and a rotation quaternion is conjugated
//      across the XZ plane -- and it stamps the document v6.
//   2. LoadJson applies it exactly when the file says v5-or-older, leaves a v6
//      file alone, and SaveJson writes v6.
// Plus a one-shot re-save TOOL (env-gated, see the last case) used once to
// bring the ReferenceProject scenes forward.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

TEST_CASE("MigrateV5ToYUp negates Y, conjugates the rotation across XZ, and flips physics Y", "[scene][migration]")
{
    nlohmann::json doc = nlohmann::json::parse(R"({
      "version": 5, "assets": [], "entities": [
        { "components": {
            "Arcane::Transform": { "position": [1.0, 2.0, 3.0], "rotation": [0.1, 0.2, 0.3, 0.9], "scale": [1,1,1] },
            "Arcane::Collider2D": { "fixtures": [ { "localPos": [0.5, -0.25], "localAngle": 0.4 } ] },
            "Arcane::RigidBody2D": { "velocity": [3.0, 4.0] },
            "Arcane::PhysicsSettings": { "gravity": [0.0, 9.81] }
        } } ] })");
    Arcane::Scene::Detail::MigrateV5ToYUp(doc);
    const auto& c = doc["entities"][0]["components"];
    CHECK(c["Arcane::Transform"]["position"][0].get<float>() == Approx( 1.0f));   // X untouched
    CHECK(c["Arcane::Transform"]["position"][1].get<float>() == Approx(-2.0f));
    CHECK(c["Arcane::Transform"]["position"][2].get<float>() == Approx( 3.0f));   // Z untouched
    CHECK(c["Arcane::Transform"]["rotation"][0].get<float>() == Approx(-0.1f));
    CHECK(c["Arcane::Transform"]["rotation"][1].get<float>() == Approx( 0.2f));
    CHECK(c["Arcane::Transform"]["rotation"][2].get<float>() == Approx(-0.3f));
    CHECK(c["Arcane::Transform"]["rotation"][3].get<float>() == Approx( 0.9f));
    CHECK(c["Arcane::Transform"]["scale"][1].get<float>() == Approx( 1.0f));      // scale is not a direction
    CHECK(c["Arcane::Collider2D"]["fixtures"][0]["localPos"][1].get<float>() == Approx(0.25f));
    CHECK(c["Arcane::Collider2D"]["fixtures"][0]["localAngle"].get<float>() == Approx(-0.4f));
    CHECK(c["Arcane::RigidBody2D"]["velocity"][1].get<float>() == Approx(-4.0f));
    CHECK(c["Arcane::PhysicsSettings"]["gravity"][1].get<float>() == Approx(-9.81f));
    CHECK(doc["version"].get<int>() == 6);
}

TEST_CASE("MigrateV5ToYUp tolerates a document with nothing to migrate", "[scene][migration]")
{
    // Forward/back compatibility, same spirit as the reflection bridge: a
    // missing key, a missing component, a non-object entry -- none of it may
    // throw or leave the document half-rewritten.
    nlohmann::json empty = nlohmann::json::parse(R"({ "version": 5, "entities": [] })");
    CHECK_NOTHROW(Arcane::Scene::Detail::MigrateV5ToYUp(empty));
    CHECK(empty["version"].get<int>() == 6);

    nlohmann::json odd = nlohmann::json::parse(R"({
      "version": 4, "entities": [ 7, { }, { "components": 3 },
        { "components": { "Arcane::Transform": { } } },
        { "components": { "Arcane::Collider2D": { "fixtures": [ 5 ] } } } ] })");
    CHECK_NOTHROW(Arcane::Scene::Detail::MigrateV5ToYUp(odd));
    CHECK(odd["version"].get<int>() == 6);

    nlohmann::json noEntities = nlohmann::json::parse(R"({ "version": 5 })");
    CHECK_NOTHROW(Arcane::Scene::Detail::MigrateV5ToYUp(noEntities));
}

TEST_CASE("LoadJson migrates a v5 scene on load and leaves a v6 one untouched; SaveJson writes v6", "[scene][migration]")
{
    // Built through the real path: a registry with a Transform at y=+2 saved by
    // THIS build is v6 and must round-trip unchanged; the same JSON hand-stamped
    // v5 must come back at y=-2.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    Arcane::Scene::CreateEmpty(reg);   // SceneRoot + Main Camera
    const Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f, 2.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.SetParent(e, reg.GetResource<Arcane::SceneRoot>()->entity);

    nlohmann::json v6 = Arcane::Scene::SaveJson(reg);
    REQUIRE(v6["version"].get<int>() == 6);

    auto readY = [](const nlohmann::json& doc)
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry back(creg);
        Arcane::RegisterSceneComponents(back);
        REQUIRE(Arcane::Scene::LoadJson(back, doc));
        float y = 0.0f;
        back.CreateView<Arcane::Transform>().ForEach(
            [&](Astra::Entity, Arcane::Transform& t) { if (t.position.y != 0.0f) y = t.position.y; });
        return y;
    };

    CHECK(readY(v6) == Approx(2.0f));

    nlohmann::json v5 = v6;
    v5["version"] = 5;
    CHECK(readY(v5) == Approx(-2.0f));

    // The migration is applied to a COPY: the caller's document must be
    // untouched, because LoadJson takes it by const reference and callers
    // (SceneAsset's SceneDocument) keep reading it afterwards.
    CHECK(v5["version"].get<int>() == 5);
    CHECK(v5 != v6);
}

namespace
{
    // One entity's component ROSTER as a single comparable string: the keys
    // under "components", sorted, joined. This is the thing a re-save can
    // silently destroy -- LoadJson SKIPS a component type this process has not
    // registered (it warns and carries on, by design: forward compatibility),
    // and SaveJson then writes only the live roster, so the skipped component
    // is gone from the file permanently.
    std::vector<std::string> EntityRosters(const nlohmann::json& doc)
    {
        std::vector<std::string> rosters;
        if (!doc.contains("entities") || !doc["entities"].is_array()) return rosters;
        for (const auto& entry : doc["entities"])
        {
            std::vector<std::string> keys;
            if (entry.is_object())
            {
                const auto cit = entry.find("components");
                if (cit != entry.end() && cit->is_object())
                    for (auto it = cit->begin(); it != cit->end(); ++it)
                        keys.push_back(it.key());
            }
            std::sort(keys.begin(), keys.end());
            std::string joined;
            for (const std::string& k : keys) { if (!joined.empty()) joined += ", "; joined += k; }
            rosters.push_back(joined.empty() ? "<none>" : joined);
        }
        return rosters;
    }
}

// A TOOL, not a test of the engine: re-saves the scenes in a directory at the
// current schema (load applies the migration, save stamps v6). Runs only when
// ARCANE_RESAVE_SCENES names the directory, so a normal suite run skips it.
//
// It GUARDS ITSELF against the one way this can quietly cost data. A registry
// built here knows the engine's components and nothing else, so a scene
// carrying a GAME-MODULE or PLUGIN component would load with that component
// skipped (an ARC_WARN nobody reads in a batch run) and be re-saved WITHOUT
// it -- and a tool that only checks read/apply/save succeeded would report
// success while doing it. So the roster of every entity is captured from the
// document BEFORE the apply and compared against the candidate document
// BEFORE the file is written: same entity count, same per-entity component
// keys, or the case fails naming the file and the roster, with the file left
// untouched. Rosters are compared as a SORTED MULTISET because re-saving
// legitimately re-orders entities (SaveJson walks the SceneRoot subtree
// root-first); losing one never is.
TEST_CASE("resave scenes at the current schema (tool)", "[migration][tool]")
{
    const char* dir = std::getenv("ARCANE_RESAVE_SCENES");
    if (!dir) { SUCCEED("ARCANE_RESAVE_SCENES unset -- tool skipped"); return; }

    for (const auto& f : std::filesystem::directory_iterator(dir))
    {
        if (f.path().extension() != Arcane::Scene::kSceneExt) continue;
        const std::string name = f.path().generic_string();
        INFO("scene: " << name);

        std::string err;
        const auto read = Arcane::Scene::ReadSceneFile(f.path(), &err);
        INFO("read: " << err);
        REQUIRE(read.has_value());

        std::vector<std::string> before = EntityRosters(read->doc);

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents(reg);
        REQUIRE(Arcane::Scene::ApplySceneDocument(*read, reg));

        // The guard runs against the CANDIDATE document, in memory, BEFORE the
        // file is touched: a tool that destroys a component and then reports
        // the destruction has still destroyed it, and this one is pointed at
        // authored content. SaveSceneFile writes the same SaveJson output, so
        // checking it here checks exactly what would land.
        std::vector<std::string> after = EntityRosters(Arcane::Scene::SaveJson(reg));
        {
            INFO("entity count: " << before.size() << " before, " << after.size()
                 << " after -- NOT re-saved");
            REQUIRE(after.size() == before.size());
        }
        std::sort(before.begin(), before.end());
        std::sort(after.begin(), after.end());
        for (std::size_t i = 0; i < before.size(); ++i)
        {
            INFO("entity roster would change -- before: [" << before[i] << "] after: ["
                 << after[i] << "] -- NOT re-saved");
            REQUIRE(after[i] == before[i]);
        }

        REQUIRE(Arcane::Scene::SaveSceneFile(f.path(), reg, read->id, &err));
        INFO("save: " << err);

        // And what the next reader of the FILE sees is v6, read back off disk
        // rather than trusted from the registry.
        const auto reread = Arcane::Scene::ReadSceneFile(f.path(), &err);
        INFO("re-read: " << err);
        REQUIRE(reread.has_value());
        CHECK(reread->doc.value("version", 0) == Arcane::Scene::kSceneJsonVersion);
        CHECK(EntityRosters(reread->doc).size() == before.size());
    }
}
