#pragma once

// NriGraphContext -- the `--nri-graph` DEV VEHICLE (NRI Phase 2, Task 7).
//
// Everything the graph path needs to put a frame on screen, in one object:
// a window, the wrapper-path native device + its NRI wrap, an NriSwapChain,
// the Task 5 upload ring, the Task 7 pipeline cache, and ONE RenderGraph that
// is Reset/declared/compiled/executed every frame. Tasks 8-12 hang their nodes
// off BuildFrame() below; today the frame is a single Raster node that clears
// the backbuffer and presents.
//
// NOT SCAFFOLDING in the sense NriSmoke is. The smoke is a straight-line proof
// with no reusable abstraction and a scheduled deletion (Task 13); this class
// is the shape Phase 3 grows into -- GpuContext's render internals move ONTO
// it when the hosts flip. What IS temporary is that it stands beside a live
// NVRHI GpuContext rather than replacing it; see THE TWO-DEVICE WINDOW below.
//
// -------------------------------------------------------------------------
// DESK COMMANDS (GPU/windowed runs are desk-only on the dev box, so nothing
// below has ever executed -- D1/D2 are its first real runs):
//
//   ArcaneRuntime --nri-graph --project ..\..\..\ReferenceProject --backend dx12 \
//                 --frames 120 --no-vsync --screenshot nri-graph-dx12.png
//   ArcaneRuntime --nri-graph --project ..\..\..\ReferenceProject --backend dx12 \
//                 --frames 120 --no-vsync --golden-compare <repo>\ReferenceProject\Goldens \
//                 --golden-stage batch
//
// Both boot the REAL engine (project, plugin, scene, material compiles) and
// swap only the render half. Exit codes follow the host's existing contract:
// 0 clean, 1 the graph run failed or the device was lost, 2 RenderErrorCount
// GREW during the run (a validation error fired -- Debug turns the D3D12 debug
// layer and VK sync validation ON for exactly this), 3 a golden/screenshot
// capture or compare failure. Precedence 1 > 2 > 3, same as the smoke's.
// -------------------------------------------------------------------------
//
// THE TWO-DEVICE WINDOW (read before changing where the vehicle renders).
// On this path the process holds TWO graphics devices at once: the engine's
// NVRHI RenderDevice (created by GpuContext during boot, because the scene
// resolver, the batcher and the material compile service are all built on it)
// and this object's NRI device. That has two consequences, and the second one
// is why this class owns a window instead of borrowing the host's:
//
//  1. Vulkan: NriDevice.hpp's "one live Vulkan device per process" rule is
//     BROKEN on this path by construction -- the Vulkan-Hpp default dispatcher
//     (Render/VulkanDispatchStorage.cpp) binds ONE VkDevice, and the creation
//     half re-inits it onto ours (DeviceVulkan.cpp's
//     VULKAN_HPP_DEFAULT_DISPATCHER.init(out.device)), so the NVRHI device's
//     device-level entry points are left resolved through THIS device's
//     dispatch chain. Those pointers coincide only while no layer is active --
//     and Debug, the configuration this vehicle exists to run in, turns
//     validation ON. Create() says so at WARN. dx12 is unaffected (two
//     ID3D12Devices coexist fine). Closing it properly means one device for the
//     whole process, which is the Phase 3 flip.
//
//     IT IS LIVE, NOT LATENT, AND `vulkan + RESIZE` IS THE SHARP EDGE. The
//     NVRHI device keeps doing device-level work every frame on this path (the
//     shader poll and the resolver's Refresh sit OUTSIDE RuntimeApp's render
//     branch), and RuntimeApp still calls GpuContext::OnResize here, which
//     DESTROYS AND RECREATES the hidden NVRHI swapchain's backbuffers on EVERY
//     resize -- the heaviest device-level burst in the frame, aimed at the
//     device whose dispatch chain was rebound. Resize() below closes the
//     graph's OWN half of the resize hazard (descriptors left naming freed
//     backbuffers), which does not fix this but does mean a surviving
//     drag-storm failure has one cause instead of two overlapping ones.
//     Desk order accordingly: dx12 first, on every item.
//  2. DXGI allows only ONE flip-model swap chain per HWND at a time
//     (IDXGIFactory2::CreateSwapChainForHwnd's own remark), and both this
//     swapchain (NRI SwapChainD3D12 -> DXGI_SWAP_EFFECT_FLIP_DISCARD) and the
//     engine's (Render/DeviceD3D12.cpp, same swap effect) are flip-model. So
//     the vehicle CANNOT create its swapchain over the host window that
//     GpuContext already owns -- it would fail at creation. It therefore
//     creates its OWN window and presents there, and RuntimeApp leaves the
//     engine's window hidden for the duration of a --nri-graph run (it is
//     created hidden and only StageFinalize reveals it). One visible window,
//     one live swapchain per HWND, no reordering of the boot.
//
// Consequences of owning the window, stated because they are observable at the
// desk: this window is the one to close/drag/resize (the hidden engine window
// still exists and still holds an idle NVRHI swapchain), and RuntimeApp pumps
// THIS window's events instead of the host's -- so the ImGui event tap the
// ImGuiLayer installed on the host window receives nothing on this path. That
// is fine for Tasks 7-11 (nothing draws ImGui through the graph yet) and is
// Task 12's to wire.
//
// Adapted, where marked, from .example/NRISamples (MIT -- see that tree's
// LICENSE.txt): the readback shape (Source/Readback.cpp's COPY_SOURCE ->
// CmdReadbackTextureToBuffer -> map, and its BGRA-vs-RGBA channel check),
// reached here through NriSmoke.cpp's adaptation of it.
//
// Include order: NRI headers first, ALWAYS (see NriCommon.hpp) --
// Extensions/NRIDeviceCreation.h (via NriDevice.hpp) declares
// nri::Message::ERROR and <windows.h> (via spdlog) #defines ERROR.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Host/HostConfig.hpp>   // HostConfig, GoldenStage
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/NriSwapChain.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace Arcane
{
    class ARCANE_API NriGraphContext
    {
    public:
        // What one RenderFrame() call renders. Grows as Tasks 8-12 add nodes;
        // today only `capture` changes the graph's shape.
        struct FrameDesc
        {
            // The --golden-stage vocabulary, reused verbatim rather than
            // re-invented: `batch` = the batcher + tonemap only, `post` = plus
            // the post chain, `full` = plus the HUD. Tasks 8-12 attach their
            // nodes under the stages that include them; TODAY every stage
            // renders the same clear-only frame, so the flag round-trips
            // without changing pixels.
            GoldenStage stage = GoldenStage::Full;

            // Add the readback node to THIS frame's graph, so the presented
            // backbuffer can be read back afterwards through ReadCapture().
            // Set on the last frame of a --screenshot / --golden-* run, the
            // same timing the NVRHI path uses.
            bool capture = false;
        };

        // What one frame did. Distinguishes the two false-y outcomes the frame
        // driver must treat differently: a SKIPPED frame is routine (a
        // zero-sized surface, a minimized window, an OUT_OF_DATE acquire) and
        // must not advance the frame counter or end a --frames N run, while a
        // FAILED frame has already bumped RenderErrorCount and must stop the
        // run.
        enum class FrameOutcome : std::uint8_t { Presented, Skipped, Failed };

        // Builds window + native device + NRI wrap + swapchain + ring + cache
        // + graph, in that order, honouring `config.backend` and
        // `config.vsync`. Null on any failure (already logged + latched).
        //
        // Debug forces validation ON -- D3D12 debug layer through
        // ID3D12InfoQueue1, VK core + SYNCHRONIZATION validation, and NRI's own
        // validation layer -- all three of which end at RenderErrorCount. That
        // is identical to NriSmoke's wiring and is the dev-loop gate for the
        // whole phase (NRI validation alone cannot catch barrier bugs).
        static std::unique_ptr<NriGraphContext> Create(const HostConfig& config);

        ~NriGraphContext();

        NriGraphContext(const NriGraphContext&)            = delete;
        NriGraphContext& operator=(const NriGraphContext&) = delete;

        // THIS object's window -- the visible one on a --nri-graph run (see
        // THE TWO-DEVICE WINDOW above). The frame driver pumps it and feeds
        // resizes back through Resize().
        [[nodiscard]] Window& Win() noexcept { return m_window; }

        // Destroy + recreate the swapchain at the new size. MUST be called
        // strictly BETWEEN RenderFrame() calls -- never from inside one --
        // which is exactly what a frame driver that pumps events at the top of
        // its loop does. (The graph's acquire/present pair lives entirely
        // inside one Execute(), so that is the whole of the CALLER's
        // obligation; RgExecuteDesc::swapChain.)
        //
        // Not a bare forward to NriSwapChain::Resize: it first idles, releases
        // the graph's GPU resources and DRAINS the graveyard, so that every
        // descriptor naming a current backbuffer is destroyed BEFORE those
        // backbuffers are. RenderGraph turns imported views over per Execute,
        // but it does so by BURYING them -- and a burial reaped after the
        // swapchain is gone destroys a view over a freed image, which is a
        // Vulkan validation error and therefore a nonzero exit on a vehicle
        // whose contract is "the latch did not grow". See the .cpp.
        void Resize(std::uint32_t width, std::uint32_t height);

        // One frame: Reset the graph, declare this frame's nodes, Compile,
        // Execute (which acquires, records, submits and presents).
        FrameOutcome RenderFrame(const FrameDesc& frame);

        // Maps the buffer the last capture frame read the backbuffer into and
        // hands back TIGHT RGBA8 -- swizzled from BGRA when that is what the
        // swapchain resolved to, so the bytes are display-referred RGBA
        // whatever NRI picked (NriSwapChain::Format() is resolved, never
        // pinned). That normalization is what lets the byte-wise golden
        // comparator compare a graph capture against an NVRHI baseline at all.
        //
        // Idles the device first (the copy has to have landed). False, already
        // logged, if no capture frame was recorded or the map failed. Callers
        // MUST NOT treat a false here as a run failure -- it is the same exit-3
        // "capture failed" class the NVRHI path reports.
        [[nodiscard]] bool ReadCapture(std::uint32_t& width, std::uint32_t& height,
                                       std::vector<unsigned char>& rgba);

        // Accessors for the node authors of Tasks 8-12. All references stay
        // valid for this object's lifetime.
        [[nodiscard]] NriDevice&        Device()    noexcept { return *m_device; }
        [[nodiscard]] NriSwapChain&     Swap()      noexcept { return *m_swap; }
        [[nodiscard]] NriUploadRing&    Ring()      noexcept { return m_ring; }
        [[nodiscard]] NriPipelineCache& Pipelines() noexcept { return m_pipelines; }
        [[nodiscard]] RenderGraph&      Graph()     noexcept { return *m_graph; }

        // Frames this vehicle has actually PRESENTED. Skipped frames do not
        // count -- it advances in lockstep with the swapchain's own frame
        // counter, which is what makes `frameIndex % kSwapchainFramesInFlight`
        // a safe command-buffer slot.
        [[nodiscard]] std::uint64_t PresentedFrames() const noexcept { return m_frameIndex; }

    private:
        NriGraphContext() = default;

        bool Init(const HostConfig& config);

        // Declares this frame's nodes into m_graph (already Reset()). The one
        // place Tasks 8-12 add to.
        void BuildFrame(const FrameDesc& frame);

        // Creates the HOST_READBACK staging buffer for the current swapchain
        // extent, or reuses the existing one when the extent is unchanged.
        // False (already logged) on failure -- the frame still renders, it
        // just cannot be captured.
        bool EnsureCaptureBuffer();

        // --- TEARDOWN CONTRACT: declaration order == reverse destruction ---
        // m_graph dies first (it buries its pool/views/command slots into the
        // device's graveyard at its own last submitted fence value), then the
        // cache and the ring, then the swapchain, then the NRI device (whose
        // destructor idles the device and DRAINS that graveyard), then the
        // native device it wrapped, and the window LAST. Do NOT reorder: this
        // is contract item 15 (the NRI device is destroyed BEFORE the native
        // one) plus the graveyard's "everything buried must outlive nothing".
        Window                             m_window;
        std::unique_ptr<NativeDeviceOwner> m_native;
        std::unique_ptr<NriDevice>         m_device;
        std::unique_ptr<NriSwapChain>      m_swap;
        NriUploadRing                      m_ring;
        NriPipelineCache                   m_pipelines;
        std::unique_ptr<RenderGraph>       m_graph;

        // The capture staging buffer, owned here (the graph only IMPORTS it --
        // a transient graph buffer is DEVICE-local and could never be mapped).
        // Buried in ~NriGraphContext at the graph's last submitted fence value.
        nri::Buffer*  m_capture          = nullptr;
        std::uint64_t m_captureRowPitch  = 0;
        std::uint64_t m_captureSlicePitch = 0;
        std::uint32_t m_captureWidth     = 0;
        std::uint32_t m_captureHeight    = 0;
        // Set when a capture node was actually RECORDED into a submitted
        // frame -- ReadCapture refuses without it rather than mapping and
        // writing whatever was already in host-readback memory.
        bool          m_captureRecorded  = false;

        // This frame's handles, minted inside the first node's setup (the
        // builder is the only place they can be minted) and read by the nodes
        // that follow. Reset every frame; a handle from a previous frame fails
        // to decode after Reset() bumps the graph's generation.
        RgTexture m_backbuffer{};
        RgBuffer  m_captureHandle{};

        nri::Format   m_format     = nri::Format::UNKNOWN;
        std::uint64_t m_frameIndex = 0;   // PRESENTED frames; the command-slot clock
        bool          m_vsync      = true;

        // --- the drag-storm heartbeat (D1 shakedown ride-along) -------------
        // An open-ended run (`--nri-graph` with no --frames) prints NOTHING
        // between "ready" and whatever the user's window close produces, so a
        // desk user dragging the window for 30s has no way to tell a healthy
        // vehicle from a wedged one. Armed only on that path -- a --frames N
        // run already ends by itself and says how it went.
        //
        // It ticks from RenderFrame, i.e. from PRESENTED frames only, which is
        // the point: a heartbeat that kept printing while the frame loop was
        // stuck inside a submit would be worse than silence. Its ABSENCE is
        // the wedge signal (and the hang watchdog is what turns a real wedge
        // into a report).
        bool                                  m_heartbeat = false;
        std::uint64_t                         m_errorBaseline = 0;
        std::chrono::steady_clock::time_point m_lastHeartbeat{};
    };
}
