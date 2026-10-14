#pragma once

// The light half of the flat ECS vocabulary (flat-gameplay-api spec 2026-10-10
// s3, FA9; file path stays EcsFwd.hpp): forward declarations + the
// non-template aliases for headers that must not pull the full Astra headers
// (Runtime.hpp, PluginABI.hpp, ProcessContext.hpp, SystemFactory.hpp). Game
// code includes <Arcane/Ecs.hpp> instead. Aliases, so the SAME types: no ABI
// or serialization change. IWorkScheduler aliases Mosaic and stays
// Arcane::IWorkScheduler; a Mosaic facade is a later decision. There is no
// Arcane::ECS namespace.

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

namespace Arcane
{
    using Registry          = Astra::Registry;
    using ComponentRegistry = Astra::ComponentRegistry;
    using TypeContext       = Astra::TypeContext;
    using BinaryWriter      = Astra::BinaryWriter;
    using BinaryReader      = Astra::BinaryReader;
    using ComponentModule   = Astra::ComponentModule;
    using SystemScheduler   = Astra::SystemScheduler;
} // namespace Arcane

namespace Arcane
{
    using IWorkScheduler = Mosaic::IWorkScheduler;
}
// ARC_INTERNAL_END
