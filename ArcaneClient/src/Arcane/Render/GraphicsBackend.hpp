#pragma once

// Render module: WHICH graphics API this process is running on.
//
// It lives in a header of its own, with no graphics-API dependency at all,
// so a consumer that only needs to know "which backend?" -- ShaderPaths (whose
// whole job is picking dxil-or-spirv), the host's HostConfig -- pulls in
// nothing else.
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
