#pragma once

// NRI substrate (Phase 4, Task 1): a one-time snapshot of the wrapped
// device's tiers/features, taken at NriDevice::FinishWrap and read back
// through NriDevice::Caps() for the rest of the process's lifetime.
//
// Why this exists: GetDeviceDesc is already called in six places in this
// tree, every one of them for alignment values -- nothing reads
// tiers/features. A later consumer gating on a feature it never checked
// (bindless indexing, ray tracing) becomes a hard crash on the first
// machine without it, arbitrarily far from a snapshot that could have
// caught it at wrap time. This struct is that snapshot: plain data, queried
// once, no live nri::Device reference to keep alive.

#include <cstdint>

namespace Arcane
{
    struct NriDeviceCaps
    {
        std::uint8_t bindlessTier   = 0;   // nri::DeviceDesc::tiers.bindless
        std::uint8_t rayTracingTier = 0;   // tiers.rayTracing
        bool         meshShader     = false;                // features.meshShader
        std::uint32_t maxDescriptorSetTextures = 0;          // descriptorSet.textureMaxNum

        [[nodiscard]] bool SupportsBindless() const noexcept { return bindlessTier > 0; }
    };
}
