#include <Arcane/Render/RenderDeviceSettings.hpp>

#include <Arcane/Config/CVarFormat.hpp>

#include <charconv>
#include <string>

ARC_SETTINGS(Arcane::RenderSettings);
ARC_SETTINGS(Arcane::RenderDebugSettings);
ARC_SETTINGS(Arcane::RenderD3d12Settings);
ARC_SETTINGS(Arcane::RenderVulkanSettings);

namespace Arcane
{
    std::array<std::uint8_t, 4> Srgb8Bytes(const CVarColor& color)
    {
        // The hex spelling IS the byte encoding (CVarFormat: sRGB-encoded RGB
        // rounded to 8 bits, linear alpha), so a value typed as "#101010FF"
        // lands as exactly those bytes.
        const std::string hex = CVarColorToHex(color);   // "#RRGGBBAA"
        std::array<std::uint8_t, 4> out{};
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const char* first = hex.data() + 1 + 2 * i;
            (void)std::from_chars(first, first + 2, out[i], 16);
        }
        return out;
    }

    RenderDeviceDesc MakeRenderDeviceDesc()
    {
        const RenderSettings& r = Settings<RenderSettings>();
        const RenderDebugSettings& d = Settings<RenderDebugSettings>();
        RenderDeviceDesc desc;
        desc.backend = r.backend;
        desc.enableValidation = d.validation;
        desc.enableD3D12DebugLayer = d.d3d12DebugLayer;
        desc.enableSyncValidation = d.vkSyncValidation;
        return desc;
    }
}
