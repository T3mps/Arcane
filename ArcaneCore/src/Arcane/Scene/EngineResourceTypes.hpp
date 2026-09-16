#pragma once

// PrewarmEngineResourceTypes: resolve every Core-owned ENGINE RESOURCE type in
// ArcaneCore.dll, before any other module gets the chance.
//
// Component types have a registrar that fixes their order (Scene/EngineRoster.hpp,
// registered by Runtime's ctor). Resource types have none: each one's id is minted
// by whichever module first calls Astra::TypeID<T>::Value() for it, and a
// hot-reloadable game module is as able to be that module as Core is. Astra
// 056063c (vendored b8291b9) removed the crash that made this dangerous -- a
// TypeContext entry is no longer keyed by a std::type_info* into an unmappable
// image -- so this is belt-and-braces, not a fix: it keeps Core the first
// registrar of engine resource types BY CONSTRUCTION, so a future first-registrar
// hazard cannot originate in a module that can be unloaded.
//
// THE LIST MUST GROW with every new engine resource type. A missing entry is not
// an error today; it just gives the guarantee up for that one type.

#include <Arcane/Core/Api.hpp>

namespace Arcane
{
    ARCANE_CORE_API void PrewarmEngineResourceTypes();
}
