#include <Arcane/Render/Nri/RenderOverlayNode.hpp>

namespace Arcane
{
    namespace { RenderOverlayHooks g_hooks; }
    void SetRenderOverlayHooks(RenderOverlayHooks hooks) noexcept { g_hooks = hooks; }
    RenderOverlayHooks GetRenderOverlayHooks() noexcept { return g_hooks; }
}
