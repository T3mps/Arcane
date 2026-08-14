// NriGraphContext -- see the header for what the vehicle is, the two desk
// commands, the exit-code contract, and THE TWO-DEVICE WINDOW (why this object
// owns a window instead of borrowing the host's, and what Vulkan still owes).
//
// Same include-order rule as every file in this directory (NriCommon.hpp): NRI
// headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRISwapChain.h>

#include "NriGraphContext.hpp"

#include "NriCommon.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Device.hpp>                 // RenderDeviceDesc, RenderErrorCount, ToString(GraphicsBackend)
#include <Arcane/Render/NvrhiMessageCallback.hpp>   // the tagged "nri-graph" error seam
#include <Arcane/Render/PostChainCache.hpp>         // PostChainDesc -- the frame's post-chain shape
#include <Arcane/Render/ShaderLibrary.hpp>          // ShaderLibrary::ResolveFlavorDir
#include <Arcane/Render/Swapchain.hpp>              // kSwapchainFramesInFlight

#undef ERROR

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Arcane
{
    namespace
    {
        // The tagged seam RenderGraphExec.cpp reports through -- the VEHICLE's
        // own refusals, not an NRI call's result (those go through
        // ARC_NRI_CHECK). Both land in the RenderErrorCount() latch, which is
        // what makes a `--nri-graph` desk run's exit code meaningful.
        void GraphError(const std::string& text)
        {
            NvrhiMessageCallback::Instance().NoteError("nri-graph", text.c_str());
        }

        // Per-frame-slot upload arena -- the batch node's vertex + index
        // streams (Task 8), the material/globals constant buffers (Task 9).
        // Init()'d at Create so a mapping failure is found at boot rather than
        // inside the first draw. A starting point, not a measurement:
        // NriUploadRing::HighWater() is the number to size it from, and it is
        // logged at shutdown. 4 MiB is ~27k quads of batch geometry
        // (128 B of vertices + 24 B of indices each).
        constexpr std::uint64_t kUploadRingBytesPerFrame = 4ull * 1024 * 1024;

        // Where the offline shader artifacts live, relative to the executable.
        // The SAME literal GpuContext::Create hands ShaderLibrary
        // (Host/GpuContext.cpp), resolved through the same function -- so a
        // desk user pointing ARCANE_SHADER_DIR at a live recompile moves both
        // render paths together instead of comparing two different shaders.
        constexpr const char* kShaderDir = "data/shaders";

        // WritePngRgba wants RGBA; NRI resolves the swapchain's channel order
        // rather than pinning it (NriSwapChain::Format()), so a BGRA
        // backbuffer has to be swizzled on the way out. Exactly the check
        // NRISamples' Readback.cpp performs on its mapped pixel, reached here
        // through NriSmoke.cpp.
        bool IsBgraFormat(nri::Format format) noexcept
        {
            return format == nri::Format::BGRA8_UNORM || format == nri::Format::BGRA8_SRGB;
        }

        std::uint64_t AlignUp(std::uint64_t value, std::uint32_t alignment) noexcept
        {
            return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
        }
    }

    std::unique_ptr<NriGraphContext> NriGraphContext::Create(const HostConfig& config)
    {
        std::unique_ptr<NriGraphContext> context(new NriGraphContext());
        if (!context->Init(config))
            return nullptr;
        return context;
    }

    bool NriGraphContext::Init(const HostConfig& config)
    {
        m_vsync = config.vsync;

        ARC_INFO("[nri-graph] starting the graph vehicle: backend={} vsync={} frames={}",
                 ToString(config.backend), config.vsync ? "on" : "off", config.maxFrames);

        // -------------------------------------------------------------
        // Window. Same WindowDesc defaults GpuContext::Create uses
        // (1280x720, resizable) -- deliberately, because the golden
        // baselines were captured at the NVRHI path's backbuffer size and a
        // different default here would make every stage compare a dimension
        // mismatch. Two differences: its own title, and NOT hidden (this is
        // the visible window on a --nri-graph run; see the header's TWO-DEVICE
        // WINDOW note). SDL_WINDOW_VULKAN must be set for a window a Vulkan
        // surface will be created against.
        // -------------------------------------------------------------
        {
            WindowDesc wd;
            wd.title     = "Arcane NRI Graph";
            wd.vulkan    = (config.backend == GraphicsBackend::Vulkan);
            wd.resizable = true;
            wd.hidden    = false;
            if (!m_window.Create(wd))
            {
                ARC_ERROR("[nri-graph] window create failed");
                return false;
            }
        }

        // -------------------------------------------------------------
        // The creation half, with validation forced ON in Debug -- every
        // channel a validation message can arrive through ends at
        // RenderErrorCount, which is what makes this run's exit code mean
        // something (VK core + sync validation -> DeviceVulkan.cpp's
        // VkDebugCallback; the D3D12 debug layer -> DeviceD3D12.cpp's
        // ID3D12InfoQueue1 callback, which is why enableD3D12DebugLayer is
        // forced here since it defaults FALSE for the Nahimic-OSD fail-fast
        // hazard; NRI's own validation layer -> MakeNriCallbacks). Identical
        // wiring to NriSmoke::RunSession -- read that function's comment for
        // the desk hazard each one carries.
        //
        // ONE OF THOSE THREE IS A REQUEST, NOT A GUARANTEE, on THIS path
        // (D1 shakedown): the D3D12 debug layer is process-global and can only
        // be turned on before the process's FIRST device, and by the time the
        // vehicle runs, the engine's NVRHI device is already live. So
        // enableD3D12DebugLayer below is honoured on the pre-boot smoke and
        // declined here, with a WARN naming the reason -- see
        // DeviceD3D12.cpp's g_d3d12DeviceCreated for the full tradeoff (the
        // alternative was removing the engine's device, which is what the
        // first desk run actually did). NRI validation and, on Vulkan, the VK
        // validation layers are per-device and unaffected: they are what makes
        // a dx12 vehicle run's exit code mean something today.
        // -------------------------------------------------------------
        RenderDeviceDesc dd;
        dd.backend = config.backend;
#if defined(ARCANE_DEBUG)
        dd.enableValidation      = true;
        dd.enableD3D12DebugLayer = true;
        dd.enableSyncValidation  = true;   // VK-only; see Device.hpp
#else
        // Release/Dist: leave RenderDeviceDesc's own defaults (validation off)
        // rather than forcing debug layers into an optimized build. A Release
        // graph run is a performance/behaviour check; its exit code still
        // fails on any error the NRI callbacks report.
#endif

        if (config.backend == GraphicsBackend::Vulkan)
        {
            // THE HAZARD, said out loud at the one moment it is created. See
            // the header's TWO-DEVICE WINDOW item 1: the engine's NVRHI Vulkan
            // device is already live on this path, and the creation half below
            // re-inits the process-wide Vulkan-Hpp dispatcher onto THIS
            // device. A desk run that behaves strangely on vulkan-only should
            // suspect this first, not the graph.
            ARC_WARN("[nri-graph] vulkan: this process now holds TWO VkDevices (the engine's NVRHI "
                     "device and the graph's). The Vulkan-Hpp default dispatcher binds one -- see "
                     "NriDevice.hpp. dx12 is unaffected; one-device-per-process is the Phase 3 flip.");
        }

        m_native = NativeDeviceOwner::Create(dd);
        if (!m_native)
        {
            ARC_ERROR("[nri-graph] native {} device creation failed", ToString(config.backend));
            return false;
        }

        m_device = NriDevice::Wrap(*m_native);
        if (!m_device)
        {
            ARC_ERROR("[nri-graph] wrapping the native device failed");
            return false;
        }

        m_swap = NriSwapChain::Create(*m_device, m_window, m_vsync);
        if (!m_swap)
        {
            // The most likely cause on dx12, and the one worth naming: DXGI
            // allows only one flip-model swap chain per HWND at a time. If
            // this ever fires while the vehicle is presenting into a window
            // some other swapchain already owns, that is the bug -- see the
            // header's TWO-DEVICE WINDOW item 2.
            ARC_ERROR("[nri-graph] swapchain create failed (dx12: is another flip-model swapchain "
                      "already associated with this HWND?)");
            return false;
        }
        m_format = m_swap->Format();
        if (m_format == nri::Format::UNKNOWN)
        {
            ARC_ERROR("[nri-graph] the swapchain resolved no format (zero-sized window at create?)");
            return false;
        }

        // The upload ring is [gpu]-only by construction (NONE's MapBuffer
        // returns null), so this is its first real Init in the tree.
        if (!m_ring.Init(*m_device, kUploadRingBytesPerFrame))
        {
            ARC_ERROR("[nri-graph] upload-ring init failed ({} bytes x {} slots)",
                      kUploadRingBytesPerFrame, kSwapchainFramesInFlight);
            return false;
        }

        m_pipelines.Bind(*m_device);
        m_graph = std::make_unique<RenderGraph>();

        // The offline artifacts the nodes below need as RAW BYTECODE (NRI's
        // ShaderDesc takes a blob; ShaderLibrary hands back nvrhi handles).
        // Resolved once, here, so a missing shader directory is one loud line
        // at boot instead of a per-node mystery.
        m_shaderDir = ShaderLibrary::ResolveFlavorDir(config.backend, kShaderDir);
        if (m_shaderDir.empty())
        {
            ARC_ERROR("[nri-graph] no shader directory -- the graph path cannot build its pipelines");
            return false;
        }

        // Built EAGERLY, not on the first frame: a node that cannot be created
        // must fail the vehicle at boot rather than render a frame that
        // silently draws nothing.
        m_batch2D = Batch2DNode::Create(*this);
        if (!m_batch2D)
            return false;   // already logged
        m_post = PostChainNode::Create(*this);
        if (!m_post)
            return false;   // already logged
        m_tonemap = TonemapNode::Create(*this);
        if (!m_tonemap)
            return false;   // already logged

        // See the header: the heartbeat exists for the open-ended drag-storm
        // run and nothing else. The baseline is taken here rather than at the
        // host's own baseline point so the number it prints means "errors this
        // vehicle has produced", which is the question a desk user is asking
        // mid-drag.
        m_heartbeat     = (config.maxFrames == 0);
        m_errorBaseline = RenderErrorCount();
        m_lastHeartbeat = std::chrono::steady_clock::now();

        ARC_INFO("[nri-graph] ready: {}x{} format={} textures={} ring={}KiB/slot",
                 m_swap->Width(), m_swap->Height(), (int)m_format, m_swap->TextureCount(),
                 kUploadRingBytesPerFrame / 1024);
        return true;
    }

    std::span<const std::uint8_t> NriGraphContext::ShaderBytecode(const char* name)
    {
        if (!name || m_shaderDir.empty())
            return {};

        const auto cached = m_shaderBins.find(name);
        if (cached != m_shaderBins.end())
            return std::span<const std::uint8_t>(cached->second);

        const std::filesystem::path path = m_shaderDir / (std::string(name) + ".bin");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            ARC_ERROR("[nri-graph] shader artifact missing/unreadable: {}", path.string());
            return {};
        }
        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(size > 0 ? (std::size_t)size : 0);
        if (bytes.empty() || !file.read(reinterpret_cast<char*>(bytes.data()), size))
        {
            ARC_ERROR("[nri-graph] shader artifact could not be read: {}", path.string());
            return {};
        }

        // Cached by VALUE in a node-based container: the node authors hold
        // spans into these vectors for their whole lifetime, and
        // NriPipelineCache dereferences the blob after the fill callback
        // returns (its fill contract, rule 2).
        const auto inserted = m_shaderBins.emplace(name, std::move(bytes));
        return std::span<const std::uint8_t>(inserted.first->second);
    }

    NriGraphContext::~NriGraphContext()
    {
        if (!m_device)
            return;   // Init() failed before the device existed; nothing was created on one

        const nri::CoreInterface& core = m_device->Core();

        // The last submit may still be in flight. Idle before releasing
        // anything it referenced -- ~NriDevice idles again before draining,
        // which is harmless.
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        // ONE fence value for every burial below, and it is the graph's own
        // last submitted value -- the same one RenderGraph::
        // ReleaseGpuResources and ~RenderGraph bury at. That is what keeps
        // Graveyard's NONDECREASING rule satisfied on a device the graph has
        // already buried against: NriSmoke's teardown pattern (bury everything
        // at fence 0 after an idle) would violate it here, because the graph
        // has been burying at m_submitValue all run.
        const std::uint64_t fence = m_graph ? m_graph->DebugSubmitCount() : 0;
        Graveyard& graves = m_device->Graves();

        if (m_capture)
        {
            const nri::CoreInterface* graveCore = &core;
            graves.Bury(fence, [graveCore, b = m_capture] { graveCore->DestroyBuffer(b); });
            m_capture = nullptr;
        }

        // The nodes' own objects go out at the same fence, and BEFORE the
        // graph releases its transient pool below -- the tonemap's source view
        // names a POOL texture, so a view buried after the texture it views
        // would reach DestroyDescriptor over freed memory (the same class of
        // teardown bug as fix round 1's imported-view ordering).
        if (m_tonemap)
            m_tonemap->Release(graves, fence);
        if (m_post)
            m_post->Release(graves, fence);
        if (m_batch2D)
            m_batch2D->Release(graves, fence);

        // The sanctioned cache release (see NriPipelineCache.hpp): explicit,
        // at a fence the caller knows, rather than the destructor's direct-
        // destroy safety net.
        m_pipelines.Clear(graves, fence);

        // Buries the transient pool + EVERY cached attachment view at the same
        // value -- including the IMPORTED views, which name this frame's
        // swapchain backbuffers (ReleaseGpuResources -> ReleaseImportedViews).
        if (m_graph)
            m_graph->ReleaseGpuResources();

        // ...and RUNS those burials HERE rather than leaving them pending.
        // Load-bearing, and Vulkan-only in its consequences (fix round 1,
        // finding 1):
        //
        // The graveyard's ordinary drain site is ~NriDevice -- which runs AFTER
        // ~NriSwapChain in this class's member order, and NriSwapChain's
        // teardown destroys the backbuffer IMAGES along with the swapchain. A
        // view left buried above would therefore reach DestroyDescriptor with
        // its VkImage already gone: exactly the "a VkImageView outliving its
        // VkSwapchainKHR is a validation error" pattern NriSmoke.cpp's
        // ~Resources documents and sidesteps by destroying its own views
        // directly. On a vehicle whose entire exit-code contract is "the latch
        // did not grow", that is not an ordering nit -- it is a guaranteed
        // nonzero exit on vulkan.
        //
        // Draining here fixes it structurally: a destructor BODY runs before
        // any member is destroyed, so the swapchain and its images are still
        // alive at this line. Graveyard::Drain's precondition -- the caller has
        // already made the GPU idle -- is satisfied by the DeviceWaitIdle at
        // the top of this function. Everything buried above (capture buffer,
        // pipeline cache, pool, views) goes out in this one sweep; ~RenderGraph
        // then buries the command slots and the submission fence into a now-
        // EMPTY graveyard (so nondecreasing holds trivially), and ~NriDevice
        // drains that as usual. Neither of those names a swapchain image, so
        // their ordering is unchanged and safe.
        graves.Drain();

        // The number to size kUploadRingBytesPerFrame from once a real frame
        // has run -- the peak across every slot, not slot 0's.
        std::uint64_t ringPeak = 0;
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
            ringPeak = ringPeak < m_ring.HighWater(slot) ? m_ring.HighWater(slot) : ringPeak;
        ARC_INFO("[nri-graph] vehicle shut down after {} presented frame(s); upload-ring peak "
                 "{} of {} bytes per slot", m_frameIndex, ringPeak, kUploadRingBytesPerFrame);
    }

    void NriGraphContext::Resize(std::uint32_t width, std::uint32_t height)
    {
        if (!m_swap)
            return;

        // ==============================================================
        // EVERYTHING THAT NAMES A BACKBUFFER DIES BEFORE THE BACKBUFFERS DO.
        // ==============================================================
        // NriSwapChain::Resize destroys and recreates every swapchain texture
        // (its header: destroy+recreate is the only path NRI offers). The
        // graph holds a COLOR_ATTACHMENT view over the currently-acquired
        // backbuffer from the last Execute -- it turns those over per frame
        // (RenderGraph::ReleaseImportedViews) but by BURYING them, so at this
        // instant they are still live descriptors naming images that are about
        // to be freed. Letting the resize proceed would leave them to be
        // destroyed later against a dead VkImage -- the same validation error
        // as the teardown case in ~NriGraphContext, and the one D2's vulkan
        // drag-storm would hit on the FIRST drag (fix round 1, finding 1).
        //
        // Order below is the whole point, and each step needs the one above it:
        //   1. idle -- Graveyard::Drain's stated precondition, and NriSwapChain
        //      does its own QueueWaitIdle only LATER, inside Resize();
        //   2. drop the capture staging buffer, whose size is the old extent;
        //   3. release the pool + every view (imported ones included);
        //   4. DRAIN, so 2 and 3 actually execute while the images still exist;
        //   5. only now, resize.
        // Releasing the transient pool here is not collateral damage: a pool
        // slot's desc is extent-derived, so RealizePool would re-create it on
        // the next frame anyway (and Phase 2's frame has no transients at all).
        // A resize is a rare event; a dangling descriptor is not a rare bug.
        const nri::CoreInterface& core = m_device->Core();
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        Graveyard& graves = m_device->Graves();
        if (m_capture)
        {
            const nri::CoreInterface* graveCore = &core;
            graves.Bury(m_graph ? m_graph->DebugSubmitCount() : 0,
                        [graveCore, b = m_capture] { graveCore->DestroyBuffer(b); });
            m_capture = nullptr;
        }
        m_captureRecorded = false;

        // Both fullscreen nodes hold SHADER_RESOURCE views over pool textures
        // -- the tonemap over whatever it samples, the chain over the canvas
        // and its own targets -- and ReleaseGpuResources below is about to
        // bury every one of them, at addresses NRI is free to hand to their
        // replacements. Drop them here, in the same order and for the same
        // reason the graph turns its own imported views over: a descriptor
        // that outlives the resource it views is a use-after-free a pointer
        // comparison reports as a cache hit.
        //
        // NOT the only path that destroys pool textures -- RealizePool does
        // too, on a shrink or a desc change, from inside Execute(). That one
        // the nodes catch themselves through RenderGraph::PoolEpoch, because
        // it happens where no owner can sequence it. THIS path is invalidated
        // explicitly instead because it is followed by a graveyard DRAIN and
        // possibly by teardown: there may be no later Record() to notice the
        // epoch at all. (FullscreenNodes.hpp, SOURCE VIEWS AND THE POOL.)
        const std::uint64_t viewFence = m_graph ? m_graph->DebugSubmitCount() : 0;
        if (m_tonemap)
            m_tonemap->InvalidateSource(graves, viewFence);
        if (m_post)
            m_post->InvalidateSources(graves, viewFence);

        if (m_graph)
            m_graph->ReleaseGpuResources();
        graves.Drain();

        // NriSwapChain's "no Resize between Acquire and Present" caller
        // contract is satisfied structurally here: the graph's acquire/present
        // pair lives entirely inside one Execute(), and this is only ever
        // called between RenderFrame() calls (header: Resize's contract).
        m_swap->Resize(width, height);

        const nri::Format resolved = m_swap->Format();
        if (resolved != nri::Format::UNKNOWN && resolved != m_format)
        {
            // Nothing to DO here, and that is by construction rather than by
            // luck: the attachment format is part of NriPipelineCache's
            // GraphicsKey, so the tonemap's next GetGraphics is a cache MISS
            // and creates a pipeline for the new format -- a stale PSO can
            // never be served. The old entries are released with the rest of
            // the cache at teardown. Still WARNed, because a swapchain that
            // changes channel order mid-run is worth knowing about when a
            // capture suddenly looks wrong-hued.
            ARC_WARN("[nri-graph] swapchain format changed across a resize (was {}, now {}) -- "
                     "format-keyed pipelines will be rebuilt on the next frame", (int)m_format,
                     (int)resolved);
            m_format = resolved;
        }
    }

    bool NriGraphContext::EnsureCaptureBuffer()
    {
        const std::uint32_t width  = m_swap->Width();
        const std::uint32_t height = m_swap->Height();
        if (width == 0 || height == 0)
        {
            ARC_WARN("[nri-graph] capture requested on a zero-sized surface");
            return false;
        }
        if (m_capture && m_captureWidth == width && m_captureHeight == height)
            return true;

        const nri::CoreInterface& core = m_device->Core();

        // Unreachable today -- Resize() is the only thing that can change the
        // swapchain extent and it already drops the staging buffer -- but a
        // silent leak is the wrong failure mode for "unreachable", and the
        // burial costs one line.
        if (m_capture)
        {
            const nri::CoreInterface* graveCore = &core;
            m_device->Graves().Bury(m_graph ? m_graph->DebugSubmitCount() : 0,
                                     [graveCore, b = m_capture] { graveCore->DestroyBuffer(b); });
            m_capture         = nullptr;
            m_captureRecorded = false;
        }

        // Both pitches carry NRI's documented alignment rules
        // (TextureDataLayoutDesc: rowPitch a multiple of
        // uploadBufferTextureRow, slicePitch of uploadBufferTextureSlice), and
        // the buffer is sized to the ALIGNED slice so the aligned values are
        // legal to hand to CmdReadbackTextureToBuffer as well.
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());
        m_captureWidth      = width;
        m_captureHeight     = height;
        m_captureRowPitch   = AlignUp(std::uint64_t(width) * 4,
                                       deviceDesc.memoryAlignment.uploadBufferTextureRow);
        m_captureSlicePitch = AlignUp(m_captureRowPitch * height,
                                       deviceDesc.memoryAlignment.uploadBufferTextureSlice);

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = m_captureSlicePitch;
        bufferDesc.usage = nri::BufferUsageBits::NONE;   // copy destination only, like Readback.cpp's
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_READBACK,
                                                       0.0f, bufferDesc, m_capture))
            || !m_capture)
        {
            ARC_ERROR("[nri-graph] capture staging buffer creation failed -- no capture this run");
            m_capture = nullptr;
            return false;
        }
        core.SetDebugName(m_capture, "nri-graph capture");
        return true;
    }

    RgFrameHandles DeclareGraphFrame(RenderGraph& graph, const RgFrameShape& shape,
                                      NriGraphContext* context)
    {
        // ---------------------------------------------------------------
        // THE CLEAR SEAM (Task 7's decision; Task 8 is its first real user).
        //
        // Task 6's executor begins every raster pass with `nri::AttachmentDesc
        // = {}`, i.e. LoadOp::LOAD + StoreOp::STORE, and the graph's
        // declaration API carries no clear ops at all. So a node that wants a
        // cleared attachment has exactly two options: CmdClearAttachments
        // inside its own exec fn, or a new declarative clear-op on the
        // attachment declaration.
        //
        // THIS TAKES THE FIRST, on purpose:
        //   - It changes NO frozen API mid-phase. RgExecuteDesc,
        //     RenderGraphNodeContext and the attachment setters are contracts
        //     Tasks 8-12 are already written against; a declarative clear op
        //     is a real design (per-attachment load op + clear value, its
        //     interaction with the swapchain's fixed discarding entry state,
        //     depth/stencil planes, and the compile-time validation that a
        //     CLEAR attachment is also declared as a Write) and deserves its
        //     own task rather than a rushed corner of this one.
        //   - It is correct here. The canvas is a TRANSIENT whose pool slot
        //     the graph hands over with a contents-discarding barrier, so its
        //     LOADed contents are undefined -- and Batch2DNode::Record clears
        //     it in full before drawing. The BACKBUFFER needs no clear at all:
        //     the tonemap covers it with a fullscreen triangle, and neither
        //     does a POST-CHAIN target, for the same reason (an opaque
        //     fullscreen pass writes every pixel of it).
        //   - It costs nothing measurable at Phase 2's scale: one full-target
        //     clear per frame on an immediate-mode desktop GPU is what
        //     LoadOp::CLEAR lowers to anyway.
        // WHEN TO REVISIT: tile-based hardware (where LoadOp::CLEAR genuinely
        // avoids a tile read) or a node that needs a partial/multi-attachment
        // clear pattern CmdClearAttachments makes awkward. Both are a Phase
        // 4+ concern, and both want the declarative version -- so this is a
        // deferral, not a rejection.
        //
        // RING FOOTGUN, for whoever adds the next node here: the vehicle calls
        // ring.BeginFrame(slot) AFTER this function returns, so an allocation
        // made from a SETUP fn lands in the PREVIOUS frame's slot and is
        // overwritten while the GPU still reads it. Allocate from the ring only
        // inside exec fns (record time). Batch2DNode does.
        // ---------------------------------------------------------------
        RgFrameHandles handles;

        handles.canvas = AddBatch2DNode(graph, context, shape.canvasWidth, shape.canvasHeight);

        // ---------------------------------------------------------------
        // THE POST CHAIN (Task 10). One node per chain pass, between the
        // canvas and the tonemap; the tonemap then samples the LAST pass's
        // target instead of the canvas. Everything about the ping-pong -- two
        // physical textures for N logical targets -- is the transient pool
        // allocator's doing, not a declaration here (FullscreenNodes.hpp, THE
        // PING-PONG IS DERIVED).
        //
        // STAGE GATING, and it is no longer a no-op: `batch` means "batcher +
        // tonemap and nothing else", so it drops the chain even when the scene
        // binds one -- the SAME bypass RuntimeApp applies to the NVRHI path,
        // which is what lets a batch-stage golden compare the same content on
        // both recorders. `post` and `full` still render the same frame,
        // because the HUD (Task 12) does not exist on this path yet.
        // ---------------------------------------------------------------
        RgTexture sceneColor = handles.canvas;
        if (shape.stage != GoldenStage::Batch && shape.post && !shape.post->passes.empty())
        {
            // With a context the NODE decides: PrepareChain builds the
            // pipelines, the descriptor sets and the packed constants, and
            // returns 0 for a chain it cannot honour -- which this then
            // declares nothing for, rather than declaring passes whose exec fn
            // would leave their targets holding undefined pool contents.
            // Headlessly it is the wiring alone, clamped to the same cap.
            std::uint32_t passCount = (std::uint32_t)std::min<std::size_t>(
                shape.post->passes.size(), PostChainNode::kMaxPasses);
            if (context)
            {
                PostChainNode* node = context->PostChain();
                passCount = node ? node->PrepareChain(*shape.post, context->CurrentGlobals(),
                                                       kGraphCanvasFormat)
                                 : 0;
            }
            if (passCount > 0)
            {
                handles.post = AddPostChainNodes(graph, context, handles.canvas, *shape.post,
                                                  passCount, shape.canvasWidth, shape.canvasHeight);
                handles.postPassCount = passCount;
                sceneColor            = handles.post;
            }
        }

        handles.backbuffer = AddTonemapNode(graph, context, sceneColor);

        if (!shape.capture)
            return handles;

        // The capture node. A Copy node, so the executor opens no rendering
        // for it; the backbuffer's COLOR_ATTACHMENT -> COPY_SOURCE transition
        // and the final COPY_SOURCE -> PRESENT exit barrier are both DERIVED
        // from these declarations -- the graph path records no hand-written
        // barrier anywhere, which is the whole point of the port.
        //
        // The staging buffer is IMPORTED rather than created as a transient:
        // the graph realizes transients in MemoryLocation::DEVICE
        // (RealizePool), which can never be mapped. RgUsage::ReadbackHost is
        // the declaration that says "this buffer is the host's to read"; it
        // derives the same COPY_DESTINATION state CopyDst does, by design.
        //
        // Shared rather than captured by value for the same reason
        // AddTonemapNode's backbuffer is: the handle is minted inside this
        // node's own setup, which runs after both lambdas are constructed.
        auto captureHandle = std::make_shared<RgBuffer>();
        const RgTexture backbuffer = handles.backbuffer;
        graph.AddNode("capture", RenderGraph::NodeKind::Copy,
            [shape, backbuffer, captureHandle](RenderGraphBuilder& builder)
            {
                *captureHandle = builder.ImportBuffer("capture", shape.captureBuffer,
                                                       shape.captureBytes);
                builder.Read(backbuffer, RgUsage::CopySrc);
                builder.Write(*captureHandle, RgUsage::ReadbackHost);
            },
            [context, backbuffer, captureHandle](RenderGraphNodeContext& ctx)
            {
                if (!context)
                    return;   // headless declaration-shape drive

                nri::Texture* source = ctx.Resolve(backbuffer);
                nri::Buffer*  dest   = ctx.Resolve(*captureHandle);
                if (!source || !dest)
                {
                    GraphError("NriGraphContext: the capture node could not resolve its backbuffer "
                               "or staging buffer");
                    return;
                }

                nri::TextureDataLayoutDesc layout = {};
                layout.rowPitch   = (std::uint32_t)context->CaptureRowPitch();
                layout.slicePitch = (std::uint32_t)context->CaptureSlicePitch();

                nri::TextureRegionDesc region = {};
                region.width  = (nri::Dim_t)context->CaptureWidth();
                region.height = (nri::Dim_t)context->CaptureHeight();
                region.depth  = 1;

                ctx.core.CmdReadbackTextureToBuffer(ctx.cmd, *dest, layout, *source, region);
                context->NoteCaptureRecorded();
            });

        handles.capture = *captureHandle;
        return handles;
    }

    void NriGraphContext::BuildFrame(const FrameDesc& frame)
    {
        // Thin on purpose: the frame's SHAPE lives in DeclareGraphFrame so the
        // headless [nri] frame-shape cases drive the real declarations instead
        // of transcribing them (see that function's comment block). The handles
        // it returns belong to the nodes that use them -- each exec fn already
        // captured what it needs -- so nothing here holds a per-frame handle
        // that would go stale on the next Reset().
        RgFrameShape shape;
        shape.stage         = frame.stage;
        shape.capture       = frame.capture;
        shape.canvasWidth   = m_swap->Width();
        shape.canvasHeight  = m_swap->Height();
        shape.captureBuffer = m_capture;
        shape.captureBytes  = m_captureSlicePitch;
        shape.post          = frame.post;

        // NOTHING follows the declaration, and that is worth stating because
        // an earlier draft of this task needed something here.
        //
        // Two things change when a post chain appears, disappears or re-wires:
        // the TONEMAP'S SOURCE texture (the canvas without a chain, the last
        // pass's target with one), and the TRANSIENT POOL -- RealizePool
        // buries a slot on a shrink or a desc change, from inside Execute().
        // Both are now handled where they are actually observable, inside the
        // nodes at record time: one descriptor set PER FRAME SLOT makes a
        // changed source ordinary rather than a hazard, and
        // RenderGraph::PoolEpoch is how a node learns its cached views were
        // buried. See FullscreenNodes.hpp, SOURCE VIEWS AND THE POOL.
        //
        // What that replaced was a DeviceWaitIdle here whenever the declared
        // pass count moved -- a stall, and INCOMPLETE: the pass count is a
        // proxy, and a chain re-wired from a DAG to a pipe keeps its count
        // while changing which physical texture the tonemap ends up sampling.
        (void)DeclareGraphFrame(*m_graph, shape, this);
    }

    NriGraphContext::FrameOutcome NriGraphContext::RenderFrame(const FrameDesc& frame)
    {
        // ==============================================================
        // A ZERO-SIZED SURFACE IS ROUTINE, AND IT HAS TO BE CAUGHT HERE.
        // ==============================================================
        // FrameOutcome's own contract names "a zero-sized surface, a minimized
        // window" as SKIPPED -- routine, does not advance the frame counter,
        // does not end a --frames N run. NriSwapChain::AcquireNextTexture
        // implements exactly that skip, and until Task 8 it was reachable:
        // Task 7's frame declared no transients, so the first thing Execute()
        // did that could fail was the acquire.
        //
        // Task 8's frame declares the CANVAS, a transient sized to this
        // swapchain -- and NriSwapChain::Resize stores a 0x0 extent verbatim,
        // so a minimised window makes that a 0x0 transient. Execute() realizes
        // the pool BEFORE it acquires, and RealizePool REFUSES a zero extent
        // through the latched error seam (nri::Dim_t cannot express it and a
        // zero-dimension texture would create successfully as something else).
        // A latched refusal is FrameOutcome::Failed, which stops the run with a
        // nonzero exit code -- the opposite of routine, and it would have made
        // the acquire's own skip unreachable.
        //
        // So the guard belongs in front of the DECLARATION, not inside it: at
        // 0x0 there is no frame worth declaring at all.
        if (m_swap->Width() == 0 || m_swap->Height() == 0)
            return FrameOutcome::Skipped;

        // A capture frame needs its staging buffer sized to the CURRENT extent
        // before the graph is declared (the node imports the buffer, so it has
        // to exist first). A failure here degrades to an uncaptured frame
        // rather than a failed one -- the run's pixels are still valid, only
        // the artifact is lost, which is the same exit-3 class the NVRHI
        // path's own capture failure reports.
        FrameDesc effective = frame;
        if (effective.capture && !EnsureCaptureBuffer())
            effective.capture = false;

        // Cheap and expected per frame: clears DECLARATIONS only. The
        // transient pool, the cached attachment views, the command slots and
        // the submission fence all survive, so a steady-state frame costs zero
        // GPU allocations (RenderGraph::Reset's contract).
        m_graph->Reset();

        // Published for exactly the span of this frame's declaration: the batch
        // node drains it from its declarator (AddBatch2DNode -> CurrentBatch),
        // and nothing may reach a stale batcher afterwards.
        //
        // The GLOBALS are copied rather than published-and-cleared, because
        // unlike the batcher they are read at RECORD time (the post chain
        // writes b1 inside its exec fn) -- see CurrentGlobals().
        m_currentGlobals = effective.globals ? *effective.globals : GlobalParams{};
        m_currentBatch = effective.batch;
        BuildFrame(effective);
        m_currentBatch = nullptr;

        std::string error;
        const std::optional<RgCompiled> compiled = m_graph->Compile(&error);
        if (!compiled)
        {
            GraphError("NriGraphContext: the frame did not compile -- " + error);
            return FrameOutcome::Failed;
        }

        // Which command-buffer / ring slot this frame records into. Advanced
        // only on a PRESENTED frame, so it stays in lockstep with
        // NriSwapChain's own frame counter -- which is what makes the pacing
        // wait inside AcquireNextTexture the proof that this slot is safe to
        // reset (RgExecuteDesc::frameSlot's caller contract).
        const std::uint32_t slot = (std::uint32_t)(m_frameIndex % kSwapchainFramesInFlight);
        m_ring.BeginFrame(slot);   // the caller owes this before Execute()

        const RgExecuteDesc desc{ *m_device, m_swap.get(), m_ring, m_pipelines, slot };

        // Execute() returns false for TWO different things and the frame
        // driver must treat them differently: a skipped acquire (routine --
        // zero-sized surface or an OUT_OF_DATE awaiting a Resize; deliberately
        // NOT latched, see RenderGraphExec) and a real failure (always
        // latched, through ARC_NRI_CHECK or the "nri-graph" seam). The latch
        // is therefore the discriminator -- there is no other observable, and
        // inventing a second return channel on a frozen signature would have
        // been worse.
        const std::uint64_t errorsBefore = RenderErrorCount();
        if (!m_graph->Execute(desc, *compiled))
            return RenderErrorCount() > errorsBefore ? FrameOutcome::Failed : FrameOutcome::Skipped;

        ++m_frameIndex;

        // ~5s heartbeat, open-ended runs only (header: m_heartbeat). Deliberately
        // AFTER the present, so "alive" means a frame actually reached the
        // screen, and it reports the latch too -- "alive and clean" and "alive
        // but latching errors" are different desk answers and the drag-storm
        // gave the user neither.
        if (m_heartbeat)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastHeartbeat >= std::chrono::seconds(5))
            {
                m_lastHeartbeat = now;
                const std::uint64_t errors = RenderErrorCount();
                ARC_INFO("[nri-graph] alive: {} frame(s) presented, {} error(s) latched since the "
                         "vehicle started", m_frameIndex,
                         errors > m_errorBaseline ? errors - m_errorBaseline : 0);
            }
        }

        return FrameOutcome::Presented;
    }

    bool NriGraphContext::ReadCapture(std::uint32_t& width, std::uint32_t& height,
                                       std::vector<unsigned char>& rgba)
    {
        width = height = 0;
        rgba.clear();

        if (!m_captureRecorded || !m_capture)
        {
            ARC_ERROR("[nri-graph] capture requested but no frame recorded one "
                      "(a capture is taken on the LAST frame -- it needs --frames N, and the run "
                      "must reach that frame)");
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        // The copy was recorded into the frame that was just submitted and
        // presented; it has to have LANDED before the mapped bytes mean
        // anything. Same stall the NVRHI path's own capture pays, on a frame
        // the process is about to exit after.
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        const auto* mapped = static_cast<const std::uint8_t*>(
            core.MapBuffer(*m_capture, 0, nri::WHOLE_SIZE));
        if (!mapped)
        {
            ARC_ERROR("[nri-graph] MapBuffer on the capture buffer returned null");
            return false;
        }

        // Tight RGBA8 out, whatever came in: the row pitch is dropped and BGRA
        // is swizzled. THIS is the Format()-aware half of the golden rule --
        // NRI cannot be told to give us BGRA8, so the normalization has to
        // happen here, and then the byte-wise comparator can hold a graph
        // capture against an NVRHI baseline.
        const bool swizzle = IsBgraFormat(m_format);
        rgba.resize(static_cast<std::size_t>(m_captureWidth) * m_captureHeight * 4);
        for (std::uint32_t y = 0; y < m_captureHeight; ++y)
        {
            const std::uint8_t* src = mapped + static_cast<std::size_t>(y * m_captureRowPitch);
            unsigned char* dst = rgba.data() + static_cast<std::size_t>(y) * m_captureWidth * 4;
            std::memcpy(dst, src, static_cast<std::size_t>(m_captureWidth) * 4);
            if (swizzle)
            {
                for (std::uint32_t x = 0; x < m_captureWidth; ++x)
                {
                    unsigned char* p = dst + static_cast<std::size_t>(x) * 4;
                    const unsigned char b = p[0];
                    p[0] = p[2];
                    p[2] = b;
                }
            }
        }
        core.UnmapBuffer(*m_capture);

        width  = m_captureWidth;
        height = m_captureHeight;
        return true;
    }
}
