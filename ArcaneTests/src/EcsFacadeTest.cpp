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
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Registry, Astra::Registry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Entity, Astra::Entity>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::View<C>, Astra::View<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Res<C>, Astra::Res<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::ResMut<C>, Astra::ResMut<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Commands, Astra::Commands>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::SystemTraits<Arcane::ECS::Before<S>>, Astra::SystemTraits<Astra::Before<S>>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::After<S>, Astra::After<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::AmbiguousWith<S>, Astra::AmbiguousWith<S>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Reads<C>, Astra::Reads<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Writes<C>, Astra::Writes<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Exclusive, Astra::Exclusive>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::ReadsResources<C>, Astra::ReadsResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::WritesResources<C>, Astra::WritesResources<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Not<C>, Astra::Not<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::With<C>, Astra::With<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Changed<C>, Astra::Changed<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Added<C>, Astra::Added<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Optional<C>, Astra::Optional<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Any<C>, Astra::Any<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::OneOf<C>, Astra::OneOf<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::IncludeDisabled<C>, Astra::IncludeDisabled<C>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::BinaryWriter, Astra::BinaryWriter>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::BinaryReader, Astra::BinaryReader>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::ComponentModule, Astra::ComponentModule>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::ComponentRegistry, Astra::ComponentRegistry>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::TypeContext, Astra::TypeContext>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::SystemScheduler, Astra::SystemScheduler>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Tick, Astra::Tick>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Result<int, Arcane::ECS::SerializationError>, Astra::Result<int, Astra::SerializationError>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::IWorkScheduler, Mosaic::IWorkScheduler>);
}

TEST_CASE("CVar flag test is HasFlag (Any is the query filter now)", "[facade][cvar]")
{
    CHECK(Arcane::HasFlag(Arcane::CVarFlags::Archive | Arcane::CVarFlags::Hidden, Arcane::CVarFlags::Hidden));
    CHECK_FALSE(Arcane::HasFlag(Arcane::CVarFlags::Archive, Arcane::CVarFlags::Hidden));
}
