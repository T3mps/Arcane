// Scene JSON: save the slice's entities (LocalTransform + SpriteRenderer) + parent
// links to JSON via the seam, then load into a fresh registry (typed roster) and
// assert transforms + hierarchy survived. Proves the north-star path end to end.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

// A 3rd reflected component type (beyond the LocalTransform+SpriteRenderer pair)
// to prove the scene JSON roster is reflection-driven, not hardcoded.
namespace { struct Tag { int value = 0; }; }
namespace { ASTRA_REFLECT_TYPE(Tag) ASTRA_REFLECT_FIELD(Tag, value) ASTRA_END_REFLECT_TYPE() }

TEST_CASE("scene round-trips through JSON (typed roster)", "[json][scene]")
{
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);

        Astra::Entity root = reg.CreateEntity();
        Arcane::LocalTransform rootT; rootT.position = glm::vec2(100, 0);
        reg.AddComponent<Arcane::LocalTransform>(root, rootT);
        reg.AddComponent<Arcane::SpriteRenderer>(root, Arcane::SpriteRenderer{});

        Astra::Entity child = reg.CreateEntity();
        Arcane::LocalTransform childT; childT.position = glm::vec2(10, 5);
        reg.AddComponent<Arcane::LocalTransform>(child, childT);
        Arcane::SpriteRenderer sr; sr.tint = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f); sr.sortingLayer = 2;
        reg.AddComponent<Arcane::SpriteRenderer>(child, sr);

        reg.SetParent(child, root);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        doc = Arcane::Scene::SaveJson(reg);
    }

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));

    int sprites = 0;
    int maxLayer = -1;
    auto view = reg.CreateView<Arcane::LocalTransform, Arcane::SpriteRenderer>();
    view.ForEach([&](Astra::Entity, Arcane::LocalTransform&, Arcane::SpriteRenderer& sr)
    {
        ++sprites;
        if (sr.sortingLayer > maxLayer) maxLayer = sr.sortingLayer;
    });
    CHECK(sprites == 2);
    CHECK(maxLayer == 2);

    const Arcane::SceneRoot* root = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(root != nullptr);
    CHECK(reg.GetChildCount(root->entity) == 1);

    bool foundChild = false;
    auto ltView = reg.CreateView<Arcane::LocalTransform>();
    ltView.ForEach([&](Astra::Entity, Arcane::LocalTransform& lt)
    {
        if (lt.position.x > 5.0f && lt.position.x < 15.0f)   // the child at (10,5)
        {
            foundChild = true;
            CHECK(lt.position.x == Approx(10.0f));
            CHECK(lt.position.y == Approx(5.0f));
        }
    });
    CHECK(foundChild);
}

TEST_CASE("scene JSON loader rejects malformed input without throwing", "[json][scene]")
{
    auto FreshReg = []
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    };

    // The engine is exception-free: a hand-edited/corrupt scene file must come
    // back as a clean `false`, never an nlohmann type_error thrown through the
    // Result-typed loader. Each of these threw before the guards were added.

    SECTION("entities is not an array")
    {
        auto reg = FreshReg();
        const nlohmann::json doc = nlohmann::json::parse(R"({"version": 2, "entities": 42})");
        bool result = true;
        CHECK_NOTHROW(result = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(result);
    }

    SECTION("an entity entry is not an object")
    {
        auto reg = FreshReg();
        const nlohmann::json doc = nlohmann::json::parse(R"({"version": 2, "entities": [ 7 ]})");
        bool result = true;
        CHECK_NOTHROW(result = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(result);
    }

    SECTION("top-level document is not an object")
    {
        auto reg = FreshReg();
        const nlohmann::json doc = nlohmann::json::parse(R"([1, 2, 3])");
        bool result = true;
        CHECK_NOTHROW(result = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(result);
    }

    SECTION("wrong-typed leaf fields and parent are tolerated, not thrown")
    {
        // position should be a [x,y] array and parent an int; here both are the
        // wrong JSON type. The guarded readers skip them (leaving defaults) rather
        // than throwing, so a structurally valid entry still loads successfully.
        auto reg = FreshReg();
        const std::string ltName(Astra::GetMeta<Arcane::LocalTransform>()->typeName);
        nlohmann::json entry;
        entry["components"][ltName]["position"] = "oops";
        entry["parent"] = "root";
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ entry });

        bool result = false;
        CHECK_NOTHROW(result = Arcane::Scene::LoadJson(*reg, doc));
        CHECK(result);

        // The corrupt position fell back to the default (0,0), no crash.
        int count = 0;
        auto view = reg->CreateView<Arcane::LocalTransform>();
        view.ForEach([&](Astra::Entity, Arcane::LocalTransform& lt)
        {
            ++count;
            CHECK(lt.position.x == Approx(0.0f));
            CHECK(lt.position.y == Approx(0.0f));
        });
        CHECK(count == 1);
    }
}

TEST_CASE("scene JSON carries a version and rejects a mismatch", "[json][scene]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    nlohmann::json doc;
    doc["version"] = Arcane::Scene::kSceneJsonVersion + 999;   // deliberately wrong
    doc["entities"] = nlohmann::json::array();

    bool result = true;
    CHECK_NOTHROW(result = Arcane::Scene::LoadJson(reg, doc));
    CHECK_FALSE(result);   // version mismatch detected + reported, not mis-parsed

    // Same body at the correct version loads -> it was the version that was rejected.
    doc["version"] = Arcane::Scene::kSceneJsonVersion;
    CHECK(Arcane::Scene::LoadJson(reg, doc));
}

TEST_CASE("scene with a 3rd component type and a non-parent link round-trips", "[json][scene]")
{
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        components->RegisterComponent<Tag>();

        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::LocalTransform>(root, Arcane::LocalTransform{});

        Astra::Entity a = reg.CreateEntity();
        reg.AddComponent<Arcane::LocalTransform>(a, Arcane::LocalTransform{});
        reg.AddComponent<Tag>(a, Tag{111});
        reg.SetParent(a, root);

        Astra::Entity b = reg.CreateEntity();
        reg.AddComponent<Arcane::LocalTransform>(b, Arcane::LocalTransform{});
        reg.AddComponent<Tag>(b, Tag{222});
        reg.SetParent(b, root);

        reg.AddLink(a, b);   // non-hierarchical link between siblings (not a parent edge)
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        doc = Arcane::Scene::SaveJson(reg);
    }

    CHECK(doc["version"].get<int>() == Arcane::Scene::kSceneJsonVersion);

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    components->RegisterComponent<Tag>();
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));

    // The 3rd component type survived the round-trip through JSON.
    int tagCount = 0;
    int tagSum = 0;
    std::vector<Astra::Entity> tagged;
    reg.CreateView<Tag>().ForEach([&](Astra::Entity e, Tag& t)
    {
        ++tagCount;
        tagSum += t.value;
        tagged.push_back(e);
    });
    CHECK(tagCount == 2);
    CHECK(tagSum == 333);

    // The non-hierarchical link survived (bidirectional after AddLink on load).
    REQUIRE(tagged.size() == 2);
    bool linked = false;
    for (Astra::Entity l : reg.GetRelationshipGraph().GetLinks(tagged[0]))
        if (l == tagged[1]) linked = true;
    CHECK(linked);
}
