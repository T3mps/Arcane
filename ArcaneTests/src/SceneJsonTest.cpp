// Scene JSON: save the slice's entities (Transform + SpriteRenderer) + parent
// links to JSON via the seam, then load into a fresh registry (typed roster) and
// assert transforms + hierarchy survived. Proves the north-star path end to end.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <Panels/DiagnosticStore.hpp>

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
        Arcane::Transform rootT; rootT.position = glm::vec3(100, 0, 0);
        reg.AddComponent<Arcane::Transform>(root, rootT);
        reg.AddComponent<Arcane::SpriteRenderer>(root, Arcane::SpriteRenderer{});

        Astra::Entity child = reg.CreateEntity();
        Arcane::Transform childT; childT.position = glm::vec3(10, 5, 0);
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
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = 42;   // built from the constant so a future bump cannot strand it
        bool result = true;
        CHECK_NOTHROW(result = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(result);
    }

    SECTION("an entity entry is not an object")
    {
        auto reg = FreshReg();
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ 7 });
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

    SECTION("a wrong-typed COMPONENT FIELD is refused, not silently defaulted")
    {
        // Task 3 (F1) inverted this case's verdict, deliberately. `position`
        // should be an [x,y,z] array; a string is a value the file HAS and that
        // the load would throw away. Silently defaulting it is exactly how a
        // stale 2D scene loaded with every entity at the origin and nothing
        // anywhere saying so. Still no throw -- that half is untouched.
        auto reg = FreshReg();
        const std::string ltName(Astra::GetMeta<Arcane::Transform>()->typeName);
        nlohmann::json entry;
        entry["components"][ltName]["position"] = "oops";
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ entry });

        bool result = true;
        CHECK_NOTHROW(result = Arcane::Scene::LoadJson(*reg, doc));
        CHECK_FALSE(result);
    }

    SECTION("a wrong-typed PARENT is still tolerated, not thrown")
    {
        // The parent guard lives in SceneSerializer's own entity walk, NOT in
        // the reflection reader, and its tolerance is UNCHANGED: a non-integer
        // parent means "no parent" and a structurally valid entry still loads.
        // Its own case now, so the field-level tightening above cannot be
        // mistaken for a change to this one.
        auto reg = FreshReg();
        const std::string ltName(Astra::GetMeta<Arcane::Transform>()->typeName);
        nlohmann::json entry;
        entry["components"][ltName]["position"] = { 4.0, 5.0, 6.0 };
        entry["parent"] = "root";
        nlohmann::json doc;
        doc["version"] = Arcane::Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array({ entry });

        bool result = false;
        CHECK_NOTHROW(result = Arcane::Scene::LoadJson(*reg, doc));
        CHECK(result);

        int count = 0;
        auto view = reg->CreateView<Arcane::Transform>();
        view.ForEach([&](Astra::Entity e, Arcane::Transform& lt)
        {
            ++count;
            CHECK(lt.position.x == Approx(4.0f));   // the well-formed field still loaded
            CHECK(lt.position.z == Approx(6.0f));
            CHECK_FALSE(reg->HasParent(e));         // the malformed parent became "no parent"
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

    // Producer-side round-trip contract (final-review fix): the locator must
    // carry the entity's FULL packed value (GetValue(), id+version), not the
    // version-stripped GetID() -- the consumer (EditorApp::RouteLocator)
    // reconstructs via Astra::Entity(StorageType), which expects the packed
    // value, and no live entity has version 0. This document has exactly one
    // entity, and LoadJson sets SceneRoot to it on success, so that is the
    // real handle to pin the reconstruction against.
    const Arcane::SceneRoot* root = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(root != nullptr);
    const Astra::Entity reconstructed(
        static_cast<Astra::Entity::StorageType>(rows[0].locator.entity));
    CHECK(reconstructed == root->entity);

    store.UninstallEngineSink();
}

// Final-review finding 1 (F1): the malformed-field diagnostic was WRITTEN AND
// NEVER READ. ReflectionJson builds a message naming the field -- "malformed
// JSON value for field 'position' ..." -- AddComponentByTypeName latched it,
// and LoadJson then read only HasError() and returned a bare `false`. The
// editor's modal said "'<file>' parsed but could not be loaded (see Console)"
// and the Console held nothing but that same sentence, while the far less
// serious Skipped* cases twenty lines below each got an ARC_WARN AND a
// published Arcane::Diagnostic.
//
// Before Task 3 that path was reachable only by an unsupported field TYPE -- a
// code defect a developer finds anyway. Task 3 made it reachable BY DATA, on
// exactly the hand-edited-file route the reader documents itself as existing
// to survive. So the message has to come out.
TEST_CASE("scene JSON load publishes a diagnostic naming the malformed field",
          "[diagnostics][json][scene]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    // "position" is PRESENT and carries a two-element array -- the stale 2D
    // Transform shape, which is the exact case Task 3 stopped reading as
    // "absent". Everything else about the document is well-formed.
    nlohmann::json entity;
    entity["components"]["Arcane::Transform"]["position"] =
        nlohmann::json::array({ 1.0, 2.0 });
    entity["parent"] = -1;
    nlohmann::json doc;
    doc["version"] = Arcane::Scene::kSceneJsonVersion;
    doc["entities"] = nlohmann::json::array({ entity });

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    // Still refuses -- Task 3's whole thesis. The change is that it is now loud.
    CHECK_FALSE(Arcane::Scene::LoadJson(reg, doc));

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "scene.component.malformed");
    CHECK(rows[0].severity == Arcane::DiagSeverity::Error);
    CHECK(rows[0].scope == Arcane::DiagScope::Scene);
    // The component name reaches the row...
    CHECK(rows[0].message.find("Arcane::Transform") != std::string::npos);
    // ... and so does the reader's own message, which is the ONLY thing that
    // names the offending FIELD. Dropping this was the finding.
    CHECK(rows[0].detail.find("position") != std::string::npos);

    store.UninstallEngineSink();
}

// Final-review, pre-existing landmine (NOT F1's doing, fixed here because it is
// one click away from the desk session this work is handed to).
//
// Collider2D::fixtures is a std::vector<Fixture>. Detail::IsHandledType has no
// vector branch, and ReflectionJsonReader::Visit classifies by TYPE before it
// calls Find() -- so the reader latched "unsupported field type" EVEN WITH THE
// KEY ABSENT, which is the only way the key ever is (the writer cannot emit it
// either). Collider2D is not structure-locked, so a user can add one from the
// Inspector; SaveJson ignores writer errors and writes the file anyway. Net:
// add a Collider2D in the editor, save, and that scene can never be opened.
//
// The fix marks the field Serializable(false) -- the same mechanism
// PhysicsBodyRef::handle and WorldTransform::matrix already use -- so it leaves
// BOTH walks, and Collider2D round-trips as a present-but-empty component. This
// pins the round trip, which is what "the scene opens again" means.
TEST_CASE("a scene carrying Collider2D round-trips (the vector field is out of the JSON contract)",
          "[json][scene]")
{
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents(reg);

        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});

        // Exactly what the Inspector's Add Component produces: a default
        // Collider2D, no fixtures authored (there is no vector editor).
        Arcane::Collider2D col;
        reg.AddComponent<Arcane::Collider2D>(root, col);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        doc = Arcane::Scene::SaveJson(reg);
    }

    // The roster stays faithful: the key is present, so the component is.
    REQUIRE(doc["entities"][0]["components"].contains("Arcane::Collider2D"));

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    // THE regression: this returned false before the fix, permanently.
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));

    int colliders = 0;
    reg.CreateView<Arcane::Collider2D>().ForEach(
        [&](Astra::Entity, Arcane::Collider2D&) { ++colliders; });
    CHECK(colliders == 1);
}
