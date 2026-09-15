#pragma once

// Arcane::Core::SetModuleTypeContext -- install the process-shared
// Astra::TypeContext into ARCANECORE.DLL'S OWN per-module slot.
//
// WHY THIS EXISTS (Core-DLL split, spec docs/specs/
// 2026-09-15-core-dll-split-design.md s1/s8): Astra resolves
// GetTypeContext()/SetTypeContext() through a PER-MODULE static
// (Astra/Core/TypeContext.hpp, Detail::CurrentTypeContextSlot -- "per-module
// slot, by design"), and the split gave the engine a THIRD module. Before it,
// every Core-bound TU that touched an Astra::Registry compiled into
// ArcaneClient.dll, whose slot Runtime::Impl's ctor installs; now
// Serialization/ResourceSerialization.cpp (Registry::Save / Get/SetResource
// behind FinishSnapshot, WriteResourceSection, ReadResourceSection) runs
// inside ArcaneCore.dll, whose slot nobody had ever set. Registry.hpp's
// birth-context guard fires on the first such call:
//
//   assertion failed: GetTypeContext() == GetComponentRegistry()->GetBirthContext()
//   -- Registry touched from a module whose TypeContext is not this registry's
//      birth context -- call Astra::SetTypeContext in the calling module first
//
// Exactly the shape of Log::Init's Mosaic-sink install one folder over: a
// per-module inline global that only code compiled INTO this DLL can reach, so
// the installer has to live in a Core .cpp and be exported. An inline helper
// would install into the CALLER's module and fix nothing.
//
// WHO CALLS IT: Runtime::Impl's ctor, immediately after its own
// Astra::SetTypeContext -- the one place the engine's shared context becomes
// known, and the single call covers every consumer. The hosts and the editor
// each build a Runtime at boot; ArcaneTests pins the slot up front with the
// throwaway `Arcane::Runtime pin(&SharedTypeContext())` test_main.cpp already
// constructs for exactly this reason (the slot persists after that Runtime
// dies, so Runtime-less cases are covered too); a plugin reaches Core's
// registry code only through the host's Runtime.
//
// Residency is Resident, not the default Transient, for the same reason
// Runtime passes it: ArcaneCore.dll never unmaps, so its drained meta
// baselines are legitimately PINNED.

#include <Arcane/Core/Api.hpp>

namespace Astra { class TypeContext; }

namespace Arcane::Core
{
    ARCANE_CORE_API void SetModuleTypeContext(Astra::TypeContext* ctx);
}
