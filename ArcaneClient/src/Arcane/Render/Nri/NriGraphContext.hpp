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
// NOT SCAFFOLDING in the sense the Phase-1 triangle smoke was. That was a
// straight-line proof with no reusable abstraction, deleted at Task 13; this
// class is the shape Phase 3 grows into -- GpuContext's render internals move
// ONTO it when the hosts flip. What IS temporary is that it stands beside a
// live NVRHI GpuContext rather than replacing it; see THE TWO-DEVICE WINDOW
// below.
//
// -------------------------------------------------------------------------
// DESK COMMANDS (GPU/windowed runs are desk-only on the dev box, so nothing
// below has ever executed -- D1/D2 are its first real runs):
//
//   ArcaneRuntime --nri-graph --project ..\..\..\ReferenceProject --backend dx12 \
//                 --frames 120 --no-vsync --screenshot nri-graph-dx12.png
//   ArcaneRuntime --nri-graph --project ..\..\..\ReferenceProject --backend vulkan \
//                 --frames 120 --no-vsync --screenshot nri-graph-vulkan.png
//   ArcaneRuntime --nri-graph --project ..\..\..\ReferenceProject --backend dx12 \
//                 --frames 120 --no-vsync --golden-compare <repo>\ReferenceProject\Goldens \
//                 --golden-stage batch
//
// The first two are the direct --nri-graph equivalents of the two commands
// Phase 1's triangle smoke carried on its own header, one per backend --
// migrated here at Task 13, which deleted that file. UNLIKE the smoke, which
// owned the whole process and was therefore always the FIRST (and only)
// D3D12/VK device the process created, --nri-graph boots the real engine
// FIRST (GpuContext's NVRHI device), so its own device is always the SECOND.
// That has one dx12 consequence worth stating here rather than leaving a
// desk user to rediscover it: the D3D12 CPU debug layer / ID3D12InfoQueue1
// channel the smoke used to exercise as Task 1's validation proof is NOT
// available to the vehicle's device on an ordinary run -- DeviceD3D12.cpp
// declines EnableDebugLayer once a device already exists in the process
// (see "THE `--nri-graph` CASE" there) rather than risk removing the
// engine's live one. NRI's own validation layer and, on Vulkan, the VK core
// + SYNCHRONIZATION validation layers stay live and per-device regardless --
// they are what makes a vehicle run's exit code mean something today -- but
// closing the InfoQueue1 gap for real means the FIRST device in the process
// would have to request the debug layer, which is a boot-sequencing change
// nobody has made (DeviceD3D12.cpp names the tradeoff).
//
// Both boot the REAL engine (project, plugin, scene, material compiles) and
// swap only the render half. Exit codes follow the host's existing contract:
// 0 clean, 1 the graph run failed or the device was lost, 2 RenderErrorCount
// GREW during the run (a validation error fired -- Debug requests the D3D12
// debug layer and turns VK sync validation ON for exactly this, subject to
// the dx12 caveat above), 3 a golden/screenshot capture or compare failure.
// Precedence 1 > 2 > 3.
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
// ImGuiLayer installed on the host window receives nothing on this path.
//
// THE HUD IS THEREFORE DRAWN BUT NOT INTERACTIVE on a --nri-graph run, and
// Task 12 left it that way ON PURPOSE rather than re-pointing the tap. Two
// reasons, and the second is the load-bearing one:
//   1. ImGui's platform backend is bound to the HOST window
//      (ImGui_ImplSDL3_InitForOther), so events carrying THIS window's id
//      would be matched against the wrong window -- mouse coordinates would be
//      wrong rather than absent.
//   2. An interactive HUD can be DRAGGED, and window placement persists per
//      exe dir in imgui.ini. The vehicle and the NVRHI runtime share that
//      file, so a drag on the vehicle would silently move the HUD on the
//      NVRHI path too -- i.e. it would change the very baseline D2 compares
//      against. A HUD that renders identically and cannot be moved is exactly
//      what a golden comparison wants.
// Phase 3 retires the whole question: one window, one device, one tap.
//
// The HUD's DISPLAY SIZE has the same root cause and is worth knowing at the
// desk: ImGui_ImplSDL3_NewFrame reads the HOST window's size, so RuntimeApp
// keeps that window's size in lockstep with this one on resize (it already
// does the same for the host canvas). Both default to 1280x720, which is the
// size every golden was captured at.
//
// Adapted, where marked, from .example/NRISamples (MIT -- see that tree's
// LICENSE.txt): the readback shape (Source/Readback.cpp's COPY_SOURCE ->
// CmdReadbackTextureToBuffer -> map, and its BGRA-vs-RGBA channel check),
// originally adapted by the Phase-1 triangle smoke (deleted, Task 13) and
// carried forward here.
//
// Include order: NRI headers first, ALWAYS (see NriCommon.hpp) --
// Extensions/NRIDeviceCreation.h (via NriDevice.hpp) declares
// nri::Message::ERROR and <windows.h> (via spdlog) #defines ERROR.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>          // HostConfig, GoldenStage
#include <Arcane/Material/GlobalParams.hpp>    // GlobalParams (held by value)
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/NriSwapChain.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
// The node types are held BY VALUE-OWNING unique_ptr below, and this class is
// dllexported -- so every TU that sees this header must see complete node
// types (MSVC instantiates the exported class's implicit members). Including
// them here rather than forward-declaring is also the honest dependency: the
// vehicle OWNS its nodes. The include is acyclic -- the node headers only
// forward-declare NriGraphContext.
#include <Arcane/Render/Nri/nodes/Batch2DNode.hpp>
#include <Arcane/Render/Nri/nodes/FullscreenNodes.hpp>
#include <Arcane/Render/Nri/nodes/ImGuiNriNode.hpp>
#include <Arcane/Render/Nri/nodes/PickOutlineNodes.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace Arcane
{
    class Batcher2D;
    struct PostChainDesc;

    // THE LINEAR CANVAS FORMAT every node on this path renders into: the batch
    // node's canvas, and every post-chain pass's target. RGBA16F, matching
    // Canvas.cpp's kCanvasFormat (nvrhi::Format::RGBA16_FLOAT) -- colours are
    // LINEAR and may exceed 1.0, and the tonemap node is what turns them
    // display-referred. It lives HERE, next to the frame's shape, because two
    // nodes now have to agree on it: a post target that did not match the
    // canvas would be a pipeline built for one format bound to an attachment
    // of another.
    inline constexpr nri::Format kGraphCanvasFormat = nri::Format::RGBA16_SFLOAT;

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
            // nodes under the stages that include them.
            //
            // AS OF TASK 10 `batch` genuinely differs: it drops the post chain
            // (the same bypass RuntimeApp applies to the NVRHI path, so a batch
            // golden compares the same content on both). AS OF TASK 12 `full`
            // differs too: it is the only stage that draws the HUD, which is
            // again the same gate RuntimeApp applies to the NVRHI path (both
            // non-Full stages end the ImGui frame without rendering it,
            // because host chrome would sit on top of every stage golden and
            // mask exactly the pixels a node-by-node cutover compares). All
            // three stages now mean what they say.
            GoldenStage stage = GoldenStage::Full;

            // Add the readback node to THIS frame's graph, so the presented
            // backbuffer can be read back afterwards through ReadCapture().
            // Set on the last frame of a --screenshot / --golden-* run, the
            // same timing the NVRHI path uses.
            bool capture = false;

            // This frame's 2D content, ALREADY SUBMITTED into the batcher by
            // the frame driver (Runtime::SetRenderContext + SubmitRender) and
            // not yet End()ed -- the graph path drains it instead. Borrowed for
            // the duration of the RenderFrame call and never stored; null
            // renders a cleared canvas.
            Batcher2D* batch = nullptr;

            // This frame's scene POST CHAIN as bytecode + layout + values --
            // SceneRenderResolver::PostDesc(), the same bind its NVRHI twin
            // renders through. Borrowed for the duration of the RenderFrame
            // call (the resolver is not Refreshed inside one) and never
            // stored; null means "no chain", i.e. canvas -> tonemap.
            const PostChainDesc* post = nullptr;

            // The engine-global material constants for THIS frame
            // (SceneRenderResolver::Globals()). Copied, not borrowed -- the
            // post chain's b1 is written at RECORD time, long after this
            // pointer's referent is out of scope. Null leaves them zeroed.
            const GlobalParams* globals = nullptr;

            // ---- the pick + outline chain (Task 11) ---------------------
            // THIS FRAME'S PICKABLE SILHOUETTES, already projected to canvas
            // pixels by CollectPickables (Render/PickEmit.hpp) -- the k-th
            // entry IS hit-proxy id k+1. Collected by the FRAME DRIVER, which
            // is the thing that owns a registry and a camera; this class must
            // not grow either (it is a RENDER vehicle, the same reasoning
            // AssetResolveFn carries). Borrowed for the duration of the
            // RenderFrame call and never stored.
            //
            // EMPTY IS LEGAL and is not "no pick": the chain is still declared
            // when `pickOutline` is set, and a scene with nothing pickable
            // simply produces an all-zero id target -- which is exactly what
            // a probe over empty space must read back.
            std::span<const PickDrawable> pickables;

            // The hit-proxy ids the outline traces as ONE silhouette (their
            // union). Task 11's driver scripts this to the scene's first
            // pickable entity; Phase 3 replaces THIS ONE LINE at the host with
            // the editor's real selection and nothing here changes.
            std::span<const std::uint32_t> selectedIds;

            // Add the pick node, the readback and the JFA outline chain to
            // this frame's graph. Off on every ordinary run, which is what
            // keeps the frame's shape -- and therefore the batch/post/full
            // stage goldens -- byte-for-byte Task 10's.
            bool pickOutline = false;

            // ---- the HUD (Task 12) --------------------------------------
            // THIS FRAME'S ImGui DRAW DATA, already built by the frame driver
            // (ImGui::Render() -> ImGui::GetDrawData()) before RenderFrame was
            // called. Null means "no HUD this frame", which is what the two
            // non-Full golden stages pass and what makes the frame's shape
            // bit-identical to Task 11's on those runs.
            //
            // BORROWED, and unlike `pickables`/`selectedIds` it is NOT fully
            // consumed at declaration time: the vertex/index copy happens at
            // RECORD time, inside the node's exec fn (the ring's BeginFrame
            // runs after the frame is declared, so an earlier allocation would
            // land in the previous frame's slot). ImGui's own buffers stay
            // valid until the next ImGui::NewFrame, which is a whole frame
            // away -- so this pointer is live for the whole RenderFrame call
            // and nothing retains it past one.
            ImDrawData* imgui = nullptr;
        };

        // What one frame did. Distinguishes the two false-y outcomes the frame
        // driver must treat differently: a SKIPPED frame is routine (a
        // zero-sized surface, a minimized window, an OUT_OF_DATE acquire) and
        // must not advance the frame counter or end a --frames N run, while a
        // FAILED frame has already bumped RenderErrorCount and must stop the
        // run.
        enum class FrameOutcome : std::uint8_t { Presented, Skipped, Failed };

        // Guid -> asset file, the SAME resolution the engine's caches use
        // (Project::ResolveAsset behind Runtime::CurrentProject; see
        // SceneRenderResolver's constructor, which builds the identical
        // lambda). Injected rather than derived here because this class owns no
        // Runtime and must not grow one -- it is a RENDER vehicle.
        using AssetResolveFn = std::function<std::optional<std::filesystem::path>(const Guid&)>;

        // THE SAME SEAM, EXTENDED TO PIXELS (NRI Phase 3, Task 2). Guid ->
        // decoded RGBA8, i.e. `Assets::PixelsFor` -- the engine's retained,
        // DEVICE-FREE decode cache (Task 1). Injected for exactly the reason
        // AssetResolveFn is: this class owns no Runtime and no Assets facade
        // and must not grow either. It feeds NriTextureCache, which is what
        // turns a drained span's texture Guid into something t0 can bind.
        using PixelSupplyFn = NriTextureCache::PixelSupplyFn;

        // Builds window + native device + NRI wrap + swapchain + ring + cache
        // + graph, in that order, honouring `config.backend` and
        // `config.vsync`. Null on any failure (already logged + latched).
        //
        // Debug REQUESTS validation ON -- D3D12 debug layer through
        // ID3D12InfoQueue1, VK core + SYNCHRONIZATION validation, and NRI's own
        // validation layer -- all three of which end at RenderErrorCount. That
        // is the dev-loop gate for the whole phase (NRI validation alone
        // cannot catch barrier bugs); see the DESK COMMANDS block above for
        // the dx12 caveat (the D3D12 debug layer request is declined once the
        // engine's own device already exists in-process, which is always).
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

        // The node objects, reached from the free AddXxxNode() declarators (and
        // therefore from their exec fns). Null only if Create() failed, which
        // never returns a vehicle.
        [[nodiscard]] Batch2DNode*   Batch2D()   noexcept { return m_batch2D.get(); }
        [[nodiscard]] TonemapNode*   Tonemap()   noexcept { return m_tonemap.get(); }
        [[nodiscard]] PostChainNode* PostChain() noexcept { return m_post.get(); }

        // The pick + outline pair (Task 11). NULL on every run that did not
        // pass --pick-probe: they are built only when the config arms the
        // probe, so an ordinary --nri-graph run creates no readback buffer, no
        // descriptor pool and no extra pipeline layout -- and the frame that
        // never declares them cannot be affected by their absence.
        [[nodiscard]] PickNode*    Pick()    noexcept { return m_pick.get(); }
        [[nodiscard]] OutlineNode* Outline() noexcept { return m_outline.get(); }

        // The HUD node (Task 12). Built on every run -- unlike the pick pair
        // it costs one sampler, one layout and one small descriptor pool, and
        // the ONE thing that decides whether the frame draws it is whether the
        // driver handed this frame draw data. Named ImGuiHud() rather than
        // ImGui() because `ImGui` is a namespace this header's consumers use.
        [[nodiscard]] ImGuiNriNode* ImGuiHud() noexcept { return m_imguiHud.get(); }

        // FrameDesc::imgui for the frame currently being declared AND
        // recorded. Unlike CurrentPickables()/CurrentSelectedIds(), which are
        // cleared the moment the declarations are done, this stays readable
        // for the whole RenderFrame call because the node's exec fn is what
        // copies the geometry -- see FrameDesc::imgui. NOT cleared after the
        // call returns (unlike its two siblings above): it holds whatever the
        // last RenderFrame published -- possibly null, if that frame supplied
        // no HUD -- until the next RenderFrame re-publishes it.
        [[nodiscard]] ImDrawData* CurrentImGuiDrawData() const noexcept { return m_currentImGui; }

        // FrameDesc::pickables / ::selectedIds for the frame currently being
        // declared -- read by the pick and outline declarators the same way
        // CurrentBatch() is. Empty outside a RenderFrame() call.
        [[nodiscard]] std::span<const PickDrawable> CurrentPickables() const noexcept
        { return m_currentPickables; }
        [[nodiscard]] std::span<const std::uint32_t> CurrentSelectedIds() const noexcept
        { return m_currentSelectedIds; }

        // The --pick-probe pixel, in canvas px (y down). Doubles as the outline
        // shader's HOVER cursor, deliberately: a desk eyeball then shows amber
        // on the scripted selection and cyan on whatever the probe is over, so
        // one run proves both halves of the composite.
        [[nodiscard]] std::int32_t ProbeX() const noexcept { return m_probeX; }
        [[nodiscard]] std::int32_t ProbeY() const noexcept { return m_probeY; }

        // The entity id read back at the probe pixel, or nullopt when no
        // readback has landed yet (the first kSwapchainFramesInFlight frames of
        // a probe run) or the probe pixel was outside the surface. 0 is a
        // legitimate value and means BACKGROUND -- the caller decides that is a
        // miss, not this.
        [[nodiscard]] std::optional<std::uint32_t> ProbeId() const noexcept;

        // The batcher the frame CURRENTLY being declared should drain, i.e.
        // FrameDesc::batch. Valid only inside a RenderFrame() call; null
        // outside one and on a frame the driver submitted nothing for.
        [[nodiscard]] Batcher2D* CurrentBatch() noexcept { return m_currentBatch; }

        // FrameDesc::globals for the frame currently being declared, COPIED so
        // it stays readable for the whole RenderFrame call (the post chain's
        // b1 is written at record time). All-zero on a frame the driver
        // supplied none for -- which is what a material reads when nobody sets
        // them on the NVRHI path either.
        [[nodiscard]] const GlobalParams& CurrentGlobals() const noexcept { return m_currentGlobals; }

        // The raw bytecode of the offline shader artifact `name` -- the SAME
        // `<name>.bin` the NVRHI path loads through ShaderLibrary, from the same
        // directory (ShaderLibrary::ResolveFlavorDir, so ARCANE_SHADER_DIR moves
        // both paths together). Loaded once and cached; the returned bytes live
        // as long as this vehicle, which is what NriPipelineCache's fill
        // contract needs (CreateGraphicsPipeline dereferences the blob after the
        // fill callback returns). Empty span when the artifact is missing or
        // unreadable -- a failed lookup is NOT cached (only a successful load
        // is), so ARC_ERROR logs on EVERY call that misses, not just the
        // first.
        [[nodiscard]] std::span<const std::uint8_t> ShaderBytecode(const char* name);

        // Called by the capture node's exec fn once the readback copy is
        // actually recorded -- ReadCapture refuses without it rather than
        // mapping whatever was already in host-readback memory. Public because
        // the frame's declarations live in a free function (DeclareGraphFrame),
        // not in a member.
        void NoteCaptureRecorded() noexcept { m_captureRecorded = true; }

        // The capture staging buffer's layout, read by that node's exec fn for
        // the same reason.
        [[nodiscard]] std::uint64_t CaptureRowPitch() const noexcept { return m_captureRowPitch; }
        [[nodiscard]] std::uint64_t CaptureSlicePitch() const noexcept { return m_captureSlicePitch; }
        [[nodiscard]] std::uint32_t CaptureWidth() const noexcept { return m_captureWidth; }
        [[nodiscard]] std::uint32_t CaptureHeight() const noexcept { return m_captureHeight; }

        // Frames this vehicle has actually PRESENTED. Skipped frames do not
        // count -- it advances in lockstep with the swapchain's own frame
        // counter, which is what makes `frameIndex % kSwapchainFramesInFlight`
        // a safe command-buffer slot.
        [[nodiscard]] std::uint64_t PresentedFrames() const noexcept { return m_frameIndex; }

        // THE slot the frame currently being declared/recorded owns -- the
        // exact value RenderFrame passes to ring.BeginFrame and
        // RgExecuteDesc::frameSlot, exposed so a node that keeps its OWN
        // per-frame-slot storage (Batch2DNode's constant-buffer arena) indexes
        // it with the same number the ring does instead of re-deriving it.
        // Stable for the whole of one RenderFrame call.
        [[nodiscard]] std::uint32_t FrameSlot() const noexcept
        {
            return (std::uint32_t)(m_frameIndex % kSwapchainFramesInFlight);
        }

        // Installed once by the frame driver, right after Create(). Without it
        // every asset Guid resolves to nothing -- a material's declared
        // textures then fall back to the white texel, loudly and once
        // (Batch2DNode). Copied, not borrowed.
        void SetAssetResolver(AssetResolveFn resolver) { m_resolveAsset = std::move(resolver); }
        [[nodiscard]] std::optional<std::filesystem::path> ResolveAsset(const Guid& id) const
        {
            return (m_resolveAsset && id.IsValid()) ? m_resolveAsset(id) : std::nullopt;
        }

        // Installed once by the frame driver beside SetAssetResolver. Without
        // it every image misses (loudly, once) and every texture slot on this
        // path binds its node's white texel.
        void SetPixelSupply(PixelSupplyFn supply)
        {
            if (m_textures)
                m_textures->SetPixelSupply(std::move(supply));
        }

        // THE SHARED image residency cache -- one per vehicle, consumed by
        // BOTH the batch node (a span's own t0 texture and a registered
        // material's declared params) and the post chain (its declared
        // params), so an image named by two of them is uploaded ONCE. Null
        // only if Create() failed, which never returns a vehicle.
        [[nodiscard]] NriTextureCache* Textures() noexcept { return m_textures.get(); }

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
        // BEFORE the graph and the nodes in declaration order, so it is
        // destroyed AFTER them: a node holds descriptor SETS naming this
        // cache's views, and its Release must run before the views do. It
        // holds nothing of the graph's (its textures are persistent and are
        // not pool tenants), so it is deliberately NOT released on Resize.
        std::unique_ptr<NriTextureCache>   m_textures;
        std::unique_ptr<RenderGraph>       m_graph;
        // The nodes, after everything they borrow (device, cache) and after the
        // graph whose transient pool the tonemap's source view names. Their
        // objects are RELEASED explicitly in ~NriGraphContext, before the drain
        // -- these destructors are the safety net, not the path.
        std::unique_ptr<Batch2DNode>       m_batch2D;
        std::unique_ptr<PostChainNode>     m_post;
        std::unique_ptr<TonemapNode>       m_tonemap;
        // Built only under --pick-probe (see Pick()/Outline()). The outline
        // node holds views over the graph's transient pool, so it is released
        // in ~NriGraphContext beside the other two and BEFORE the graph
        // releases that pool.
        std::unique_ptr<PickNode>          m_pick;
        std::unique_ptr<OutlineNode>       m_outline;
        // The HUD node. It caches no view over the graph's transient pool (see
        // ImGuiNriNode.hpp), but it is released in ~NriGraphContext beside the
        // others anyway -- one teardown order for every node is cheaper to
        // keep correct than a per-node exception.
        std::unique_ptr<ImGuiNriNode>      m_imguiHud;

        // Raw offline shader artifacts, keyed by artifact stem. unordered_map
        // and not vector: the node authors hold SPANS into these vectors for
        // their whole lifetime, and a node-based container never relocates a
        // value (NriPipelineCache's fill contract, rule 2).
        std::filesystem::path m_shaderDir;
        std::unordered_map<std::string, std::vector<std::uint8_t>> m_shaderBins;

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

        // FrameDesc::batch for the frame currently being declared -- see
        // CurrentBatch(). Cleared at the end of every RenderFrame so a stale
        // batcher can never be drained by a later frame.
        Batcher2D* m_currentBatch = nullptr;

        // FrameDesc::globals, by VALUE -- see CurrentGlobals().
        GlobalParams m_currentGlobals{};

        // FrameDesc::pickables / ::selectedIds, published for exactly the span
        // of one frame's declaration and cleared afterwards -- the same
        // treatment m_currentBatch gets, and for the same reason: nothing may
        // reach a stale span from a later frame.
        //
        // BOTH ARE FULLY CONSUMED AT DECLARATION TIME, which is what makes a
        // borrowed span the right shape here: PickNode::PrepareDrawables turns
        // the drawables into vertices and OutlineNode::PrepareSelection COPIES
        // the ids, both into node-owned storage, before this function returns.
        // Reading either of these from an exec fn would read an empty span.
        std::span<const PickDrawable>  m_currentPickables;
        std::span<const std::uint32_t> m_currentSelectedIds;

        // FrameDesc::imgui, published for the whole of one RenderFrame call
        // (declaration AND execution) and re-published -- possibly as null --
        // at the top of the next one, so a later frame can never reach a
        // previous frame's draw data. See CurrentImGuiDrawData().
        ImDrawData* m_currentImGui = nullptr;

        // --pick-probe: armed at Create from the config, so an ordinary run
        // never builds the nodes at all.
        bool         m_pickArmed      = false;
        std::int32_t m_probeX         = 0;
        std::int32_t m_probeY         = 0;
        // Latched when the probe pixel falls outside the current surface: the
        // node would still copy a CLAMPED texel (the frame's shape must not
        // depend on a coordinate), but reporting that id as the answer would be
        // a confident lie about a pixel nobody asked for. Said once.
        bool         m_probeOutOfRange = false;

        // See SetAssetResolver. Empty until the frame driver installs one.
        AssetResolveFn m_resolveAsset;

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

    // =====================================================================
    // THE FRAME'S SHAPE, as a free function both the vehicle and the headless
    // [nri] tests DRIVE.
    //
    // NriGraphContext::BuildFrame is a two-line wrapper over
    // DeclareGraphFrame, and RenderGraphTest.cpp's frame-shape case calls the
    // same function with a null context. That is deliberate and it is the
    // point: the previous shape test TRANSCRIBED BuildFrame's declarations
    // into the test, so a change to the frame could not make it red. It can
    // now -- change what the frame declares and the compiled barrier chain the
    // test asserts changes with it.
    //
    // A null `context` means "declarations only": every AddNode call, every
    // resource, every Read/Write and every attachment is identical, and only
    // the exec fns become no-ops (they have no device to record against).
    // =====================================================================
    struct RgFrameShape
    {
        GoldenStage   stage        = GoldenStage::Full;
        bool          capture      = false;
        // The canvas transient's extent -- the swapchain's, so the tonemap
        // samples 1:1.
        std::uint32_t canvasWidth  = 0;
        std::uint32_t canvasHeight = 0;
        // The HOST_READBACK staging buffer the capture node copies into. It is
        // IMPORTED rather than created as a transient because the graph
        // realizes transients in MemoryLocation::DEVICE, which can never be
        // mapped. May be null when `capture` is false (and headlessly even when
        // it is true -- ImportBuffer stores the pointer without dereferencing).
        nri::Buffer*  captureBuffer = nullptr;
        std::uint64_t captureBytes  = 0;

        // The scene post chain to insert between the canvas and the tonemap,
        // or null for none. Read for its per-pass WIRING and slot count here;
        // its bytecode and values are PostChainNode::PrepareChain's business.
        // Honoured only when `stage` includes the chain (Batch drops it).
        //
        // A headless drive can point this at a PostChainDesc carrying nothing
        // but `passes[i].inputs` -- the declarations depend on the wiring and
        // on nothing else, which is what lets the [nri] frame-shape cases
        // exercise the real chain shape with no device.
        const PostChainDesc* post = nullptr;

        // Declare the pick + JFA outline chain after the tonemap (Task 11).
        // The ONLY thing about that chain the declarations depend on is this
        // flag and the canvas extent -- the drawables, the selection and the
        // probe pixel are all RECORD-time data that travels through
        // NriGraphContext -- which is exactly what lets the headless [nri]
        // cases drive the real shape with a null context.
        //
        // Stage-independent, deliberately: --golden-stage names slices of the
        // SCENE render (batch / post / full), and the outline is not one of
        // them. A stage golden is taken without the probe, so the two never
        // meet in practice.
        bool pickOutline = false;

        // Declare the HUD node after the tonemap (Task 12). Unlike
        // `pickOutline` this IS stage-gated -- `full` alone draws host chrome
        // -- and the declarations depend on this flag and on nothing else: the
        // ImDrawData itself is RECORD-time data that travels through
        // NriGraphContext, which is what lets the headless [nri] cases drive
        // the real shape with a null context.
        bool imgui = false;
    };

    struct RgFrameHandles
    {
        RgTexture backbuffer{};
        RgTexture canvas{};
        RgBuffer  capture{};
        // The LAST post-chain pass's target -- what the tonemap sampled. An
        // invalid handle (and a zero count) when the frame carried no chain,
        // in which case the tonemap sampled `canvas`.
        RgTexture     post{};
        std::uint32_t postPassCount = 0;

        // The pick + outline chain's handles (Task 11). All invalid, and
        // jfaStepCount 0, on a frame that did not declare it.
        RgTexture     pickIds{};        // the R32_UINT entity-id transient
        RgBuffer      pickReadback{};   // the imported HOST_READBACK staging buffer
        RgTexture     outlineField{};   // the LAST JFA target -- what the composite sampled
        std::uint32_t jfaStepCount = 0;
    };

    ARCANE_API RgFrameHandles DeclareGraphFrame(RenderGraph& graph, const RgFrameShape& shape,
                                                 NriGraphContext* context);
}
