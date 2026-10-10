#pragma once

// The flat ECS vocabulary (flat-gameplay-api spec 2026-10-10 s3, FA9; was
// Arcane::ECS, namespace-facades spec s3; was the input-seam prelude). Game
// code spells ONLY Arcane:: names. One alias per Astra type, in namespace
// Arcane, with no using-directive: a new Astra name reaches game code only
// when an alias is added here. Every name is an ALIAS of the library type.
// There is no Arcane::ECS namespace. File path stays Ecs.hpp.
//
//     struct Mover : Arcane::SystemTraits<Arcane::Before<Arcane::Physics2D::System>>
//     {
//         void operator()(Arcane::View<Arcane::Transform>& view, Arcane::Res<Arcane::Time> time);
//     };

#include <Arcane/EcsFwd.hpp>
#include <Arcane/Sim/Time.hpp>

// ARC_INTERNAL_BEGIN: the facade's library side
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Core/Result.hpp>
#include <Astra/Core/Tick.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Query.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/SerializationError.hpp>
#include <Astra/System/System.hpp>
#include <Astra/System/SystemParam.hpp>
#include <Astra/System/SystemScheduler.hpp>

namespace Arcane
{
    // Entities and queries
    using Entity = Astra::Entity;
    template<typename... C> using View            = Astra::View<C...>;
    template<typename T>    using Not             = Astra::Not<T>;
    template<typename T>    using With            = Astra::With<T>;
    template<typename T>    using Changed         = Astra::Changed<T>;
    template<typename T>    using Added           = Astra::Added<T>;
    template<typename T>    using Optional        = Astra::Optional<T>;
    template<typename... T> using Any             = Astra::Any<T...>;
    template<typename... T> using OneOf           = Astra::OneOf<T...>;
    template<typename T>    using IncludeDisabled = Astra::IncludeDisabled<T>;

    // System parameters
    template<typename T> using Res    = Astra::Res<T>;
    template<typename T> using ResMut = Astra::ResMut<T>;
    using Commands = Astra::Commands;

    // System traits (parameter systems use ordering only; Reads/Writes/
    // ReadsResources/WritesResources/Exclusive are for registry-style systems)
    template<typename... T> using SystemTraits    = Astra::SystemTraits<T...>;
    template<typename... S> using Before          = Astra::Before<S...>;
    template<typename... S> using After           = Astra::After<S...>;
    template<typename... S> using AmbiguousWith   = Astra::AmbiguousWith<S...>;
    template<typename... C> using Reads           = Astra::Reads<C...>;
    template<typename... C> using Writes          = Astra::Writes<C...>;
    template<typename... R> using ReadsResources  = Astra::ReadsResources<R...>;
    template<typename... R> using WritesResources = Astra::WritesResources<R...>;
    using Exclusive = Astra::Exclusive;

    // Misc vocabulary module code meets
    using Tick = Astra::Tick;
    template<typename T, typename E> using Result = Astra::Result<T, E>;
    using SerializationError = Astra::SerializationError;
} // namespace Arcane
// ARC_INTERNAL_END
