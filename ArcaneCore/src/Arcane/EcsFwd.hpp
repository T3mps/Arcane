#pragma once

// The light half of the Arcane:: ECS facade (input-seam spec 2026-10-02 s6):
// forward declarations + aliases for headers that must not pull the full
// Astra headers (Runtime.hpp, PluginABI.hpp, ProcessContext.hpp,
// SystemFactory.hpp). Game code includes <Arcane/Ecs.hpp> instead.
// Aliases, so the SAME types: no ABI or serialization change.

// ARCANE_INTERNAL_BEGIN: the facade's library side
namespace Astra
{
    class Registry;
    class ComponentRegistry;
    class TypeContext;
    class BinaryWriter;
    class BinaryReader;
    class ComponentModule;
    class SystemScheduler;
}
namespace Mosaic { struct IWorkScheduler; }

namespace Arcane
{
    using Registry          = Astra::Registry;
    using ComponentRegistry = Astra::ComponentRegistry;
    using TypeContext       = Astra::TypeContext;
    using BinaryWriter      = Astra::BinaryWriter;
    using BinaryReader      = Astra::BinaryReader;
    using ComponentModule   = Astra::ComponentModule;
    using SystemScheduler   = Astra::SystemScheduler;
    using IWorkScheduler    = Mosaic::IWorkScheduler;
}
// ARCANE_INTERNAL_END
