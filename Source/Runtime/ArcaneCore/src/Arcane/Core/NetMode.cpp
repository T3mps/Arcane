#include <Arcane/Core/NetMode.hpp>

namespace Arcane
{
    const char* ToString(NetMode mode) noexcept
    {
        switch (mode)
        {
            case NetMode::Standalone:      return "Standalone";
            case NetMode::DedicatedServer: return "DedicatedServer";
            case NetMode::ListenServer:    return "ListenServer";
            case NetMode::Client:          return "Client";
        }
        return "Unknown";
    }
}
