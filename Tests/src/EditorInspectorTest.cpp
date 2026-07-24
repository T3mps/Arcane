// Arcane Editor inspector: field classification + reflected write-back. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>

#include <Arcane/Scene/Components.hpp>

#include <InspectorFields.hpp>

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
