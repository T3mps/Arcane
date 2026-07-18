// Grimoire inspector: field classification + reflected write-back. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>

#include <Arcane/Scene/Components.hpp>

#include <InspectorFields.hpp>

TEST_CASE("ClassifyField maps arithmetic/bool fields, ReadOnly otherwise", "[grimoire]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    bool sawFloatOrInt = false;
    for (const Astra::FieldInfo& f : meta->fields)
    {
        const Grimoire::FieldKind k = Grimoire::ClassifyField(f);
        if (f.name == "sortingLayer" || f.name == "orderInLayer")
            CHECK(k == Grimoire::FieldKind::Int32);
        if (k == Grimoire::FieldKind::Float || k == Grimoire::FieldKind::Int32) sawFloatOrInt = true;
    }
    CHECK(sawFloatOrInt);

    // tint is a glm::vec4 -- no Vec4 editor exists, so it must classify ReadOnly
    // rather than being silently misclassified (regression coverage for the
    // now-removed size==1-arithmetic bool fallback).
    const Astra::FieldInfo* tintField = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "tint") tintField = &f;
    REQUIRE(tintField != nullptr);
    CHECK(Grimoire::ClassifyField(*tintField) == Grimoire::FieldKind::ReadOnly);
}

TEST_CASE("ApplyIntEdit writes through reflection to the live component", "[grimoire]")
{
    Arcane::SpriteRenderer sprite;
    sprite.sortingLayer = 0;
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::SpriteRenderer>();
    REQUIRE(meta != nullptr);

    const Astra::FieldInfo* layer = nullptr;
    for (const Astra::FieldInfo& f : meta->fields) if (f.name == "sortingLayer") layer = &f;
    REQUIRE(layer != nullptr);

    Grimoire::ApplyIntEdit(*layer, &sprite, 7);
    CHECK(sprite.sortingLayer == 7);
}
