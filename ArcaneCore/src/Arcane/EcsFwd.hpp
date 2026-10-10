#pragma once

// The light half of the Arcane::ECS facade (namespace-facades spec 2026-10-10
// s3; was input-seam spec 2026-10-02 s6): forward declarations + the
// non-template aliases for headers that must not pull the full Astra headers
// (Runtime.hpp, PluginABI.hpp, ProcessContext.hpp, SystemFactory.hpp). Game
// code includes <Arcane/Ecs.hpp> instead. Aliases, so the SAME types: no ABI
// or serialization change. IWorkScheduler aliases Mosaic and stays
// Arcane::IWorkScheduler; a Mosaic facade is a later decision.

// ARC_INTERNAL_BEGIN: the facade's library side
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

namespace Arcane::ECS
{
    using Registry          = Astra::Registry;
    using ComponentRegistry = Astra::ComponentRegistry;
    using TypeContext       = Astra::TypeContext;
    using BinaryWriter      = Astra::BinaryWriter;
    using BinaryReader      = Astra::BinaryReader;
    using ComponentModule   = Astra::ComponentModule;
    using SystemScheduler   = Astra::SystemScheduler;
} // namespace Arcane::ECS

namespace Arcane
{
    using IWorkScheduler = Mosaic::IWorkScheduler;
}
// ARC_INTERNAL_END
