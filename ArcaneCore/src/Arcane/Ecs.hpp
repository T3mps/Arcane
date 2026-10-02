#pragma once

// The Arcane:: ECS facade (input-seam spec 2026-10-02 s6.1). Game code spells
// ONLY Arcane:: names; the standalone libraries (Astra, Mosaic, Manifold2D)
// keep their own namespaces underneath. Bevy's prelude is the model: one
// include, one namespace. Every name is an ALIAS of the library type, so this
// changes no ABI and no serialized type name.
//
//     struct Mover : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem>>
//     {
//         void operator()(Arcane::View<Arcane::Transform>& view, Arcane::Res<Arcane::Time> time);
//     };

#include <Arcane/EcsFwd.hpp>
#include <Arcane/Sim/Time.hpp>

// ARCANE_INTERNAL_BEGIN: the facade's library side
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
}
// ARCANE_INTERNAL_END
