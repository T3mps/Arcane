#pragma once

// NRI substrate (Phase 1, Task 8): NRISwapChain over our SDL window, plus
// kSwapchainFramesInFlight-deep frame pacing on ONE timeline nri::Fence.
//
// Adapted from .example/NRISamples (MIT -- see that tree's LICENSE.txt):
// Source/Triangle.cpp's swap chain setup/acquire/present shape and
// Source/Resize.cpp's destroy+recreate resize idiom. Both samples create the
// per-image acquire/release fences and the frame-pacing fence directly
// through CoreInterface/SwapChainInterface, with no NRIFramework machinery
// in between -- this class follows the same idiom.
//
// -------------------------------------------------------------------------
// Why destroy+recreate on resize, and why on the WINDOW EVENT rather than an
// acquire/present return code (spec ruling; survey finding):
// -------------------------------------------------------------------------
// nri::SwapChainInterface offers exactly CreateSwapChain/DestroySwapChain --
// no resize entry point (no analogue of IDXGISwapChain::ResizeBuffers or an
// in-place vkCreateSwapchainKHR rebuild) -- so destroy+recreate is the only
// path. NRISamples' own Resize sample does the same (ResizeSwapChain()).
//
// It is also the ONLY complete signal, because NRI does not surface a
// reliable "please resize" return code:
//   - VK: ConversionVK.h's GetResultFromVkResult treats any vkResult >= 0 as
//     Result::SUCCESS. VK_SUBOPTIMAL_KHR (1000001003) is a positive/success
//     code per the Vulkan spec, so it is silently folded into SUCCESS by
//     both SwapChainVK::AcquireNextTexture and ::Present (NRI_RETURN_ON_BAD_
//     VKRESULT only branches on `vkResult < 0` -- SharedExternal.h). Only
//     VK_ERROR_OUT_OF_DATE_KHR (negative) becomes Result::OUT_OF_DATE.
//   - D3D12: AcquireNextTexture/Present never produce Result::OUT_OF_DATE at
//     all -- SwapChainD3D12 only checks GetDeviceRemovedReason(); a stale
//     buffer size is not a D3D12 present-time error, it just presents wrong
//     until ResizeBuffers runs (which we never call -- we destroy+recreate
//     instead).
// An OUT_OF_DATE-only auto-recreate would therefore "fix" the hard VK error
// but never the soft one, silently continuing to present into a suboptimal
// surface until a genuinely fatal acquire forced a recreate. The window's
// resize EVENT (driven externally into Resize(), the same shape as
// GpuContext::OnResize for the NVRHI swapchains) is complete and covers both
// backends uniformly, so it is the only trigger this class listens to.
// AcquireNextTexture()/Present() still handle a stray OUT_OF_DATE (the one
// VK CAN report) as an ordinary, non-latching skip -- see the .cpp.
//
// -------------------------------------------------------------------------
// Frame pacing vs GpuFrameSlot (Render/GpuInstrumentation.hpp)
// -------------------------------------------------------------------------
// Same shape as the NVRHI swapchains' GpuFrameSlot pacing, replumbed onto
// NRI's fence model: ONE timeline nri::Fence, kSwapchainFramesInFlight slots
// (Render/Swapchain.hpp -- reused, not reinvented), signalled with an
// increasing value at the tail of Present() and waited on at the top of
// AcquireNextTexture() once frameCounter >= kSwapchainFramesInFlight (so the
// first kSwapchainFramesInFlight frames never wait at all -- no call, not
// even a trivially-true one).
//
// The wait is NOT a bare `Core().Wait(fence, value)`. Both backends'
// nri::Fence::Wait blocks internally with no poll/callback hook and no
// heartbeat: FenceVK::Wait (ThirdParty/NRI/Source/VK/FenceVK.hpp) calls
// vkWaitSemaphores with a fixed NRI_TIMEOUT_FENCE (5s, SharedExternal.h) and
// discards its VkResult; FenceD3D12::Wait either busy-waits or blocks on
// WaitForSingleObjectEx for the same 5s. Calling either directly from the
// pacing wait would park the render thread for up to 5s publishing nothing,
// which is exactly the trap GpuFrameSlot's header comment describes for
// nvrhi's waitEventQuery -- a hung GPU must not look like a hung process.
// So the wait here is our own poll loop -- GetFenceValue(fence) >= value,
// SDL_DelayNS(1ms), Diagnostics::Heartbeat() + Diagnostics::
// GpuHeartbeatRefresh() every iteration, same 15s window as GpuInstrumentation
// .cpp's PollingWaitForStampedQuery -- falling back to the blocking
// Core().Wait() only once that window has elapsed (see the .cpp).
//
// -------------------------------------------------------------------------
// What this exposes, and what it deliberately does not
// -------------------------------------------------------------------------
// Phase 2's frame graph presents THROUGH this class -- it is not scaffolding
// and carries no deletion comment. It exposes exactly what Task 9's smoke and
// Phase 2 need: the acquired nri::Texture*, its acquire/release fences (so
// the caller's OWN command-buffer submission can wait/signal them -- see
// AcquireNextTexture()), the resolved format, and the texture count. It does
// NOT create texture views/descriptors (NRISamples' CreateTextureView calls
// are deliberately not replicated here) -- that is frame-graph machinery and
// belongs to Phase 2, not the swapchain wrapper.

#include <NRI.h>
#include <Extensions/NRISwapChain.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Swapchain.hpp>   // kSwapchainFramesInFlight

#include <cstdint>
#include <memory>
#include <vector>

namespace Arcane
{
    class Window;

    class ARCANE_API NriSwapChain
    {
    public:
        // `device` and `window` must outlive this object (same contract as
        // the NVRHI swapchains -- Device.hpp's CreateSwapchain comment).
        // Window must have been created with a valid native handle (HWND on
        // Windows -- Window::NativeHandle()). Returns null on failure, reason
        // already logged.
        static std::unique_ptr<NriSwapChain> Create(NriDevice& device, Window& window, bool vsync);

        ~NriSwapChain();

        NriSwapChain(const NriSwapChain&)            = delete;
        NriSwapChain& operator=(const NriSwapChain&) = delete;

        // Frame-pacing wait (heartbeat-polling; see the header block above),
        // then NRI's AcquireNextTexture. Returns null when the frame must be
        // skipped -- a zero-sized surface (minimized, or not yet sized by a
        // first Resize()), or an acquire failure (OUT_OF_DATE is logged at
        // WARN and does NOT bump the RenderErrorCount latch -- it is routine,
        // see the .cpp; every other result does, via ARC_NRI_CHECK).
        //
        // On success, CurrentAcquireFence()/CurrentReleaseFence() identify
        // the fences the CALLER's own command-buffer submission must wait on
        // (before writing the returned texture) and signal (before calling
        // Present()) -- this class owns no command buffers and never submits
        // real work itself (Phase 2's frame graph does), so it cannot wire
        // those fences into a submission on the caller's behalf.
        [[nodiscard]] nri::Texture* AcquireNextTexture();

        [[nodiscard]] nri::Fence* CurrentAcquireFence() const { return m_acquired ? m_currentAcquireFence : nullptr; }
        [[nodiscard]] nri::Fence* CurrentReleaseFence() const;

        // Presents the texture AcquireNextTexture() returned, using its
        // release fence (which the caller's submission must already have
        // signalled), then stamps this frame's pacing-fence completion point
        // via a small QueueSubmit carrying only that signal -- "signal at
        // submit", mirroring NRISamples' trailing signal-only submit
        // ("Signaling after Present improves D3D11 performance a bit") and
        // SwapchainVulkan::Present's own empty-submit semaphore flush
        // (DeviceVulkan.cpp). No-op if nothing is currently acquired.
        void Present();

        // Destroy + recreate at the given size -- see the header block above
        // for why this is the only resize path and why it must be driven by
        // the window's resize EVENT, not an acquire/present return code. A
        // no-op if the size is unchanged. The pacing fence and frame counter
        // survive this call (matching NRISamples' ResizeSwapChain(), which
        // leaves m_FrameFence untouched) -- only the swapchain, its textures,
        // and their per-image fences are destroyed and rebuilt.
        //
        // CALLER CONTRACT: must NOT be called between a successful
        // AcquireNextTexture() and its matching Present(). Destroying the
        // swapchain frees the nri::Texture* AcquireNextTexture() already
        // handed out -- with no API signal, so calling this mid-acquire
        // leaves the caller holding a silently dangling pointer. There is no
        // cheap structurally-safe alternative (deferring the free until a
        // Present() that a resize may have already made impossible is frame-
        // graph machinery, not this class's job) -- the reference NVRHI
        // Vulkan swapchain has the identical gap (SwapchainVulkan::Resize,
        // DeviceVulkan.cpp, also unconditional). Task 9's frame loop (and
        // every later caller) MUST sequence Resize() -- driven by the
        // window's resize event -- at frame boundaries only, never between
        // Acquire and Present. Debug builds ARC_ASSERT this; release builds
        // ARC_WARN loudly and proceed (see the .cpp) -- there is no safe
        // recovery once the caller already violated the contract.
        void Resize(uint32_t width, uint32_t height);

        // The format actually resolved by NRI (queried via GetTextureDesc on
        // the first swapchain texture, same as the samples) -- NOT assumed.
        // NRI's SwapChainFormat is an abstract BT709/BT2020 classification,
        // not a channel-order pin: D3D12's own concrete mapping table
        // hardcodes BT709_G22_8BIT -> DXGI_FORMAT_R8G8B8A8_UNORM (RGBA, not
        // NVRHI's BGRA -- SwapChainD3D12.hpp), and VK's format-priority sort
        // also prefers R8G8B8A8_UNORM over B8G8R8A8_UNORM when both are
        // available (SwapChainVK.hpp). Callers that need to compare against
        // NVRHI's BGRA8_UNORM must read this, not assume it.
        [[nodiscard]] nri::Format Format() const { return m_format; }
        [[nodiscard]] uint32_t Width() const { return m_width; }
        [[nodiscard]] uint32_t Height() const { return m_height; }
        [[nodiscard]] uint32_t TextureCount() const { return (uint32_t)m_textures.size(); }

    private:
        NriSwapChain() = default;

        bool Init(NriDevice& device, Window& window, bool vsync);
        bool CreateSwapChainObjects();
        void ReleaseSwapChainObjects();

        NriDevice* m_device = nullptr;
        Window*    m_window = nullptr;

        nri::SwapChainInterface m_swapChainInterface{};
        nri::SwapChain*         m_swapChain = nullptr;

        struct TextureSlot
        {
            nri::Texture* texture      = nullptr;  // borrowed: NRI's SwapChain owns and frees these in DestroySwapChain
            nri::Fence*   acquireFence = nullptr;   // SWAPCHAIN_SEMAPHORE fence, owned here
            nri::Fence*   releaseFence = nullptr;   // SWAPCHAIN_SEMAPHORE fence, owned here
        };
        std::vector<TextureSlot> m_textures;

        // The pacing fence: ONE timeline fence, kSwapchainFramesInFlight
        // slots deep. Survives Resize() (see Resize()'s comment).
        nri::Fence* m_frameFence = nullptr;

        nri::Format m_format = nri::Format::UNKNOWN;
        uint32_t    m_width  = 0;
        uint32_t    m_height = 0;
        bool        m_vsync  = true;

        uint64_t      m_frameCounter        = 0;         // frames ACQUIRED so far (0-based)
        uint32_t      m_currentTextureIndex = 0;
        nri::Fence*   m_currentAcquireFence  = nullptr;   // the recycled-slot acquire fence THIS frame used
        bool          m_acquired            = false;
    };
}
