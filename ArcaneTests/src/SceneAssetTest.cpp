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
#include <optional>
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

// ---------------------------------------------------------------------------
// Schema v4: the save-time asset reference manifest (asset-manager Plan 2,
// spec s3.3). SaveJson's reflected-field walk already passes over every guid a
// scene names; v4 records the distinct, sorted, non-identity, non-nil ones as a
// top-level "assets" array so the asset panel can read a scene's outgoing
// references without re-scanning it. The four cases below pin the emission
// rule, the widened load gate, the loader's blindness to the manifest, and the
// unconditional key.
// ---------------------------------------------------------------------------

TEST_CASE("SaveSceneFile emits a v4 assets manifest: distinct, sorted, identity-excluded, nil-dropped",
          "[scene][json]")
{
    const std::filesystem::path dir  = TempDir("arcane_scene_asset_manifest");
    const std::filesystem::path file = dir / ("manifest" + std::string(Arcane::Scene::kSceneExt));

    // Chosen so the SORTED order is the REVERSE of the collection order: the
    // root carries `late` and the child -- walked after it, root-first BFS --
    // carries `early`. An unsorted manifest comes out [late, early] and fails.
    const std::optional<Arcane::Guid> early =
        Arcane::Guid::FromString("11111111-1111-4111-8111-111111111111");
    const std::optional<Arcane::Guid> late =
        Arcane::Guid::FromString("ffffffff-ffff-4fff-8fff-ffffffffffff");
    REQUIRE(early.has_value());
    REQUIRE(late.has_value());

    Fixture f;
    // Root: one reference (`late`).
    f.reg.AddComponent<Arcane::PostProcess>(f.root, Arcane::PostProcess{*late});
    // Child: `early` named TWICE, from two different components on two
    // different field names (the dedup case), plus two nil guids that must be
    // dropped (SpriteRenderer::sprite and MeshRenderer::materialOverride are
    // left at their nil defaults) -- and both entities already carry an
    // Identity guid, which the field-NAME rule must exclude.
    Arcane::SpriteRenderer sr;
    sr.material = *early;
    f.reg.AddComponent<Arcane::SpriteRenderer>(f.child, sr);
    Arcane::MeshRenderer mr;
    mr.mesh = *early;
    f.reg.AddComponent<Arcane::MeshRenderer>(f.child, mr);

    std::string err;
    REQUIRE(Arcane::Scene::SaveSceneFile(file, f.reg, Arcane::Guid::Generate(), &err));
    CHECK(err.empty());

    std::ifstream in(file, std::ios::binary);
    const nlohmann::json doc = nlohmann::json::parse(in);
    REQUIRE(doc["version"].get<int>() == 4);
    REQUIRE(doc.contains("assets"));
    REQUIRE(doc["assets"].is_array());
    REQUIRE(doc["assets"].size() == 2);   // `early` ONCE, despite its two mentions
    CHECK(doc["assets"][0].get<std::string>() == early->ToString());
    CHECK(doc["assets"][1].get<std::string>() == late->ToString());

    // Identity guids are the exclusion the field-name rule exists for: every
    // entity carries one and no asset registry can ever resolve it.
    const Arcane::Identity* rootInfo = f.reg.GetComponent<Arcane::Identity>(f.root);
    REQUIRE(rootInfo != nullptr);
    const std::string rootIdStr = rootInfo->id.ToString();
    const std::string nilStr    = Arcane::Guid::Nil().ToString();
    for (const nlohmann::json& g : doc["assets"])
    {
        CHECK(g.get<std::string>() != rootIdStr);
        CHECK(g.get<std::string>() != nilStr);
    }
}

TEST_CASE("a v3 scene still loads after the v4 bump", "[scene][json]")
{
    // The bump is ADDITIVE, so v3 must keep loading rather than being refused
    // at the envelope the way v1/v2 are. Stamped with a LITERAL 3, not the
    // symbolic constant: the claim is that this exact byte sequence survives a
    // constant that moved, and a symbolic stamp would move with it.
    const std::filesystem::path dir  = TempDir("arcane_scene_asset_v3");
    const std::filesystem::path file = dir / ("legacy" + std::string(Arcane::Scene::kSceneExt));
    const std::string tName(Astra::GetMeta<Arcane::Transform>()->typeName);

    nlohmann::json e0;
    e0["components"][tName]["position"] = { 100.0, 0.0, 0.0 };
    e0["parent"] = -1;
    nlohmann::json doc;
    doc["id"]       = "00000000-0000-0000-0000-000000000001";
    doc["version"]  = 3;   // LITERAL -- see above
    doc["entities"] = nlohmann::json::array({ e0 });
    std::ofstream(file, std::ios::binary) << doc.dump();

    std::string err;
    const auto read = Arcane::Scene::ReadSceneFile(file, &err);
    REQUIRE(read.has_value());
    CHECK(err.empty());

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry fresh{components};
    Arcane::RegisterSceneComponents(fresh);
    REQUIRE(Arcane::Scene::ApplySceneDocument(*read, fresh));

    const Arcane::SceneRoot* sr = fresh.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    const Arcane::Transform* t = fresh.GetComponent<Arcane::Transform>(sr->entity);
    REQUIRE(t != nullptr);
    CHECK(t->position.x == 100.0f);
}

TEST_CASE("the scene loader never reads the assets manifest", "[scene][json]")
{
    // Spec s3.3's safety property, and the reason the manifest can be a mere
    // index rather than scene data: a stale or hand-corrupted manifest may
    // mislead the asset panel, but it can never break a scene. Corrupt it past
    // anything a reader would tolerate -- a string and a bare number where
    // guid strings belong -- and every entity still loads intact.
    const std::filesystem::path dir  = TempDir("arcane_scene_asset_manifest_corrupt");
    const std::filesystem::path file = dir / ("corrupt" + std::string(Arcane::Scene::kSceneExt));

    {
        Fixture f;
        std::string err;
        REQUIRE(Arcane::Scene::SaveSceneFile(file, f.reg, Arcane::Guid::Generate(), &err));
    }

    nlohmann::json doc;
    {
        std::ifstream in(file, std::ios::binary);
        doc = nlohmann::json::parse(in);
    }
    REQUIRE(doc.contains("assets"));
    doc["assets"] = nlohmann::json::array({ "garbage", 42 });
    std::ofstream(file, std::ios::binary) << doc.dump();

    std::string err;
    const auto read = Arcane::Scene::ReadSceneFile(file, &err);
    REQUIRE(read.has_value());
    CHECK(err.empty());

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

TEST_CASE("the assets manifest is always emitted, even when it is empty", "[scene][json]")
{
    // The key is UNCONDITIONAL. That is what lets a consumer read "v4 with no
    // assets key" as a malformed file instead of having to guess between "this
    // scene references nothing" and "an older writer produced it".
    SECTION("a scene with entities but no asset references")
    {
        // The Fixture's only guids are Identity ids, which the field-name rule
        // excludes -- so the manifest is legitimately empty, not missing.
        Fixture f;
        const nlohmann::json doc = Arcane::Scene::SaveJson(f.reg);
        CHECK(doc["entities"].size() == 2);
        REQUIRE(doc.contains("assets"));
        CHECK(doc["assets"].is_array());
        CHECK(doc["assets"].empty());
    }
    SECTION("a registry with no SceneRoot at all")
    {
        // SaveJson's early return: the one path that never reaches the entity
        // walk, and so the one an "emit it after the loop" implementation
        // would silently skip.
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Arcane::RegisterSceneComponents(reg);

        const nlohmann::json doc = Arcane::Scene::SaveJson(reg);
        CHECK(doc["entities"].empty());
        REQUIRE(doc.contains("assets"));
        CHECK(doc["assets"].is_array());
        CHECK(doc["assets"].empty());
    }
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
