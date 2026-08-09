// Scene JSON: save the slice's entities (Transform + SpriteRenderer) + parent
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

#include <DiagnosticStore.hpp>

#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

// A 3rd reflected component type (beyond the Transform+SpriteRenderer pair)
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
        Arcane::Transform rootT; rootT.position = glm::vec2(100, 0);
        reg.AddComponent<Arcane::Transform>(root, rootT);
        reg.AddComponent<Arcane::SpriteRenderer>(root, Arcane::SpriteRenderer{});

        Astra::Entity child = reg.CreateEntity();
        Arcane::Transform childT; childT.position = glm::vec2(10, 5);
        reg.AddComponent<Arcane::Transform>(child, childT);
        Arcane::SpriteRenderer sr; sr.tint = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f); sr.sortingLayer = 2;
        reg.AddComponent<Arcane::SpriteRenderer>(child, sr);

        // The scene's post chain assignment (post arc slice 3): a PostProcess
        // component's material Guid must survive the JSON round-trip.
        Arcane::PostProcess post;
        post.material = Arcane::Guid{ 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull };
        reg.AddComponent<Arcane::PostProcess>(root, post);

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
    auto view = reg.CreateView<Arcane::Transform, Arcane::SpriteRenderer>();
    view.ForEach([&](Astra::Entity, Arcane::Transform&, Arcane::SpriteRenderer& sr)
    {
        ++sprites;
        if (sr.sortingLayer > maxLayer) maxLayer = sr.sortingLayer;
    });
    CHECK(sprites == 2);
    CHECK(maxLayer == 2);

    const Arcane::SceneRoot* root = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(root != nullptr);
    CHECK(reg.GetChildCount(root->entity) == 1);

    int postCount = 0;
    Arcane::Guid postGuid{};
    reg.CreateView<Arcane::PostProcess>().ForEach(
        [&](Astra::Entity, Arcane::PostProcess& pp)
    {
        ++postCount;
        postGuid = pp.material;
    });
    CHECK(postCount == 1);
    CHECK(postGuid == Arcane::Guid{ 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull });

    bool foundChild = false;
    auto ltView = reg.CreateView<Arcane::Transform>();
    ltView.ForEach([&](Astra::Entity, Arcane::Transform& lt)
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
        const std::string ltName(Astra::GetMeta<Arcane::Transform>()->typeName);
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
        auto view = reg->CreateView<Arcane::Transform>();
        view.ForEach([&](Astra::Entity, Arcane::Transform& lt)
        {
            ++count;
            CHECK(lt.position.x == Approx(0.0f));
            CHECK(lt.position.y == Approx(0.0f));
        });
        CHECK(count == 1);
    }
}

// Review finding (Fix 1): WorldTransform's only field (matrix) is
// Serializable(false), so SaveJson writes its component body as JSON null (the
// writer visits zero fields). The roster must stay faithful: key present ->
// component present. Before the fix, LoadJson's "!it.value().is_object()"
// guard skipped null bodies outright, so a reloaded entity silently lost its
// WorldTransform -- TransformPropagationSystem only WRITES an existing
// WorldTransform (never creates one) and RenderSubmissionSystem requires it
// to render, so the entity would neither propagate nor render after reload.
TEST_CASE("scene round-trip preserves an all-Serializable(false) component (WorldTransform)", "[json][scene]")
{
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);

        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
        reg.AddComponent<Arcane::SpriteRenderer>(root, Arcane::SpriteRenderer{});

        Astra::Entity child = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(child, Arcane::Transform{});
        reg.AddComponent<Arcane::WorldTransform>(child, Arcane::WorldTransform{});
        reg.AddComponent<Arcane::SpriteRenderer>(child, Arcane::SpriteRenderer{});
        reg.SetParent(child, root);

        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        doc = Arcane::Scene::SaveJson(reg);
    }

    // The all-non-serializable component still gets a roster key (as JSON
    // null -- there are zero fields to write for it). Look the key up via
    // GetMeta rather than hardcoding the string: TypeID<T>::Name() is
    // compiler-derived and may be namespace-qualified.
    const std::string worldName(Astra::GetMeta<Arcane::WorldTransform>()->typeName);
    REQUIRE(doc["entities"].size() == 2);
    for (const auto& entry : doc["entities"])
    {
        REQUIRE(entry["components"].contains(worldName));
        CHECK(entry["components"][worldName].is_null());
    }

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));

    int total = 0;
    int withWorld = 0;
    reg.CreateView<Arcane::Transform>().ForEach([&](Astra::Entity e, Arcane::Transform&)
    {
        ++total;
        if (reg.GetComponent<Arcane::WorldTransform>(e) != nullptr)
            ++withWorld;
    });
    CHECK(total == 2);
    CHECK(withWorld == 2);   // both entities kept their WorldTransform after reload
}

// Review finding (Fix 2): the reflection reader latches an error for a field
// whose TYPE the JSON bridge cannot represent (E02-3 "unsupported field
// type"), but AddComponentByTypeName previously ignored HasError() after
// populating -- a scene JSON hitting such a field silently mis-loaded
// (component added, but with the unsupported field never populated and no
// signal that anything went wrong). LoadJson must now fail loud instead.
namespace
{
    // Reflected + registered as a component (both fields are trivially
    // copyable, so Astra's binary POD path can still serialize the whole
    // struct), but "coord" is glm::ivec2 -- a type Detail::IsHandledType
    // (ReflectionJson.hpp) has no branch for (only float vec2/vec3/vec4 are
    // handled, and glm::ivec2 has no reflected TypeMeta of its own), so the
    // reader latches an "unsupported field type" error while populating it.
    struct BadField
    {
        int        id = 0;
        glm::ivec2 coord{0, 0};
    };
}
namespace { ASTRA_REFLECT_TYPE(BadField) ASTRA_REFLECT_FIELD(BadField, id) ASTRA_REFLECT_FIELD(BadField, coord) ASTRA_END_REFLECT_TYPE() }

TEST_CASE("scene JSON load fails loud on a component with an unsupported field type", "[json][scene]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    components->RegisterComponent<BadField>();

    const std::string badFieldName(Astra::GetMeta<BadField>()->typeName);
    nlohmann::json entry;
    entry["components"][badFieldName]["id"] = 7;
    entry["parent"] = -1;
    nlohmann::json doc;
    doc["version"] = Arcane::Scene::kSceneJsonVersion;
    doc["entities"] = nlohmann::json::array({ entry });

    bool result = true;
    CHECK_NOTHROW(result = Arcane::Scene::LoadJson(reg, doc));
    CHECK_FALSE(result);   // the reader's latched error must fail the whole load
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
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});

        Astra::Entity a = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(a, Arcane::Transform{});
        reg.AddComponent<Tag>(a, Tag{111});
        reg.SetParent(a, root);

        Astra::Entity b = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(b, Arcane::Transform{});
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

// Task 7: the highest-consequence diagnostic in the engine -- a skipped
// component means "re-saving this scene will drop it permanently" (see the
// ARC_WARN text in SceneSerializer.hpp). This proves the skip is now ALSO
// visible as a structured diagnostic, not just a log line.
TEST_CASE("Loading a scene with an unknown component publishes a scene diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    // Hand-built scene JSON carrying a component name no reflected type
    // matches. Mirrors the shape SaveJson emits above ("version" / "entities"
    // / per-entity "components"); only the component key is bogus.
    nlohmann::json entity;
    entity["components"]["ThisComponentTypeDoesNotExist"] = nlohmann::json::object();
    entity["parent"] = -1;
    nlohmann::json doc;
    doc["version"] = Arcane::Scene::kSceneJsonVersion;
    doc["entities"] = nlohmann::json::array({ entity });

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    CHECK(Arcane::Scene::LoadJson(reg, doc));   // an unknown component is tolerated, not fatal

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "scene.component.unknown");
    CHECK(rows[0].severity == Arcane::DiagSeverity::Error);
    CHECK(rows[0].scope == Arcane::DiagScope::Scene);
    CHECK(rows[0].locator.kind == Arcane::DiagLocator::Kind::Entity);
    CHECK_FALSE(rows[0].detail.empty());   // the permanent-data-loss consequence

    store.UninstallEngineSink();
}
