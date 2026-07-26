// Arcane Editor inspector: field classification + reflected write-back. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <InspectorFields.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "Helpers/TestTypeContext.hpp"

namespace
{
    // Live registry for the mixed-value tests. Pins Arcane.dll's TypeContext to
    // the shared one (never a bare Arcane::Runtime -- that installs an unshared
    // context and Edit:: ops then silently report 0 changes).
    struct MixedWorld
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };

        MixedWorld()
        {
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            Arcane::RegisterSceneComponents(reg);
        }

        Astra::Entity Make(glm::vec2 pos, float rot)
        {
            Astra::Entity e = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{ pos, rot, glm::vec2(1.0f) });
            return e;
        }

        std::uint64_t TransformHash() const
        {
            const Astra::ComponentDescriptor* d =
                creg->GetComponentDescriptor(Astra::TypeID<Arcane::Transform>::Value());
            return d ? d->hash : 0;
        }
    };

    const Astra::FieldInfo* TransformField(const char* name)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::Transform>();
        if (!meta) return nullptr;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == name)
                return &f;
        return nullptr;
    }
}

TEST_CASE("ClassifyField maps arithmetic/bool fields, ReadOnly otherwise", "[editor]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    bool sawFloatOrInt = false;
    for (const Astra::FieldInfo& f : meta->fields)
    {
        const Arcane::Editor::FieldKind k = Arcane::Editor::ClassifyField(f);
        if (f.name == "sortingLayer" || f.name == "orderInLayer")
            CHECK(k == Arcane::Editor::FieldKind::Int32);
        if (k == Arcane::Editor::FieldKind::Float || k == Arcane::Editor::FieldKind::Int32) sawFloatOrInt = true;
    }
    CHECK(sawFloatOrInt);

    // tint is a glm::vec4 -- no Vec4 editor exists, so it must classify ReadOnly
    // rather than being silently misclassified (regression coverage for the
    // now-removed size==1-arithmetic bool fallback).
    const Astra::FieldInfo* tintField = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "tint") tintField = &f;
    REQUIRE(tintField != nullptr);
    CHECK(Arcane::Editor::ClassifyField(*tintField) == Arcane::Editor::FieldKind::ReadOnly);
}

TEST_CASE("ApplyIntEdit writes through reflection to the live component", "[editor]")
{
    Arcane::SpriteRenderer sprite;
    sprite.sortingLayer = 0;
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    const Astra::FieldInfo* layer = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "sortingLayer") layer = &f;
    REQUIRE(layer != nullptr);

    Arcane::Editor::ApplyIntEdit(*layer, &sprite, 7);
    CHECK(sprite.sortingLayer == 7);
}

TEST_CASE("Guid fields classify AssetRef and round-trip through ApplyGuidEdit", "[editor]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    const Astra::FieldInfo* material = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "material") material = &f;
    REQUIRE(material != nullptr);
    CHECK(Arcane::Editor::ClassifyField(*material) == Arcane::Editor::FieldKind::AssetRef);

    Arcane::SpriteRenderer sprite;
    REQUIRE_FALSE(sprite.material.IsValid());

    const Arcane::Guid g = Arcane::Guid::FromString("aaaa1111-1111-4111-8111-111111111111").value();
    Arcane::Editor::ApplyGuidEdit(*material, &sprite, g);
    CHECK(sprite.material == g);

    Arcane::Editor::ApplyGuidEdit(*material, &sprite, Arcane::Guid::Nil());
    CHECK_FALSE(sprite.material.IsValid());
}

// --- Multi-select mixed-value mask (UE parity) ---------------------------------
// Ground truth: FComponentTransformDetails::CacheDetails, vendored UE 5.6 at
// ComponentTransformDetails.cpp:1215-1225 -- per-axis, first carrier seeds, later
// carriers mark differing axes, and a marked axis STAYS marked.

TEST_CASE("FieldComponentCount reports scalar width per kind", "[editor]")
{
    using Arcane::Editor::FieldComponentCount;
    using K = Arcane::Editor::FieldKind;
    CHECK(FieldComponentCount(K::Vec3) == 3);
    CHECK(FieldComponentCount(K::Vec2) == 2);
    CHECK(FieldComponentCount(K::Float) == 1);
    CHECK(FieldComponentCount(K::Int32) == 1);
    CHECK(FieldComponentCount(K::Bool) == 1);
    CHECK(FieldComponentCount(K::AssetRef) == 1);
    CHECK(FieldComponentCount(K::ReadOnly) == 1);
}

TEST_CASE("ComputeFieldMixed: a single selected entity is never mixed", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(1.0f, 2.0f), 0.5f);
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    const std::array<Astra::Entity, 1> sel{ a };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos).Any());
}

TEST_CASE("ComputeFieldMixed: identical values are not mixed", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(3.0f, 4.0f), 1.0f);
    const Astra::Entity b = w.Make(glm::vec2(3.0f, 4.0f), 1.0f);
    const Astra::FieldInfo* pos = TransformField("position");
    const Astra::FieldInfo* rot = TransformField("rotation");
    REQUIRE(pos != nullptr);
    REQUIRE(rot != nullptr);

    const std::array<Astra::Entity, 2> sel{ a, b };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos).Any());
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *rot).Any());
}

TEST_CASE("ComputeFieldMixed marks ONLY the axis that differs", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(3.0f, 4.0f), 1.0f);
    const Astra::Entity b = w.Make(glm::vec2(9.0f, 4.0f), 1.0f);   // x differs, y same
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    const std::array<Astra::Entity, 2> sel{ a, b };
    const Arcane::Editor::FieldMixedMask m =
        Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos);
    CHECK(m.Any());
    CHECK(m.Test(0));         // x mixed
    CHECK_FALSE(m.Test(1));   // y agreed

    // A scalar field on the same entities is independent.
    const Astra::FieldInfo* rot = TransformField("rotation");
    REQUIRE(rot != nullptr);
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *rot).Any());
}

TEST_CASE("ComputeFieldMixed is STICKY: a later match does not clear a marked axis", "[editor]")
{
    // THE case a naive "compare against the previous entity" implementation gets
    // wrong. UE's `&& Cached.IsSet()` term is what makes it sticky.
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(1.0f, 0.0f), 0.0f);
    const Astra::Entity b = w.Make(glm::vec2(2.0f, 0.0f), 0.0f);   // x diverges here
    const Astra::Entity c = w.Make(glm::vec2(1.0f, 0.0f), 0.0f);   // and matches the SEED again
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    const std::array<Astra::Entity, 3> sel{ a, b, c };
    const Arcane::Editor::FieldMixedMask m =
        Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos);
    CHECK(m.Test(0));         // still mixed despite c agreeing with a
    CHECK_FALSE(m.Test(1));
}

TEST_CASE("ComputeFieldMixed skips dead entities and non-carriers", "[editor]")
{
    MixedWorld w;
    const Astra::Entity a = w.Make(glm::vec2(5.0f, 5.0f), 0.0f);
    const Astra::Entity b = w.Make(glm::vec2(5.0f, 5.0f), 0.0f);
    const Astra::Entity bare = w.reg.CreateEntity();          // no Transform at all
    const Astra::Entity doomed = w.Make(glm::vec2(99.0f, 99.0f), 9.0f);
    const std::array<Astra::Entity, 1> kill{ doomed };
    REQUIRE(Arcane::Edit::DeleteEntities(w.reg, kill) == 1);

    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);

    // Neither the bare entity nor the dead one may make this look mixed.
    const std::array<Astra::Entity, 4> sel{ a, b, bare, doomed };
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(w.reg, sel, w.TransformHash(), *pos).Any());
}

TEST_CASE("ComputeFieldMixed on an empty selection is not mixed", "[editor]")
{
    MixedWorld w;
    const Astra::FieldInfo* pos = TransformField("position");
    REQUIRE(pos != nullptr);
    CHECK_FALSE(Arcane::Editor::ComputeFieldMixed(
        w.reg, std::span<const Astra::Entity>{}, w.TransformHash(), *pos).Any());
}
