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
