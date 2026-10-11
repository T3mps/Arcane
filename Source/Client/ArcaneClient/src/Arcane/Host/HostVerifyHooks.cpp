#include <Arcane/Host/HostVerifyHooks.hpp>

namespace Arcane
{
    namespace { HostVerifyHooks g_hooks; }
    void SetHostVerifyHooks(HostVerifyHooks hooks) noexcept { g_hooks = hooks; }
    HostVerifyHooks GetHostVerifyHooks() noexcept { return g_hooks; }
}
