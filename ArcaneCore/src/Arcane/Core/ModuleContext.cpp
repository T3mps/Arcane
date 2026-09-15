#include <Arcane/Core/ModuleContext.hpp>

#include <Astra/Core/TypeContext.hpp>

namespace Arcane::Core
{
    void SetModuleTypeContext(Astra::TypeContext* ctx)
    {
        // Compiled INTO ArcaneCore.dll, so this reaches ArcaneCore.dll's own
        // per-module slot -- the whole point of the export (see the header).
        Astra::SetTypeContext(ctx, Astra::ModuleResidency::Resident);
    }
}
