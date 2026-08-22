// SceneAsset: the .arcscene FILE layer over Scene::SaveJson/LoadJson -- id +
// version validation, and the read-then-apply split that lets a caller validate
// a file before destroying the scene it already has.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

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
            Arcane::Transform rt; rt.position = glm::vec3(100.0f, 0.0f, 0.0f);
            reg.AddComponent<Arcane::Transform>(root, rt);
            reg.AddComponent<Arcane::Identity>(root, Arcane::Identity{Arcane::Guid::Generate(), "Root"});

            child = reg.CreateEntity();
            Arcane::Transform ct; ct.position = glm::vec3(5.0f, 7.0f, 0.0f);
            reg.AddComponent<Arcane::Transform>(child, ct);
            reg.AddComponent<Arcane::Identity>(child, Arcane::Identity{Arcane::Guid::Generate(), "Child"});
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

    std::string ReadAll(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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
    const Arcane::Identity* kidInfo = fresh.GetComponent<Arcane::Identity>(kids[0]);
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

        const Arcane::Identity* childInfo = f.reg.GetComponent<Arcane::Identity>(f.child);
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
        std::ofstream(file) << R"({"id":"not-a-guid","version":)"
                            << Arcane::Scene::kSceneJsonVersion << R"(,"entities":[]})";
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
        std::ofstream(file) << R"({"id":"00000000-0000-0000-0000-000000000001","version":)"
                            << Arcane::Scene::kSceneJsonVersion
                            << R"(,"entities":[{"components":{}}, 42]})";
        std::string err;
        CHECK_FALSE(Arcane::Scene::ReadSceneFile(file, &err).has_value());
        CHECK(err.find("entity 1") != std::string::npos);
        assertFixtureUnchanged();
    }
}

// Review finding: the gate used to reject a non-object `components` field, a
// non-integer `parent` field, and a malformed `links` field (non-array, or an
// entry that is not an integer) as if they were structural errors. They are
// not -- LoadJson (SceneSerializer.hpp:212, :246, :254, :258) tolerates every
// one of them: it skips the components block, leaves the parent unset, or
// skips the offending links entry, and still returns true. A gate stricter
// than the loader it guards would make a scene file the running game loads
// fine unopenable in the editor, so these SECTIONs assert the opposite of
// what they used to: ReadSceneFile succeeds, and ApplySceneDocument into a
// fresh registry also succeeds, reproducing LoadJson's own tolerance end to
// end through the gate.
TEST_CASE("a malformed parent, links, or non-object components field is tolerated, not rejected", "[scene][json]")
{
    const std::filesystem::path dir = TempDir("arcane_scene_asset_tolerate");
    const std::string ltName(Astra::GetMeta<Arcane::Transform>()->typeName);

    auto ApplyToFreshRegistry = [](const Arcane::Scene::SceneDocument& read, Astra::Registry& fresh)
    {
        Arcane::RegisterSceneComponents(fresh);
        return Arcane::Scene::ApplySceneDocument(read, fresh);
    };

    SECTION("non-object components")
    {
        // Neither the populated-object branch nor the all-Serializable(false)
        // null branch (SceneSerializer.hpp:226-227) matches a JSON string, so
        // LoadJson leaves the whole components block unprocessed -- the
        // entity is still created, just with none of the usual components.
        const std::filesystem::path file = dir / "goodcomponents.arcscene";
        nlohmann::json doc;
        doc["id"] = "00000000-0000-0000-0000-000000000001";
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ nlohmann::json{{"components", "not-an-object"}} });
        std::ofstream(file) << doc.dump();

        std::string err;
        const auto read = Arcane::Scene::ReadSceneFile(file, &err);
        REQUIRE(read.has_value());
        CHECK(err.empty());

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry fresh{components};
        REQUIRE(ApplyToFreshRegistry(*read, fresh));

        const Arcane::SceneRoot* sr = fresh.GetResource<Arcane::SceneRoot>();
        REQUIRE(sr != nullptr);
        CHECK(fresh.GetComponent<Arcane::Transform>(sr->entity) == nullptr);
        CHECK(fresh.GetComponent<Arcane::Identity>(sr->entity) == nullptr);
    }

    SECTION("non-integer parent")
    {
        // pit->is_number_integer() (SceneSerializer.hpp:246) fails on a
        // string, so SetParent is simply never called for this entity -- it
        // loads as a root-level entity with no parent, not as a rejected file.
        const std::filesystem::path file = dir / "goodparent.arcscene";
        nlohmann::json e0, e1;
        e0["components"][ltName]["position"] = { 100.0, 0.0, 0.0 };
        e1["components"][ltName]["position"] = { 5.0, 7.0, 0.0 };
        e1["parent"] = "root";
        nlohmann::json doc;
        doc["id"] = "00000000-0000-0000-0000-000000000001";
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ e0, e1 });
        std::ofstream(file) << doc.dump();

        std::string err;
        const auto read = Arcane::Scene::ReadSceneFile(file, &err);
        REQUIRE(read.has_value());
        CHECK(err.empty());

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry fresh{components};
        REQUIRE(ApplyToFreshRegistry(*read, fresh));

        Astra::Entity malformed{};
        fresh.CreateView<Arcane::Transform>().ForEach([&](Astra::Entity e, Arcane::Transform& t)
        {
            if (t.position.x > 4.0f && t.position.x < 6.0f) malformed = e;
        });
        REQUIRE(malformed.IsValid());
        CHECK_FALSE(fresh.HasParent(malformed));
    }

    SECTION("non-array links")
    {
        // lit->is_array() (SceneSerializer.hpp:254) fails on a number, so the
        // whole links block is skipped for this entity -- no link is formed,
        // but the entity (and the rest of the file) still loads.
        const std::filesystem::path file = dir / "goodlinksarray.arcscene";
        nlohmann::json e0, e1;
        e0["components"][ltName]["position"] = { 1.0, 1.0, 0.0 };
        e0["links"] = 0;
        e1["components"][ltName]["position"] = { 2.0, 2.0, 0.0 };
        nlohmann::json doc;
        doc["id"] = "00000000-0000-0000-0000-000000000001";
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ e0, e1 });
        std::ofstream(file) << doc.dump();

        std::string err;
        const auto read = Arcane::Scene::ReadSceneFile(file, &err);
        REQUIRE(read.has_value());
        CHECK(err.empty());

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry fresh{components};
        REQUIRE(ApplyToFreshRegistry(*read, fresh));

        Astra::Entity first{};
        fresh.CreateView<Arcane::Transform>().ForEach([&](Astra::Entity e, Arcane::Transform& t)
        {
            if (t.position.x > 0.5f && t.position.x < 1.5f) first = e;
        });
        REQUIRE(first.IsValid());
        CHECK(fresh.GetRelationshipGraph().GetLinks(first).empty());
    }

    SECTION("non-integer links entry")
    {
        // Each links entry is checked individually (SceneSerializer.hpp:258);
        // a non-integer entry is `continue`d past, but earlier/later valid
        // entries in the same array still apply -- partial tolerance within
        // one field, not an all-or-nothing rejection of the entity.
        const std::filesystem::path file = dir / "goodlinksentry.arcscene";
        nlohmann::json e0, e1;
        e0["components"][ltName]["position"] = { 1.0, 1.0, 0.0 };
        e0["links"] = { 1, "not-an-integer" };
        e1["components"][ltName]["position"] = { 2.0, 2.0, 0.0 };
        nlohmann::json doc;
        doc["id"] = "00000000-0000-0000-0000-000000000001";
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ e0, e1 });
        std::ofstream(file) << doc.dump();

        std::string err;
        const auto read = Arcane::Scene::ReadSceneFile(file, &err);
        REQUIRE(read.has_value());
        CHECK(err.empty());

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry fresh{components};
        REQUIRE(ApplyToFreshRegistry(*read, fresh));

        Astra::Entity first{}, second{};
        fresh.CreateView<Arcane::Transform>().ForEach([&](Astra::Entity e, Arcane::Transform& t)
        {
            if (t.position.x > 0.5f && t.position.x < 1.5f) first = e;
            if (t.position.x > 1.5f && t.position.x < 2.5f) second = e;
        });
        REQUIRE(first.IsValid());
        REQUIRE(second.IsValid());
        const auto& links = fresh.GetRelationshipGraph().GetLinks(first);
        CHECK(links.size() == 1);
        CHECK(std::find(links.begin(), links.end(), second) != links.end());
    }
}

TEST_CASE("CreateEmpty yields a saveable scene with a root and a camera", "[scene][json]")
{
    // SaveJson walks the SceneRoot subtree and returns an EMPTY document when the
    // resource is absent, so New Scene has to establish a root or the first save
    // silently writes nothing.
    //
    // Two entities, not one: the camera arc made CreateEmpty ship a "Main Camera"
    // child, because a scene with no Camera renders nothing in a runtime host and
    // "author a level, press Play, get a black window" is a terrible first five
    // minutes (Unity puts a Main Camera in every new scene for the same reason).
    // The count below is the assertion that keeps that default HONEST -- it is
    // what proves the camera is inside the saved subtree rather than a sibling of
    // the root, which SaveJson would silently drop.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Astra::Entity root = Arcane::Scene::CreateEmpty(reg);
    CHECK(root.IsValid());

    const Arcane::SceneRoot* sr = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    CHECK(sr->entity == root);
    CHECK(reg.GetComponent<Arcane::Transform>(root) != nullptr);
    const Arcane::Identity* info = reg.GetComponent<Arcane::Identity>(root);
    REQUIRE(info != nullptr);
    CHECK(info->name == "Scene");
    CHECK(info->id.IsValid());

    const nlohmann::json doc = Arcane::Scene::SaveJson(reg);
    REQUIRE(doc.contains("entities"));
    CHECK(doc["entities"].size() == 2);   // the root + its "Main Camera" child
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

TEST_CASE("a failed save leaves the previously-saved scene byte-for-byte intact", "[scene][json]")
{
    // The review finding this covers: SaveSceneFile used to open the destination
    // with std::ios::trunc, so the authored level was destroyed at OPEN and any
    // later failure returned a tidy `false` over an already-empty file that
    // ReadSceneFile then rejects. The temp-sibling + atomic-replace route means
    // every failure path must leave the ORIGINAL readable.
    const std::filesystem::path dir  = TempDir("arcane_scene_asset_failed_save");
    const std::filesystem::path file = dir / ("level" + std::string(Arcane::Scene::kSceneExt));
    const std::filesystem::path tmp  = file.string() + ".tmp";
    const Arcane::Guid id = Arcane::Guid::Generate();

    // The authored work a failed save must not take with it.
    {
        Fixture f;
        std::string err;
        REQUIRE(Arcane::Scene::SaveSceneFile(file, f.reg, id, &err));
    }
    const std::string original = ReadAll(file);
    REQUIRE_FALSE(original.empty());

    // Each SECTION re-saves from a FRESH Fixture, whose Identity Guids are
    // freshly generated -- so a save that wrongly succeeded would change the
    // file's bytes and the trailing comparison would catch it.
    SECTION("the serializer throws mid-write")
    {
        // This one genuinely reaches the post-truncate window rather than
        // approximating it. nlohmann accepts arbitrary bytes INTO a json string
        // and only rejects invalid UTF-8 at dump() -- so the throw lands after
        // the old code's truncating open had already zeroed the file, which is
        // exactly the ordering the fix changes.
        Fixture f;
        Arcane::Identity* info = f.reg.GetComponent<Arcane::Identity>(f.root);
        REQUIRE(info != nullptr);
        info->name = "bad\xC3\x28";   // 0xC3 opens a 2-byte sequence; 0x28 is no continuation byte

        // Proof that this really is the post-open window and not a failure that
        // happens harmlessly early: the document ASSEMBLES fine and only the
        // serialization of it throws. SaveSceneFile calls dump() with the output
        // stream already open, so the old truncating version had zeroed the
        // destination by the time this throw arrived.
        const nlohmann::json built = Arcane::Scene::SaveJson(f.reg);
        CHECK_THROWS_AS(built.dump(2), nlohmann::json::exception);

        std::string err;
        CHECK_FALSE(Arcane::Scene::SaveSceneFile(file, f.reg, id, &err));
        CHECK_FALSE(err.empty());
    }
    SECTION("the temp sibling cannot be created")
    {
        // A directory squatting on the temp path makes the ofstream open fail --
        // the failure that happens BEFORE any byte is written.
        std::error_code ec;
        std::filesystem::create_directory(tmp, ec);
        REQUIRE_FALSE(ec);

        Fixture f;
        std::string err;
        CHECK_FALSE(Arcane::Scene::SaveSceneFile(file, f.reg, id, &err));
        CHECK_FALSE(err.empty());

        std::filesystem::remove(tmp, ec);
    }

    CHECK(ReadAll(file) == original);
    CHECK_FALSE(std::filesystem::exists(tmp));   // no stray temp left behind

    // Still a scene the editor will open, not truncated remains.
    std::string err;
    const auto read = Arcane::Scene::ReadSceneFile(file, &err);
    REQUIRE(read.has_value());
    CHECK(read->id == id);
}
