#include <Arcane/Render/FramePacing.hpp>

#include <Arcane/Base/Log.hpp>
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

        // The depth the published render.framesInFlight asks for, clamped.
        // The published snapshot, not the registry: wait-free and safe from
        // any thread, so a first read off the main thread can latch too.
        std::uint32_t PublishedFramesInFlight() noexcept
        {
            const std::shared_ptr<const RenderSettings> render = SettingsShared<RenderSettings>();
            return std::clamp<std::uint32_t>(render->framesInFlight, 2u, kMaxFramesInFlight);
        }
    }

    void LatchFramesInFlight() noexcept
    {
        if (g_framesInFlight.load(std::memory_order_acquire) != 0)
            return;
        std::uint32_t expected = 0;
        g_framesInFlight.compare_exchange_strong(expected, PublishedFramesInFlight(), std::memory_order_acq_rel);
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

    bool CheckFramesInFlightLatch() noexcept
    {
        const std::uint32_t latched = g_framesInFlight.load(std::memory_order_acquire);
        if (latched == 0)
            return true;
        const std::uint32_t published = PublishedFramesInFlight();
        if (latched == published)
            return true;
        ARC_WARN("[frame-pacing] render.framesInFlight={} is ignored for this process: the pacing depth "
                 "was already latched at {} by a FramesInFlight() read that ran before the host's config "
                 "rungs published -- that caller runs too early",
                 published, latched);
        return false;
    }
}
