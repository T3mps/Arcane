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
#include <Arcane/Render/Swapchain.hpp>              // kSwapchainFramesInFlight

#undef ERROR

#include <chrono>
#include <cstring>
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

        // Deliberately the SAME colour RuntimeApp::MainLoop clears its canvas
        // to (clearTextureFloat, 0.02/0.02/0.04/1). A clear-only graph frame
        // and the NVRHI frame therefore share a background, so the first thing
        // a desk screenshot proves is that the graph reached the same surface
        // -- and a stage golden's background pixels match by construction
        // rather than by coincidence.
        constexpr float kClearColor[4] = { 0.02f, 0.02f, 0.04f, 1.0f };

        // Per-frame-slot upload arena. Nothing in Task 7's clear-only frame
        // allocates from it; it is sized here (and Init()'d at Create, so a
        // mapping failure is found at boot rather than inside Task 8's first
        // draw) for the sprite VB/IB + constants Task 8 puts through it. A
        // starting point, not a measurement: NriUploadRing::HighWater() is the
        // number to size it from once a real frame runs.
        constexpr std::uint64_t kUploadRingBytesPerFrame = 4ull * 1024 * 1024;

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
            // Every cached PSO baked the OLD attachment format (that is why
            // the format is in NriPipelineCache::GraphicsKey), so drawing
            // through them after this would be undefined. Nothing in Task 7's
            // clear-only frame owns a PSO yet, so this is a loud FYI rather
            // than a refusal -- Task 8 is the first task that must act on it.
            ARC_WARN("[nri-graph] swapchain format changed across a resize (was {}, now {}) -- "
                     "every format-keyed pipeline is now stale", (int)m_format, (int)resolved);
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

    void NriGraphContext::BuildFrame(const FrameDesc& frame)
    {
        // ---------------------------------------------------------------
        // THE CLEAR SEAM (Task 7's deliberate choice; Task 8 inherits it).
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
        //   - It is correct here. The graph pins an imported swapchain
        //     texture's ENTRY state to the discarding {NONE, UNDEFINED, ALL}
        //     (RenderGraphBuilder::ImportSwapChainTexture), so LOADing the
        //     backbuffer loads undefined content -- which this node then
        //     overwrites in full before anything reads it.
        //   - It costs nothing measurable at Phase 2's scale: one full-target
        //     clear per frame on an immediate-mode desktop GPU is what
        //     LoadOp::CLEAR lowers to anyway.
        // WHEN TO REVISIT: tile-based hardware (where LoadOp::CLEAR genuinely
        // avoids a tile read) or a node that needs a partial/multi-attachment
        // clear pattern CmdClearAttachments makes awkward. Both are a Phase
        // 4+ concern, and both want the declarative version -- so this is a
        // deferral, not a rejection.
        // ---------------------------------------------------------------
        (void)frame.stage;   // Tasks 8-12: the stage selects which nodes below run

        m_graph->AddNode("clear", RenderGraph::NodeKind::Raster,
            [this](RenderGraphBuilder& builder)
            {
                // NOT ImportTexture: the graph owns acquire/present sequencing
                // and there is no nri::Texture* to hand over at declaration
                // time -- Execute() resolves this to the texture it acquires.
                m_backbuffer = builder.ImportSwapChainTexture("backbuffer");
                builder.Write(m_backbuffer, RgUsage::ColorWrite);
                m_graph->SetColorAttachments(std::span<const RgTexture>(&m_backbuffer, 1));
            },
            [](RenderGraphNodeContext& ctx)
            {
                // The executor has already emitted this node's barriers and
                // opened rendering with the declared attachment, and set the
                // viewport + scissor to the full target -- an exec fn must not
                // do any of that itself (RenderGraphNodeContext's contract).
                nri::ClearAttachmentDesc clear = {};
                clear.planes               = nri::PlaneBits::COLOR;
                clear.colorAttachmentIndex = 0;
                clear.value.color.f = { kClearColor[0], kClearColor[1], kClearColor[2], kClearColor[3] };
                ctx.core.CmdClearAttachments(ctx.cmd, &clear, 1, nullptr, 0);
            });

        if (!frame.capture)
            return;

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
        m_graph->AddNode("capture", RenderGraph::NodeKind::Copy,
            [this](RenderGraphBuilder& builder)
            {
                m_captureHandle = builder.ImportBuffer("capture", m_capture, m_captureSlicePitch);
                builder.Read(m_backbuffer, RgUsage::CopySrc);
                builder.Write(m_captureHandle, RgUsage::ReadbackHost);
            },
            [this](RenderGraphNodeContext& ctx)
            {
                nri::Texture* source = ctx.Resolve(m_backbuffer);
                nri::Buffer*  dest   = ctx.Resolve(m_captureHandle);
                if (!source || !dest)
                {
                    GraphError("NriGraphContext: the capture node could not resolve its backbuffer "
                               "or staging buffer");
                    return;
                }

                nri::TextureDataLayoutDesc layout = {};
                layout.rowPitch   = (std::uint32_t)m_captureRowPitch;
                layout.slicePitch = (std::uint32_t)m_captureSlicePitch;

                nri::TextureRegionDesc region = {};
                region.width  = (nri::Dim_t)m_captureWidth;
                region.height = (nri::Dim_t)m_captureHeight;
                region.depth  = 1;

                ctx.core.CmdReadbackTextureToBuffer(ctx.cmd, *dest, layout, *source, region);
                m_captureRecorded = true;
            });
    }

    NriGraphContext::FrameOutcome NriGraphContext::RenderFrame(const FrameDesc& frame)
    {
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
        m_backbuffer    = RgTexture{};
        m_captureHandle = RgBuffer{};
        BuildFrame(effective);

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
