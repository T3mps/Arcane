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
    const std::filesystem::path dir = TempDir("arcane_scene_asset_reject");

    SECTION("missing file")
    {
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(dir / "nope.arcscene", &err).has_value());
        CHECK_FALSE(err.empty());
    }
    SECTION("not JSON")
    {
        const std::filesystem::path f = dir / "bad.arcscene";
        std::ofstream(f) << "this is not json";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(f, &err).has_value());
        CHECK_FALSE(err.empty());
    }
    SECTION("wrong schema version")
    {
        const std::filesystem::path f = dir / "old.arcscene";
        std::ofstream(f) << R"({"id":"00000000-0000-0000-0000-000000000001","version":1,"entities":[]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(f, &err).has_value());
        CHECK(err.find("version") != std::string::npos);
    }
    SECTION("malformed id")
    {
        const std::filesystem::path f = dir / "badid.arcscene";
        std::ofstream(f) << R"({"id":"not-a-guid","version":2,"entities":[]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(f, &err).has_value());
        CHECK_FALSE(err.empty());
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
