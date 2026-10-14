#pragma once
#include <Arcane/Core/Api.hpp>
#include <Astra/Core/TypeContext.hpp>

// Module attach (ruling R13). Astra's current-TypeContext slot is per module IMAGE
// by design, so with one DLL per engine module every image must be handed the
// process context or it silently runs on its own default one (different type ids).
// Each module's ARC_MODULE registers an attach callback at image load; every site
// that installs the process context calls AttachAllModules(ctx), which runs all
// callbacks and remembers ctx so a module registering later attaches at once.
namespace Arcane
{
    using ModuleAttachFn = void(*)(Astra::TypeContext*);
    ARC_CORE_API void RegisterModuleAttach(const char* moduleName, ModuleAttachFn fn) noexcept;
    ARC_CORE_API void AttachAllModules(Astra::TypeContext* ctx) noexcept;
}

#define ARC_MODULE_ATTACH(Name)                                                       \
    namespace {                                                                       \
        struct ArcModuleAttachRegistrar_##Name {                                      \
            ArcModuleAttachRegistrar_##Name() {                                       \
                ::Arcane::RegisterModuleAttach(#Name, [](Astra::TypeContext* c) {      \
                    Astra::SetTypeContext(c, Astra::ModuleResidency::Resident);        \
                });                                                                    \
            }                                                                          \
        } arcModuleAttachRegistrar_##Name;                                             \
    }
