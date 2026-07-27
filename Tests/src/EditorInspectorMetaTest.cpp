// InspectorMeta: the Inspector's PURE decisions -- what a field is called, what
// category it belongs to, whether a filter matches it. No ImGui, so the whole
// surface the user reads is pinned here rather than desk-verified.

#include <catch2/catch_test_macros.hpp>

#include "InspectorMeta.hpp"

using namespace Arcane::Editor;

TEST_CASE("DeriveDisplayName turns identifiers into words", "[editor]")
{
    CHECK(DeriveDisplayName("sortingLayer") == "Sorting Layer");
    CHECK(DeriveDisplayName("orderInLayer") == "Order In Layer");
    CHECK(DeriveDisplayName("order_in_layer") == "Order In Layer");
    CHECK(DeriveDisplayName("size") == "Size");
    CHECK(DeriveDisplayName("Position") == "Position");
    CHECK(DeriveDisplayName("textureId") == "Texture Id");

    // Acronym boundary: the run stays together and the next word splits off.
    CHECK(DeriveDisplayName("HTTPServer") == "HTTP Server");
    CHECK(DeriveDisplayName("useHDR") == "Use HDR");

    // A digit ends a word, so a trailing number reads as its own token.
    CHECK(DeriveDisplayName("vec2Field") == "Vec2 Field");

    // Already-readable input must survive untouched rather than gain spaces.
    CHECK(DeriveDisplayName("Already Spaced") == "Already Spaced");

    // Degenerate input must not crash or produce stray spaces.
    CHECK(DeriveDisplayName("") == "");
    CHECK(DeriveDisplayName("_") == "");
    CHECK(DeriveDisplayName("__a__b__") == "A B");
}

TEST_CASE("DisplayNameForComponent strips the namespace first", "[editor]")
{
    CHECK(DisplayNameForComponent("Arcane::SpriteRenderer") == "Sprite Renderer");
    CHECK(DisplayNameForComponent("Arcane::EntityInfo") == "Entity Info");
    CHECK(DisplayNameForComponent("Transform") == "Transform");
    // Nested namespaces: only the trailing type name matters.
    CHECK(DisplayNameForComponent("A::B::PostProcess") == "Post Process");
    CHECK(DisplayNameForComponent("") == "");
}

#include <Astra/Reflection/Reflection.hpp>

namespace
{
    // A throwaway reflected type carrying one of every attribute this module
    // reads, plus a bare field, so both the present and absent paths are real
    // rather than hand-built FieldInfo values.
    struct MetaProbe
    {
        float ranged   = 0.0f;
        int   locked   = 0;
        int   secret   = 0;
        float described = 0.0f;
        int   bare     = 0;
    };

    ASTRA_REFLECT_TYPE(MetaProbe)
        ASTRA_REFLECT_FIELD(MetaProbe, ranged)
            ASTRA_REFLECT_ATTR(Range, 0.0, 10.0, 0.5)
            ASTRA_REFLECT_ATTR(Category, "Bounds")
        ASTRA_REFLECT_FIELD(MetaProbe, locked)
            ASTRA_REFLECT_ATTR(ReadOnly)
        ASTRA_REFLECT_FIELD(MetaProbe, secret)
            ASTRA_REFLECT_ATTR(Hidden)
        ASTRA_REFLECT_FIELD(MetaProbe, described)
            ASTRA_REFLECT_ATTR(DisplayName, "Custom Label")
            ASTRA_REFLECT_ATTR(Tooltip, "explains itself")
        ASTRA_REFLECT_FIELD(MetaProbe, bare)
    ASTRA_REFLECT_TYPE_END()

    const Astra::FieldInfo& ProbeField(std::string_view name)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<MetaProbe>();
        REQUIRE(meta != nullptr);
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == name) return f;
        FAIL("no such field: " << std::string(name));
        return meta->fields[0];   // unreachable; keeps the compiler happy
    }
}

TEST_CASE("attributes are read when present", "[editor]")
{
    const auto range = RangeOfField(ProbeField("ranged"));
    REQUIRE(range.has_value());
    CHECK(range->min == 0.0);
    CHECK(range->max == 10.0);
    CHECK(range->step == 0.5);

    CHECK(CategoryOfField(ProbeField("ranged")) == "Bounds");
    CHECK(FieldIsReadOnly(ProbeField("locked")));
    CHECK(FieldIsAttributeHidden(ProbeField("secret")));
    CHECK(TooltipOfField(ProbeField("described")) == "explains itself");

    // An explicit DisplayName beats derivation -- the author overriding it is
    // the whole point of the attribute.
    CHECK(DisplayNameForField(ProbeField("described")) == "Custom Label");
}

TEST_CASE("a field with no attributes falls back cleanly", "[editor]")
{
    const Astra::FieldInfo& bare = ProbeField("bare");
    CHECK_FALSE(RangeOfField(bare).has_value());
    CHECK(CategoryOfField(bare).empty());
    CHECK(TooltipOfField(bare).empty());
    CHECK_FALSE(FieldIsReadOnly(bare));
    CHECK_FALSE(FieldIsAttributeHidden(bare));
    CHECK(DisplayNameForField(bare) == "Bare");
}

TEST_CASE("the Inspector filter matches either spelling", "[editor]")
{
    // Friendly name and raw identifier both hit, so it does not matter whether
    // the user knows the code.
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Sorting Layer", "sortingLayer", "sorting"));
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Sorting Layer", "sortingLayer", "sortingLayer"));
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Sorting Layer", "sortingLayer", "LAYER"));

    // A component-name hit keeps every field, so searching "sprite" shows the
    // whole component rather than none of it.
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Tint", "tint", "sprite"));
    CHECK(ComponentMatchesFilter("Sprite Renderer", "sprite"));

    // Empty query matches everything -- the unfiltered case is not special-cased
    // at the call site.
    CHECK(MatchesInspectorFilter("Sprite Renderer", "Tint", "tint", ""));
    CHECK(ComponentMatchesFilter("Sprite Renderer", ""));

    // A genuine miss.
    CHECK_FALSE(MatchesInspectorFilter("Sprite Renderer", "Tint", "tint", "physics"));
    CHECK_FALSE(ComponentMatchesFilter("Sprite Renderer", "physics"));
}
