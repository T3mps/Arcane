// Namespace facades (spec 2026-10-10): Arcane::ECS is the only spelling of the
// ECS vocabulary. Each alias is the Astra type. The flat Arcane:: names are gone.
//
// Absence is a compile-time probe. A missing qualified name is not a substitution
// failure in standard C++, so the probe is MSVC's __if_exists (the gate compiler).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Ecs.hpp>

#include <Astra/Astra.hpp>

#include <type_traits>

namespace
{
    struct C {};
    struct S { void operator()(Astra::Registry&) {} };

#if defined(_MSC_VER) && !defined(__clang__)
#define NSFACADE_NAME_PRESENT(qual)                                    \
    []() -> bool {                                                     \
        bool found = false;                                            \
        __if_exists(qual) { found = true; }                            \
        return found;                                                  \
    }()
#endif
}

TEST_CASE("Arcane::ECS aliases are the Astra types", "[namespaces]")
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
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::Result<int, Arcane::ECS::SerializationError>,
                                  Astra::Result<int, Astra::SerializationError>>);
    STATIC_REQUIRE(std::is_same_v<Arcane::ECS::SerializationError, Astra::SerializationError>);
    // Mosaic stays at Arcane:: until its own facade (spec s3).
    STATIC_REQUIRE(std::is_same_v<Arcane::IWorkScheduler, Mosaic::IWorkScheduler>);
}

#if defined(_MSC_VER) && !defined(__clang__)
TEST_CASE("flat Arcane:: ECS prelude names no longer name anything", "[namespaces]")
{
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Entity));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::View));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Not));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::With));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Changed));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Added));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Optional));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Any));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::OneOf));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::IncludeDisabled));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Res));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::ResMut));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Commands));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::SystemTraits));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Before));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::After));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::AmbiguousWith));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Reads));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Writes));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::ReadsResources));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::WritesResources));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Exclusive));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Tick));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Result));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::SerializationError));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::Registry));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::ComponentRegistry));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::TypeContext));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::BinaryWriter));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::BinaryReader));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::ComponentModule));
    CHECK_FALSE(NSFACADE_NAME_PRESENT(Arcane::SystemScheduler));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::IWorkScheduler));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::ECS::Entity));
    CHECK(NSFACADE_NAME_PRESENT(Arcane::ECS::Registry));
}
#endif
