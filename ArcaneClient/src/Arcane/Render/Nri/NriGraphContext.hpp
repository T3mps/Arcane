#pragma once

// NriGraphContext -- the `--nri-graph` render half (NRI Phase 2, Task 7;
// LANDED on one device and one window at NRI Phase 3, Task 6).
//
// Everything the graph path needs to put a frame on screen, in one object:
// the HOST's window (borrowed, see below), the wrapper-path native device +
// its NRI wrap, an NriSwapChain, the Task 5 upload ring, the Task 7 pipeline
// cache, and ONE RenderGraph that is Reset/declared/compiled/executed every
// frame. The frame's nodes hang off BuildFrame() below: batch2d -> [post
// chain] -> tonemap -> [pick/outline] -> [imgui] -> [capture] -> present.
//
// NOT SCAFFOLDING in the sense the Phase-1 triangle smoke was. That was a
// straight-line proof with no reusable abstraction, deleted at Task 13; this
// class is the shape Phase 3 grows into -- GpuContext's render internals move
// ONTO it as the hosts flip.
//
// -------------------------------------------------------------------------
// DESK COMMANDS (GPU/windowed runs are desk-only on the dev box):
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
// migrated here at Task 13, which deleted that file. LIKE the smoke, and
// unlike this vehicle through the whole of Phase 2, THIS DEVICE IS NOW THE
// FIRST AND ONLY GRAPHICS DEVICE IN THE PROCESS: GpuContext::CreateForGraph
// builds no NVRHI device at all. Two consequences a desk user sees:
//   * dx12 Debug genuinely gets the D3D12 CPU debug layer. EnableDebugLayer is
//     a before-any-device call, and through Phase 2 it was DECLINED here
//     because the engine's NVRHI device already existed (DeviceD3D12.cpp's
//     g_d3d12DeviceCreated). Nothing is declined now, so D3D12 validation
//     messages reach D3D12DebugLayerCallback and therefore the
//     RenderErrorCount latch -- for the first time on this path.
//   * Vulkan's two-VkDevice dispatcher hazard is GONE by construction: there
//     is one VkDevice, so the Vulkan-Hpp default dispatcher binds the only one
//     there is.
//
// Both boot the REAL engine (project, plugin, scene, material compiles) and
// swap only the render half. Exit codes follow the host's existing contract:
// 0 clean, 1 the graph run failed or the device was lost, 2 RenderErrorCount
// GREW during the run (a validation error fired -- Debug turns the D3D12 debug
// layer and VK sync validation ON for exactly this), 3 a golden/screenshot
// capture or compare failure. Precedence 1 > 2 > 3.
// -------------------------------------------------------------------------
//
// THE BORROWED WINDOW (read before changing where the graph renders).
// This object does NOT own a window. Create() takes the host's -- the same
// Window GpuContext::CreateForGraph built and the same one the host's
// ImGuiLayer, InputDevices and event pump use.
//
// WHY BORROWED RATHER THAN OWNED, and why the flip had to be one landing:
// DXGI allows only ONE flip-model swap chain per HWND at a time
// (IDXGIFactory2::CreateSwapChainForHwnd's own remark), and both this
// swapchain (NRI SwapChainD3D12 -> DXGI_SWAP_EFFECT_FLIP_DISCARD) and the
// engine's (Render/DeviceD3D12.cpp, same swap effect) are flip-model. So
// through Phase 2, while an NVRHI swapchain still existed on the host window,
// this class had to create its OWN second window and present there. Removing
// the NVRHI device and binding this swapchain to the host window is therefore
// a single, indivisible change -- plan reconciliation 1.
//
// WHAT THE CALLER OWES: the borrowed window must OUTLIVE this object. The
// runtime host guarantees that by declaring m_graphContext AFTER m_gpu, so it
// is destroyed first (RuntimeApp.hpp's teardown contract). Nothing in this
// class destroys, resizes or re-titles the window -- it only reads its native
// handle at swapchain create, and Resize() is driven by the host's pump.
//
// THE HUD IS DRAWN BUT DELIBERATELY NOT INTERACTIVE on a --nri-graph run.
// Through Phase 2 that was an accident of the two-window topology (the ImGui
// event tap sat on the host window while the user's events went to the
// vehicle's). One window makes it a CHOICE, and it is kept until desk
// checkpoint D3b's compares are done for the load-bearing half of the old
// reason: an interactive HUD can be DRAGGED, ImGui persists window placement
// per exe dir in imgui.ini, and the graph path and the NVRHI path share that
// file -- so one drag on a graph run would move the HUD on the NVRHI path too,
// i.e. would change the very `full` baseline D3b compares against. The gate is
// ImGuiLayer::CreateForGraph withholding the event tap; see ImGuiLayer.cpp.
//
// The HUD's DISPLAY SIZE now needs no lockstep at all, which is the other half
// of the landing: ImGui_ImplSDL3_NewFrame reads the window the platform
// backend was initialised with, and that is the same window this swapchain
// binds. One surface, one extent. It defaults to 1280x720, the size every
// golden was captured at.
//
// -------------------------------------------------------------------------
// OFFSCREEN MODE (NRI Phase 3, Task 7) -- the SECOND way to run one of these.
//
// CreateOffscreen() builds a vehicle with NO WINDOW and NO SWAPCHAIN that
// renders the same frame into a persistent, sampled texture it owns, and
// presents nothing. That is the editor's capability: a viewport panel is an
// ImGui::Image over a texture, not a surface, so the frame has to end in a
// SHADER_RESOURCE the chrome pass can sample rather than in a PRESENT.
//
// IT IS ADDITIVE. The host-window mode above is untouched -- same Create(),
// same RenderFrame(), same Resize(), same barriers, same goldens. What the two
// modes share is EVERYTHING except the final target: DeclareGraphFrame builds
// one frame shape and the tonemap writes either the acquired backbuffer or the
// imported output (RgFrameShape::offscreenOutput). A node census over the two
// is identical, which is what the [nri] cases pin.
//
// The four differences, in full:
//   * the device is SHARED, not owned (see below);
//   * there is no NriSwapChain, so Execute() runs with
//     RgExecuteDesc::swapChain = nullptr -- no acquire, no present -- and the
//     graph's own submission fence is the frame's only completion signal;
//   * PACING is therefore ours: RenderFrameOffscreen waits THIS object's own
//     timeline fence kSwapchainFramesInFlight deep, signalled by a trailing
//     signal-only submit, exactly the shape NriSwapChain::Present uses to
//     stamp its pacing fence. Without it nothing would bound frames in flight
//     and the per-frame-slot command buffers, upload-ring arena and node
//     descriptor sets would all be rewritten under a GPU still reading them;
//   * the output texture is BGRA8_UNORM and persistent, recreated only by
//     ResizeOffscreen().
//
// ONE DEVICE PER PROCESS, TWO CONTEXTS. The editor holds the host-window
// context (chrome -> present) and an offscreen context (the viewport), over
// ONE NriDevice: the host-window one creates and owns it, the offscreen one
// BORROWS it. That is not a convenience -- Vulkan-Hpp's default dispatcher
// binds one VkDevice per process and DXGI allows one flip-model swapchain per
// HWND, so a second device is exactly the topology Task 6 removed.
//
// ============ CALLER CONTRACT: TWO CONTEXTS, ONE GRAVEYARD ============
// READ THIS BEFORE WIRING A SECOND CONTEXT ONTO A LIVE DEVICE (Task 8).
//
// The Graveyard is per-DEVICE (NriDevice::Graves()), not per-context, and
// every burial is keyed to the burying GRAPH's own submission fence value.
// Two contexts have two RenderGraphs and therefore two INDEPENDENT fence
// timelines whose values mean nothing to each other, so with both live:
//   * Graveyard::Bury's nondecreasing-fenceValue assert can fire in Debug the
//     moment the two counters interleave out of order; and, worse,
//   * RenderGraph::Execute reaps with ITS fence's completed value, which can
//     run the OTHER graph's thunks before that graph's submission retired --
//     a use-after-free no assert catches.
// Nothing in the tree hits this today: this task adds the capability and no
// caller, and a lone offscreen context (one graph, one timeline) is as safe as
// the host-window one. The fix belongs with the first caller and is a
// per-context Graveyard lane threaded through RgExecuteDesc + the node
// Release() calls -- deliberately NOT done here, because changing which
// graveyard the HOST-WINDOW graph buries into would move the very path desk
// checkpoint D3b is currently pinning. See the Task 7 report.
// =====================================================================
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

    // THE OFFSCREEN OUTPUT FORMAT (NRI Phase 3, Task 7). The NVRHI twin's
    // rationale carries over verbatim -- OffscreenCanvas.cpp:22, quoted:
    //
    //     "Display-referred output. BGRA8_UNORM is exactly the engine
    //      backbuffer format (see DeviceD3D12/DeviceVulkan kSwapchainFormat):
    //      the tonemap already gamma-2.2 encodes, so a plain UNORM target
    //      matches what a real backbuffer shows AND lets the ImGui-NVRHI
    //      backend sample it without any extra sRGB conversion. A `_SRGB`
    //      format would double-apply gamma on the ImGui sample."
    //
    // Every clause holds on this path with one word changed: the sampler is
    // ImGuiNri's rather than ImGui-NVRHI's, and it samples the raw
    // nri::Texture* this class hands out (OffscreenTextureId). It is stated as
    // a CONCRETE format rather than resolved the way NriSwapChain::Format() is,
    // and that is the difference between an output we create and a surface the
    // driver hands us: nothing here is free to pick a channel order.
    inline constexpr nri::Format kGraphOffscreenFormat = nri::Format::BGRA8_UNORM;

    class ARCANE_API NriGraphContext
    {
    public:
        // Which of the two ways this vehicle was created -- see OFFSCREEN MODE
        // in the file header. Not a runtime switch: it is fixed at Create and
        // decides which of the RenderFrame/Resize pairs is legal to call.
        enum class Mode : std::uint8_t { HostWindow, Offscreen };

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
        //
        // OFFSCREEN MODE reads `Presented` as "rendered and submitted" -- there
        // is nothing to present to. Its Skipped cases are a zero-extent target
        // and a context whose last ResizeOffscreen could not create one (both
        // routine for a collapsed editor panel); Failed still means the latch
        // grew.
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

        // Builds native device + NRI wrap + swapchain (over `window`) + ring +
        // cache + graph, in that order, honouring `config.backend` and
        // `config.vsync`. Null on any failure (already logged + latched).
        //
        // `window` is BORROWED and must outlive the returned object -- see THE
        // BORROWED WINDOW above. It must already exist and carry a native
        // handle, and (on Vulkan) have been created with SDL_WINDOW_VULKAN;
        // GpuContext::CreateForGraph does both.
        //
        // Debug turns validation ON -- the D3D12 debug layer through
        // ID3D12InfoQueue1, VK core + SYNCHRONIZATION validation, and NRI's own
        // validation layer -- all three of which end at RenderErrorCount. That
        // is the dev-loop gate for the whole phase (NRI validation alone
        // cannot catch barrier bugs). Since Task 6 this device is the FIRST in
        // the process, so the D3D12 debug-layer request is no longer declined.
        static std::unique_ptr<NriGraphContext> Create(const HostConfig& config, Window& window);

        // THE OFFSCREEN FLAVOR (NRI Phase 3, Task 7) -- see OFFSCREEN MODE in
        // the file header for the full contract, and CALLER CONTRACT: TWO
        // CONTEXTS, ONE GRAVEYARD for what a second live context owes.
        //
        // No window, no swapchain: builds the ring, the cache, the graph, the
        // nodes and ONE persistent kGraphOffscreenFormat output texture at
        // `width` x `height`, then renders through RenderFrameOffscreen().
        // Null on any failure (already logged + latched), including a zero
        // extent -- unlike a minimised window, a zero-sized offscreen target is
        // a caller bug rather than a routine state, because nothing outside
        // this call chose the size.
        //
        // `shared` is BORROWED and must OUTLIVE this object. It is deliberately
        // the whole device rather than a backend enum: one process holds one
        // graphics device (Task 6), and the host-window context is what creates
        // and owns it. `config` is read for the same knobs Create() reads
        // EXCEPT the ones a surface owns -- vsync is meaningless with nothing to
        // present to, and the open-ended drag-storm heartbeat is a windowed
        // desk affordance, so neither is armed here.
        //
        // IT DOES NOT ARM THE CRASH CHAIN. NriDiagnostics::Arm/Disarm install
        // and clear ONE process-wide slot with no per-owner identity, so an
        // offscreen context that armed would be harmless but one that DISARMED
        // would tear down the chain the host-window context owns. The device
        // this borrows is already covered by whoever created it; see
        // ~NriGraphContext.
        static std::unique_ptr<NriGraphContext> CreateOffscreen(const HostConfig& config,
                                                                NriDevice& shared,
                                                                std::uint32_t width,
                                                                std::uint32_t height);

        ~NriGraphContext();

        NriGraphContext(const NriGraphContext&)            = delete;
        NriGraphContext& operator=(const NriGraphContext&) = delete;

        [[nodiscard]] Mode Kind()        const noexcept { return m_mode; }
        [[nodiscard]] bool IsOffscreen() const noexcept { return m_mode == Mode::Offscreen; }

        // The window this swapchain is bound to -- the HOST's, borrowed (see
        // THE BORROWED WINDOW above). Handed back so a caller that only holds
        // the graph context can reach it; the frame driver pumps the same
        // object through its own GpuContext and feeds resizes here.
        //
        // HOST-WINDOW MODE ONLY. An offscreen context borrows no window and
        // this would dereference null -- check IsOffscreen() first. It has no
        // caller in the tree yet (the plan expects the editor tasks to consume
        // it); it is kept rather than deleted for that reason, and the
        // precondition is stated here rather than left to be discovered.
        [[nodiscard]] Window& Win() noexcept { return *m_borrowedWindow; }

        // The extent the frame is being rendered at, whichever mode this is --
        // the swapchain's, or the offscreen output's. The mode-agnostic reading
        // of Swap().Width()/Height(), which an offscreen context cannot answer.
        [[nodiscard]] std::uint32_t SurfaceWidth()  const noexcept;
        [[nodiscard]] std::uint32_t SurfaceHeight() const noexcept;

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
        //
        // HOST-WINDOW MODE ONLY -- ResizeOffscreen() is the offscreen twin.
        void Resize(std::uint32_t width, std::uint32_t height);

        // One frame: Reset the graph, declare this frame's nodes, Compile,
        // Execute (which acquires, records, submits and presents).
        //
        // HOST-WINDOW MODE ONLY -- the offscreen twin is below.
        FrameOutcome RenderFrame(const FrameDesc& frame);

        // ------------------------------------------------------------------
        // THE OFFSCREEN HALF (NRI Phase 3, Task 7). Everything below is
        // OFFSCREEN MODE ONLY and refuses (loudly, latched) on a host-window
        // context, exactly as RenderFrame/Resize refuse on an offscreen one --
        // a vehicle that quietly did the wrong thing for its mode would be a
        // wrong picture, not an error.
        // ------------------------------------------------------------------

        // One frame into the persistent output: pacing wait, Reset, declare
        // (the tonemap's final target is the IMPORTED output rather than an
        // acquired backbuffer), Compile, Execute with no swapchain, then stamp
        // the pacing fence. Presents nothing.
        //
        // On return -- once this frame's submission retires, which the next
        // pacing wait is what guarantees -- OffscreenOutput() is in
        // SHADER_RESOURCE and is the frame's finished, display-referred image.
        // FrameOutcome means what it does on the present path: Skipped is
        // routine (a zero extent), Failed has already bumped the latch.
        FrameOutcome RenderFrameOffscreen(const FrameDesc& frame);

        // The persistent output texture -- kGraphOffscreenFormat, owned here,
        // stable until ResizeOffscreen(). Null on a host-window context.
        //
        // BORROWED BY THE CALLER, and ResizeOffscreen INVALIDATES IT: a caller
        // that cached this pointer across a resize holds a destroyed texture
        // (and NRI may hand its address to the replacement, so a pointer
        // comparison cannot detect it -- the same hazard RenderGraph's imported
        // views exist to dodge). Re-read it after every resize.
        [[nodiscard]] nri::Texture* OffscreenOutput() noexcept { return m_offscreen; }

        // OffscreenOutput() as an ImGui texture id, ready for ImGui::Image().
        //
        // The return type is `std::uint64_t`, which IS `ImTextureID` (imgui.h:
        // `typedef ImU64 ImTextureID`, and ImU64 is `unsigned long long`) --
        // spelled as the underlying type for the same reason ImGuiNri.hpp and
        // OffscreenCanvas::TextureId() do: no NRI header in this directory
        // pulls in <imgui.h>, and one that did would drag it into every
        // consumer of this header for a typedef.
        //
        // THE ImGuiNri CONVENTION (that header's §7.3): an ImTextureID is the
        // backend's RAW TEXTURE HANDLE cast through intptr_t -- here the
        // nri::Texture* itself, not a descriptor. 0 on a host-window context,
        // which is ImTextureID_Invalid.
        [[nodiscard]] std::uint64_t OffscreenTextureId() const noexcept;

        // Destroy + recreate the output at the new size. MUST be called
        // strictly BETWEEN RenderFrameOffscreen() calls, the same contract
        // Resize() carries and for the same reason.
        //
        // Not a bare destroy/create pair, for the reason Resize() is not a bare
        // forward to NriSwapChain::Resize: it first idles, releases the graph's
        // GPU resources and DRAINS the graveyard, so that every descriptor
        // naming the current output -- the graph's own imported attachment view
        // above all -- is destroyed BEFORE the texture it views is. The node
        // view invalidation (PoolEpoch discipline) is byte-for-byte the
        // present path's. A no-op on an unchanged size, and on a zero extent it
        // logs and leaves the current output alone.
        void ResizeOffscreen(std::uint32_t width, std::uint32_t height);

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
        // HOST-WINDOW MODE ONLY: an offscreen context holds no swapchain and
        // this would dereference null. SurfaceWidth()/SurfaceHeight() above are
        // the mode-agnostic readings of its two most-asked questions.
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
        //
        // OFFSCREEN MODE counts frames RENDERED (nothing is presented), and the
        // slot property is preserved by the same construction: it advances only
        // on a frame that submitted, and RenderFrameOffscreen's own pacing
        // fence -- signalled once per such frame, waited on at the same depth --
        // is what makes reusing the slot safe. The name is kept because the
        // counter's MEANING for its one consumer (the slot clock) is unchanged.
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

        bool Init(const HostConfig& config, Window& window);
        bool InitOffscreen(const HostConfig& config, NriDevice& shared,
                           std::uint32_t width, std::uint32_t height);
        // The half both Init flavors share, in the order the teardown contract
        // below reverses: ring, pipeline cache, texture cache, graph, shader
        // dir, nodes. Everything above it (device, swapchain / output texture)
        // is the flavor's own.
        bool InitCommon(const HostConfig& config);

        // Creates the persistent offscreen output at `width` x `height` and
        // publishes it as m_offscreen. False (already logged + latched) on
        // failure. Does NOT destroy a previous one -- ResizeOffscreen sequences
        // that, because the destroy has to follow a drain.
        bool CreateOffscreenTarget(std::uint32_t width, std::uint32_t height);

        // Declares this frame's nodes into m_graph (already Reset()). The one
        // place Tasks 8-12 add to. Mode-agnostic: the only thing the mode
        // changes is RgFrameShape::offscreenOutput.
        void BuildFrame(const FrameDesc& frame);

        // Creates the HOST_READBACK staging buffer for the current surface
        // extent (the swapchain's, or the offscreen output's), or reuses the
        // existing one when the extent is unchanged. False (already logged) on
        // failure -- the frame still renders, it just cannot be captured.
        bool EnsureCaptureBuffer();

        // --- TEARDOWN CONTRACT: declaration order == reverse destruction ---
        // m_graph dies first (it buries its pool/views/command slots into the
        // device's graveyard at its own last submitted fence value), then the
        // cache and the ring, then the swapchain, then the NRI device (whose
        // destructor idles the device and DRAINS that graveyard), then the
        // native device it wrapped. Do NOT reorder: this is contract item 15
        // (the NRI device is destroyed BEFORE the native one) plus the
        // graveyard's "everything buried must outlive nothing".
        //
        // THE WINDOW IS NO LONGER PART OF THIS CONTRACT because this object no
        // longer owns one (Task 6). It is the host's, and the swapchain that
        // names its HWND/surface must still die before it does -- which the
        // host guarantees by OUTER ordering instead: RuntimeApp declares
        // m_graphContext after m_gpu, so this whole object (swapchain
        // included) is destroyed while that window is still alive. A pointer
        // rather than a reference so the default ctor + Init() shape survives.
        // OFFSCREEN MODE holds neither of the first three: no window, no native
        // device (the shared one's owner has it) and no swapchain. m_device
        // points at m_ownedDevice in host-window mode and at the caller's
        // NriDevice in offscreen mode -- which is exactly why the owning handle
        // and the reading pointer are two members rather than one. The teardown
        // order below is unchanged by the split: m_ownedDevice sits where
        // m_device used to, so a host-window context is destroyed member for
        // member as before, and an offscreen context simply has nothing there.
        Mode                               m_mode = Mode::HostWindow;
        Window*                            m_borrowedWindow = nullptr;
        std::unique_ptr<NativeDeviceOwner> m_native;
        std::unique_ptr<NriDevice>         m_ownedDevice;
        NriDevice*                         m_device = nullptr;
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

        // ---- offscreen mode (NRI Phase 3, Task 7) --------------------------
        // The persistent output, and the pacing timeline that bounds frames in
        // flight without a swapchain to do it. Both null/absent in host-window
        // mode; both buried/destroyed in ~NriGraphContext, before the drain,
        // like every other NRI object here.
        //
        // m_offscreenFence is signalled with m_frameIndex + 1 by a trailing
        // signal-only QueueSubmit and waited on kSwapchainFramesInFlight deep
        // at the top of the next frame -- the same 1-based values, the same
        // depth and the same polling wait NriSwapChain's pacing fence uses.
        nri::Texture* m_offscreen       = nullptr;
        nri::Fence*   m_offscreenFence  = nullptr;
        std::uint32_t m_offscreenWidth  = 0;
        std::uint32_t m_offscreenHeight = 0;

        // Whether THIS context armed the process-wide crash chain, and
        // therefore whether its destructor may disarm it. Only a host-window
        // context ever arms (CreateOffscreen's comment says why), and only an
        // armer may disarm: NriDiagnostics keeps ONE slot with no owner
        // identity, so an unconditional Disarm() from a second context would
        // silently unplug the first one's chain.
        bool          m_armedDiagnostics = false;

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

        // OFFSCREEN MODE (NRI Phase 3, Task 7). The persistent, sampled
        // texture the tonemap renders into INSTEAD of a swapchain backbuffer;
        // null means "present mode", i.e. the tonemap calls
        // ImportSwapChainTexture as it always did.
        //
        // IT IS THE ONLY DIFFERENCE BETWEEN THE TWO MODES' FRAMES. Everything
        // downstream of the tonemap -- the outline composite, the HUD, the
        // capture -- writes RgFrameHandles::backbuffer, which is whichever of
        // the two this chose, so the node census, the reads/writes and every
        // derived barrier except the final one are identical. That is pinned by
        // an [nri] case, so a later node that branches on this field will make
        // it red.
        //
        // Held as a bare nri::Texture* and never dereferenced at declaration
        // time (ImportTexture only records it), which is what lets the headless
        // [nri] cases drive the real offscreen shape with a stand-in pointer --
        // the same licence `captureBuffer` above takes.
        nri::Texture* offscreenOutput = nullptr;

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
        // THE FRAME'S FINAL TARGET -- the acquired swapchain backbuffer, or
        // (when RgFrameShape::offscreenOutput was set) the imported offscreen
        // output. Named for the common case rather than renamed at Task 7:
        // every node downstream of the tonemap writes THIS handle whichever it
        // is, and a second name would have suggested a second code path where
        // there is none.
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
