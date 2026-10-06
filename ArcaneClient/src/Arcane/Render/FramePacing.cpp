#include <Arcane/Render/FramePacing.hpp>

#include <Arcane/Render/RenderDeviceSettings.hpp>

#include <algorithm>
#include <atomic>
#include <memory>

namespace Arcane
{
    namespace
    {
        // 0 = not latched yet. Every latched value is >= 2.
        std::atomic<std::uint32_t> g_framesInFlight{ 0 };
    }

    void LatchFramesInFlight() noexcept
    {
        if (g_framesInFlight.load(std::memory_order_acquire) != 0)
            return;
        // The published snapshot, not the registry: wait-free and safe from
        // any thread, so a first read off the main thread can latch too.
        const std::shared_ptr<const RenderSettings> render = SettingsShared<RenderSettings>();
        const std::uint32_t want = std::clamp<std::uint32_t>(render->framesInFlight, 2u, kMaxFramesInFlight);
        std::uint32_t expected = 0;
        g_framesInFlight.compare_exchange_strong(expected, want, std::memory_order_acq_rel);
    }

    std::uint32_t FramesInFlight() noexcept
    {
        std::uint32_t depth = g_framesInFlight.load(std::memory_order_acquire);
        if (depth == 0)
        {
            LatchFramesInFlight();
            depth = g_framesInFlight.load(std::memory_order_acquire);
        }
        return depth;
    }
}
