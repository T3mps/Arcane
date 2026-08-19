#include <Arcane/Render/GraphicsBackend.hpp>

namespace Arcane
{
    const char* ToString(GraphicsBackend backend)
    {
        switch (backend)
        {
            case GraphicsBackend::D3D12:  return "D3D12";
            case GraphicsBackend::Vulkan: return "Vulkan";
        }
        return "Unknown";
    }
}
