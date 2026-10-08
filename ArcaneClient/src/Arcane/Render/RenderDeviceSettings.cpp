#include <Arcane/Render/RenderDeviceSettings.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Core/Constant.hpp>

#include <atomic>
#include <charconv>
#include <memory>
#include <string>

ARC_SETTINGS(Arcane::RenderSettings);
ARC_SETTINGS(Arcane::RenderDebugSettings);
ARC_SETTINGS(Arcane::RenderD3d12Settings);
ARC_SETTINGS(Arcane::RenderVulkanSettings);

namespace Arcane
{
    namespace
    {
        // The graph format latch. kUnlatched is no enum's ordinal; every
        // latched value is one.
        ARC_CONSTANT("the latch's not-latched-yet sentinel; no CanvasFormat/DepthFormat ordinal")
        constexpr std::uint8_t kUnlatched = 0xFF;
        std::atomic<std::uint8_t> g_canvasFormat{ kUnlatched };
        std::atomic<std::uint8_t> g_depthFormat{ kUnlatched };

        // The first read wins; a racing reader adopts the winner's value.
        // Reads the published snapshot, not the registry: wait-free and safe
        // from any thread (FramePacing.cpp's reasoning).
        template <typename E, typename Field>
        E LatchOnce(std::atomic<std::uint8_t>& slot, Field field) noexcept
        {
            std::uint8_t value = slot.load(std::memory_order_acquire);
            if (value == kUnlatched)
            {
                const std::uint8_t published = static_cast<std::uint8_t>(field(*SettingsShared<RenderSettings>()));
                std::uint8_t expected = kUnlatched;
                value = slot.compare_exchange_strong(expected, published, std::memory_order_acq_rel) ? published : expected;
            }
            return static_cast<E>(value);
        }

        const char* Name(CanvasFormat f) noexcept { return f == CanvasFormat::R11g11b10f ? "R11g11b10f" : "Rgba16f"; }
        const char* Name(DepthFormat f) noexcept { return f == DepthFormat::D24s8 ? "D24s8" : "D32"; }
    }

    CanvasFormat LatchedCanvasFormat() noexcept
    {
        return LatchOnce<CanvasFormat>(g_canvasFormat, [](const RenderSettings& r) { return r.canvasFormat; });
    }

    DepthFormat LatchedDepthFormat() noexcept
    {
        return LatchOnce<DepthFormat>(g_depthFormat, [](const RenderSettings& r) { return r.depthFormat; });
    }

    bool CheckGraphFormatLatch() noexcept
    {
        const std::shared_ptr<const RenderSettings> render = SettingsShared<RenderSettings>();
        bool agrees = true;
        const std::uint8_t canvas = g_canvasFormat.load(std::memory_order_acquire);
        if (canvas != kUnlatched && canvas != static_cast<std::uint8_t>(render->canvasFormat))
        {
            ARC_WARN("[render] render.canvasFormat={} is ignored for this process: the canvas format was already "
                     "latched at {} by a read that ran before the host's config rungs published -- that caller "
                     "runs too early",
                     Name(render->canvasFormat), Name(static_cast<CanvasFormat>(canvas)));
            agrees = false;
        }
        const std::uint8_t depth = g_depthFormat.load(std::memory_order_acquire);
        if (depth != kUnlatched && depth != static_cast<std::uint8_t>(render->depthFormat))
        {
            ARC_WARN("[render] render.depthFormat={} is ignored for this process: the depth format was already "
                     "latched at {} by a read that ran before the host's config rungs published -- that caller "
                     "runs too early",
                     Name(render->depthFormat), Name(static_cast<DepthFormat>(depth)));
            agrees = false;
        }
        return agrees;
    }

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
