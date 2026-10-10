#pragma once

// The Arcane::ECS facade (namespace-facades spec 2026-10-10 s3; was the flat
// prelude, input-seam spec 2026-10-02 s6.1). Game code spells ONLY Arcane::
// names. The ECS vocabulary lives in Arcane::ECS, one alias per Astra type,
// with no using-directive: a new Astra name reaches game code only when an
// alias is added here. Every name is an ALIAS of the library type, so this
// changes no ABI and no serialized type name.
//
//     struct Mover : Arcane::ECS::SystemTraits<Arcane::ECS::Before<Arcane::PhysicsSystem>>
//     {
//         void operator()(Arcane::ECS::View<Arcane::Transform>& view, Arcane::ECS::Res<Arcane::Time> time);
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

namespace Arcane::ECS
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
} // namespace Arcane::ECS
// ARC_INTERNAL_END
