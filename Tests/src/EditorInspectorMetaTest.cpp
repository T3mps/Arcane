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

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>

#include <memory>

namespace
{
    // Descriptors come from a plain ComponentRegistry rather than a Runtime:
    // a bare `Arcane::Runtime` in a test steals Arcane.dll's TypeContext slot,
    // after which reflection-driven work silently reports nothing.
    std::shared_ptr<Astra::ComponentRegistry> MakeSceneComponentRegistry()
    {
        auto creg = std::make_shared<Astra::ComponentRegistry>();
        Arcane::RegisterSceneComponents(*creg);
        return creg;
    }

    const Astra::FieldInfo* FindField(const Astra::ComponentDescriptor& desc,
                                      std::string_view name)
    {
        if (!desc.meta) return nullptr;
        for (const Astra::FieldInfo& f : desc.meta->fields)
            if (f.name == name) return &f;
        return nullptr;
    }
}

TEST_CASE("FieldDiffersFromDefault sees a changed field and not an unchanged one", "[editor]")
{
    auto creg = MakeSceneComponentRegistry();

    const Astra::ComponentDescriptor* desc =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::Transform>::Hash());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->meta != nullptr);

    const Astra::FieldInfo* rotation = FindField(*desc, "rotation");
    REQUIRE(rotation != nullptr);

    Arcane::Transform t;   // default-constructed: rotation is the default
    CHECK_FALSE(FieldDiffersFromDefault(*desc, *rotation, &t));

    t.rotation = 1.5f;
    CHECK(FieldDiffersFromDefault(*desc, *rotation, &t));

    // Reading the default back out gives the value a reset would write.
    float defaultRotation = -1.0f;
    ReadDefaultFieldBytes(*desc, *rotation, &defaultRotation);
    CHECK(defaultRotation == 0.0f);

    // A non-scalar default is the type's NSDMI, not zero -- scale is {1,1}, so
    // a comparison that only ever saw zeroed memory would get this wrong.
    const Astra::FieldInfo* scale = FindField(*desc, "scale");
    REQUIRE(scale != nullptr);
    CHECK_FALSE(FieldDiffersFromDefault(*desc, *scale, &t));

    glm::vec2 defaultScale{-1.0f, -1.0f};
    ReadDefaultFieldBytes(*desc, *scale, &defaultScale);
    CHECK(defaultScale.x == 1.0f);
    CHECK(defaultScale.y == 1.0f);

    t.scale.y = 2.0f;
    CHECK(FieldDiffersFromDefault(*desc, *scale, &t));
}

TEST_CASE("FieldDiffersFromDefault handles a component holding a string", "[editor]")
{
    // EntityInfo carries a std::string, so the scratch default instance MUST be
    // destructed or this leaks. Run under a leak checker if you have one; the
    // assertion here is just that it behaves.
    auto creg = MakeSceneComponentRegistry();

    const Astra::ComponentDescriptor* desc =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::EntityInfo>::Hash());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->meta != nullptr);

    const Astra::FieldInfo* name = FindField(*desc, "name");
    REQUIRE(name != nullptr);

    Arcane::EntityInfo info;
    CHECK_FALSE(FieldDiffersFromDefault(*desc, *name, &info));

    info.name = "Player";
    CHECK(FieldDiffersFromDefault(*desc, *name, &info));

    // Back to the default string: a name the user cleared must stop offering a
    // revert, so the comparison has to be by VALUE and not by "was it touched".
    info.name.clear();
    CHECK_FALSE(FieldDiffersFromDefault(*desc, *name, &info));
}

TEST_CASE("FieldDiffersFromDefault is inert on a tag component", "[editor]")
{
    // Arcane::Hidden is empty, so Astra gives its descriptor size 0 and there is
    // no scratch to build. A tag has no fields of its own, so the only way to
    // reach this path is a caller pairing the wrong field with the wrong
    // descriptor -- which must read nothing rather than run off the allocation.
    auto creg = MakeSceneComponentRegistry();

    const Astra::ComponentDescriptor* tag =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::Hidden>::Hash());
    REQUIRE(tag != nullptr);
    REQUIRE(tag->size == 0);

    const Astra::ComponentDescriptor* xform =
        creg->GetComponentDescriptorByHash(Astra::TypeID<Arcane::Transform>::Hash());
    REQUIRE(xform != nullptr);
    const Astra::FieldInfo* rotation = FindField(*xform, "rotation");
    REQUIRE(rotation != nullptr);

    Arcane::Transform t;
    t.rotation = 1.5f;
    CHECK_FALSE(FieldDiffersFromDefault(*tag, *rotation, &t));

    float out = -1.0f;
    ReadDefaultFieldBytes(*tag, *rotation, &out);
    CHECK(out == -1.0f);   // untouched: nothing was read

    // A null instance is the Inspector's "no component here" case.
    CHECK_FALSE(FieldDiffersFromDefault(*xform, *rotation, nullptr));
}
