// The Arcane:: ECS facade (input-seam spec 2026-10-02 s6.1): every alias is
// the SAME type as the library's, so it changes no ABI and no serialized name.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Ecs.hpp>

#include <Astra/Astra.hpp>

#include <type_traits>

namespace { struct C {}; struct S { void operator()(Astra::Registry&) {} }; }

TEST_CASE("Arcane ECS aliases are the library types", "[facade]")
{
    STATIC_REQUIRE(std::is_same_v<Arcane::Registry, Astra::Registry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Entity, Astra::Entity>);
    STATIC_REQUIRE(std::is_same_v<Arcane::View<C>, Astra::View<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Res<C>, Astra::Res<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ResMut<C>, Astra::ResMut<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Commands, Astra::Commands>);
    STATIC_REQUIRE(std::is_same_v<Arcane::SystemTraits<Arcane::Before<S>>, Astra::SystemTraits<Astra::Before<S>>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::After<S>, Astra::After<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::AmbiguousWith<S>, Astra::AmbiguousWith<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Reads<C>, Astra::Reads<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Writes<C>, Astra::Writes<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Exclusive, Astra::Exclusive>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ReadsResources<C>, Astra::ReadsResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::WritesResources<C>, Astra::WritesResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Not<C>, Astra::Not<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::With<C>, Astra::With<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Changed<C>, Astra::Changed<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Added<C>, Astra::Added<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Optional<C>, Astra::Optional<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Any<C>, Astra::Any<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::OneOf<C>, Astra::OneOf<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::IncludeDisabled<C>, Astra::IncludeDisabled<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::BinaryWriter, Astra::BinaryWriter>);
    STATIC_REQUIRE(std::is_same_v<Arcane::BinaryReader, Astra::BinaryReader>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ComponentModule, Astra::ComponentModule>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ComponentRegistry, Astra::ComponentRegistry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::TypeContext, Astra::TypeContext>);
    STATIC_REQUIRE(std::is_same_v<Arcane::SystemScheduler, Astra::SystemScheduler>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Tick, Astra::Tick>);
    STATIC_REQUIRE(std::is_same_v<Arcane::Result<int, Arcane::SerializationError>, Astra::Result<int, Astra::SerializationError>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::IWorkScheduler, Mosaic::IWorkScheduler>);
}

TEST_CASE("CVar flag test is HasFlag (Any is the query filter now)", "[facade][cvar]")
{
    CHECK(Arcane::HasFlag(Arcane::CVarFlags::Archive | Arcane::CVarFlags::Hidden, Arcane::CVarFlags::Hidden));
    CHECK_FALSE(Arcane::HasFlag(Arcane::CVarFlags::Archive, Arcane::CVarFlags::Hidden));
}
