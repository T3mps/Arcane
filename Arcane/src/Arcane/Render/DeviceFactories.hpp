#pragma once

// Internal to the Render module: per-backend factory functions implemented
// in DeviceD3D12.cpp / DeviceVulkan.cpp, dispatched by RenderDevice::Create.
// Not part of the engine's public API surface.

#include <Arcane/Render/Device.hpp>

#include <memory>

namespace Arcane
{
    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc);
}
