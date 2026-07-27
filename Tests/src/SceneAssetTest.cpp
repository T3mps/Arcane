// SceneAsset: the .arcscene FILE layer over Scene::SaveJson/LoadJson -- id +
// version validation, and the read-then-apply split that lets a caller validate
// a file before destroying the scene it already has.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

namespace
{
    // A registry with the scene roster registered and a two-entity scene:
    // root "Root" at (100, 0) with child "Child" at (5, 7).
    struct Fixture
    {
        std::shared_ptr<Astra::ComponentRegistry> components =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Astra::Entity   root{};
        Astra::Entity   child{};

        Fixture()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            Arcane::Transform rt; rt.position = glm::vec2(100.0f, 0.0f);
            reg.AddComponent<Arcane::Transform>(root, rt);
            reg.AddComponent<Arcane::EntityInfo>(root, Arcane::EntityInfo{Arcane::Guid::Generate(), "Root"});

            child = reg.CreateEntity();
            Arcane::Transform ct; ct.position = glm::vec2(5.0f, 7.0f);
            reg.AddComponent<Arcane::Transform>(child, ct);
            reg.AddComponent<Arcane::EntityInfo>(child, Arcane::EntityInfo{Arcane::Guid::Generate(), "Child"});
            reg.SetParent(child, root);

            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        }
    };

    std::filesystem::path TempDir(const char* leaf)
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / leaf;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
}

TEST_CASE("a scene round-trips through a file, preserving its id", "[scene][json]")
{
    const std::filesystem::path dir  = TempDir("arcane_scene_asset_roundtrip");
    const std::filesystem::path file = dir / ("level" + std::string(Arcane::Scene::kSceneExt));
    const Arcane::Guid id = Arcane::Guid::Generate();

    {
        Fixture f;
        std::string err;
        REQUIRE(Arcane::Scene::SaveSceneFile(file, f.reg, id, &err));
        CHECK(err.empty());
    }
    REQUIRE(std::filesystem::exists(file));

    std::string err;
    const auto read = Arcane::Scene::ReadSceneFile(file, &err);
    REQUIRE(read.has_value());
    CHECK(err.empty());
    CHECK(read->id == id);

    // Apply into a FRESH registry -- the load path a scene open takes.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry fresh{components};
    Arcane::RegisterSceneComponents(fresh);
    REQUIRE(Arcane::Scene::ApplySceneDocument(*read, fresh));

    const Arcane::SceneRoot* sr = fresh.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    const Arcane::Transform* rootT = fresh.GetComponent<Arcane::Transform>(sr->entity);
    REQUIRE(rootT != nullptr);
    CHECK(rootT->position.x == 100.0f);

    const auto kids = fresh.GetChildren(sr->entity);
    REQUIRE(kids.size() == 1);
    const Arcane::EntityInfo* kidInfo = fresh.GetComponent<Arcane::EntityInfo>(kids[0]);
    REQUIRE(kidInfo != nullptr);
    CHECK(kidInfo->name == "Child");
}

TEST_CASE("a file the reader rejects leaves the target registry untouched", "[scene][json]")
{
    // THE ordering guarantee: Open Scene must not empty the editor on a bad file.
    // Earning that name means there has to be an actual registry with actual
    // content in the test, and an assertion that it still holds that content
    // after ReadSceneFile rejects -- ReadSceneFile alone (no registry in sight)
    // cannot demonstrate "untouched" no matter how many ways it is called.
    const std::filesystem::path dir = TempDir("arcane_scene_asset_reject");

    Fixture f;

    // Snapshot everything CreateEmpty/Fixture established, so each SECTION can
    // assert the registry still matches it byte-for-byte after a rejection.
    const auto assertFixtureUnchanged = [&f]()
    {
        const Arcane::SceneRoot* sr = f.reg.GetResource<Arcane::SceneRoot>();
        REQUIRE(sr != nullptr);
        CHECK(sr->entity == f.root);

        const Arcane::Transform* rootT = f.reg.GetComponent<Arcane::Transform>(f.root);
        REQUIRE(rootT != nullptr);
        CHECK(rootT->position.x == 100.0f);
        CHECK(rootT->position.y == 0.0f);

        const auto kids = f.reg.GetChildren(f.root);
        REQUIRE(kids.size() == 1);
        CHECK(kids[0] == f.child);

        const Arcane::EntityInfo* childInfo = f.reg.GetComponent<Arcane::EntityInfo>(f.child);
        REQUIRE(childInfo != nullptr);
        CHECK(childInfo->name == "Child");
        const Arcane::Transform* childT = f.reg.GetComponent<Arcane::Transform>(f.child);
        REQUIRE(childT != nullptr);
        CHECK(childT->position.x == 5.0f);
        CHECK(childT->position.y == 7.0f);
    };

    SECTION("missing file")
    {
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(dir / "nope.arcscene", &err).has_value());
        CHECK_FALSE(err.empty());
        assertFixtureUnchanged();
    }
    SECTION("not JSON")
    {
        const std::filesystem::path file = dir / "bad.arcscene";
        std::ofstream(file) << "this is not json";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK_FALSE(err.empty());
        assertFixtureUnchanged();
    }
    SECTION("wrong schema version")
    {
        const std::filesystem::path file = dir / "old.arcscene";
        std::ofstream(file) << R"({"id":"00000000-0000-0000-0000-000000000001","version":1,"entities":[]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK(err.find("version") != std::string::npos);
        assertFixtureUnchanged();
    }
    SECTION("malformed id")
    {
        const std::filesystem::path file = dir / "badid.arcscene";
        std::ofstream(file) << R"({"id":"not-a-guid","version":2,"entities":[]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK_FALSE(err.empty());
        assertFixtureUnchanged();
    }
    SECTION("non-object entity")
    {
        // The reviewer's repro: passes the outer envelope (version, id,
        // entities-is-an-array), then LoadJson would create one entity for
        // the object element before rejecting the integer one at index 1.
        const std::filesystem::path file = dir / "badentity.arcscene";
        std::ofstream(file) << R"({"id":"00000000-0000-0000-0000-000000000001","version":2,)"
                               R"("entities":[{"components":{}}, 42]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK(err.find("entity 1") != std::string::npos);
        assertFixtureUnchanged();
    }
    SECTION("non-object components")
    {
        const std::filesystem::path file = dir / "badcomponents.arcscene";
        std::ofstream(file) << R"({"id":"00000000-0000-0000-0000-000000000001","version":2,)"
                               R"("entities":[{"components":"not-an-object"}]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK(err.find("entity 0") != std::string::npos);
        assertFixtureUnchanged();
    }
    SECTION("non-integer parent")
    {
        const std::filesystem::path file = dir / "badparent.arcscene";
        std::ofstream(file) << R"({"id":"00000000-0000-0000-0000-000000000001","version":2,)"
                               R"("entities":[{"components":{},"parent":"root"}]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK(err.find("entity 0") != std::string::npos);
        assertFixtureUnchanged();
    }
    SECTION("non-array links")
    {
        const std::filesystem::path file = dir / "badlinksarray.arcscene";
        std::ofstream(file) << R"({"id":"00000000-0000-0000-0000-000000000001","version":2,)"
                               R"("entities":[{"components":{},"links":0}]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK(err.find("entity 0") != std::string::npos);
        assertFixtureUnchanged();
    }
    SECTION("non-integer links entry")
    {
        const std::filesystem::path file = dir / "badlinksentry.arcscene";
        std::ofstream(file) << R"({"id":"00000000-0000-0000-0000-000000000001","version":2,)"
                               R"("entities":[{"components":{},"links":[0,"1"]}]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK(err.find("entity 0") != std::string::npos);
        assertFixtureUnchanged();
    }
}

TEST_CASE("CreateEmpty yields a saveable one-entity scene", "[scene][json]")
{
    // SaveJson walks the SceneRoot subtree and returns an EMPTY document when the
    // resource is absent, so New Scene has to establish a root or the first save
    // silently writes nothing.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity root = Arcane::Scene::CreateEmpty(reg);
    CHECK(root.IsValid());

    const Arcane::SceneRoot* sr = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    CHECK(sr->entity == root);
    CHECK(reg.GetComponent<Arcane::Transform>(root) != nullptr);
    const Arcane::EntityInfo* info = reg.GetComponent<Arcane::EntityInfo>(root);
    REQUIRE(info != nullptr);
    CHECK(info->name == "Scene");
    CHECK(info->id.IsValid());

    const nlohmann::json doc = Arcane::Scene::SaveJson(reg);
    REQUIRE(doc.contains("entities"));
    CHECK(doc["entities"].size() == 1);
}

TEST_CASE("SaveSceneFile reports an unwritable path instead of throwing", "[scene][json]")
{
    Fixture f;
    std::string err;
    // A directory that does not exist -- SaveSceneFile does not create parents.
    const std::filesystem::path bad =
        std::filesystem::temp_directory_path() / "arcane_no_such_dir_xyz" / "s.arcscene";
    CHECK_FALSE(Arcane::Scene::SaveSceneFile(bad, f.reg, Arcane::Guid::Generate(), &err));
    CHECK_FALSE(err.empty());
}
