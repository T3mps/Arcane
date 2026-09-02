#include "GpuCapability.hpp"

#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>

#include <optional>

namespace Arcane::Test
{
    namespace
    {
        std::optional<bool> g_d3d12;
        std::optional<bool> g_vulkan;

        bool Probe(GraphicsBackend backend)
        {
            // A real creation attempt, because that is the only question that
            // matters -- an adapter that enumerates but cannot create a device
            // is not an available backend. Validation layers stay OFF here: the
            // probe must be cheap and must not fail for a reason unrelated to
            // availability.
            RenderDeviceDesc desc;
            desc.backend = backend;
            // Validation OFF even in Debug, where RenderDeviceDesc defaults it
            // ON: the probe asks ONE question -- can a device be created -- and
            // must not fail for a reason unrelated to availability, nor pay for
            // layers it will immediately throw away.
            desc.enableValidation      = false;
            desc.enableD3D12DebugLayer = false;
            desc.enableSyncValidation  = false;
            auto owner = NativeDeviceOwner::Create(desc);
            return owner != nullptr;
        }
    }

    const char* BackendName(GraphicsBackend backend)
    {
        return ToString(backend);
    }

    bool BackendAvailable(GraphicsBackend backend)
    {
        std::optional<bool>& slot =
            (backend == GraphicsBackend::Vulkan) ? g_vulkan : g_d3d12;
        if (!slot.has_value())
            slot = Probe(backend);
        return *slot;
    }
}
