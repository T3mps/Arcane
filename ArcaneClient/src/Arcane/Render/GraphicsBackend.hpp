#pragma once

// Render module: WHICH graphics API this process is running on.
//
// NRI Phase 5a, Task 8a: the enum used to sit at the top of
// Render/Device.hpp, the NVRHI device interface Task 8b deleted -- so
// every consumer of "which backend?" pulled <nvrhi/nvrhi.h> transitively,
// including Render/ShaderPaths.hpp (whose whole job is picking dxil-or-spirv)
// and the host's own HostConfig. It has no NVRHI dependency of its own and
// never had one, so it moved to a header that has none either.
//
// Deliberately NOT an "RHI backend" enum with a NONE/Null member: the value
// answers "dxil or spirv, D3D12 or Vulkan" for shader paths, golden-image
// directory names, launch arguments and log banners. NriDevice::
// CreateNoneForTests states the other half of that rule -- it reports D3D12
// for a NONE device, because "GraphicsBackend has no NONE value and inventing
// one would leak a test-only concept into every backend switch in the tree"
// (Nri/NriDevice.hpp).

#include <Arcane/Base/Api.hpp>

#include <cstdint>

namespace Arcane
{
    enum class GraphicsBackend : uint8_t
    {
        D3D12,
        Vulkan,
    };

    ARCANE_API const char* ToString(GraphicsBackend backend);
}
