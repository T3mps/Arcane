// SCAFFOLDING -- see NriSmoke.hpp for what this is, when it gets deleted, the
// two desk commands, and the NRISamples (MIT) attribution.
//
// Same include-order rule as NriDevice.cpp / NriSwapChain.cpp / NriCommon.hpp
// (nri::Message::ERROR vs wingdi.h's ERROR macro) -- NRI headers stay first.
#include <NRI.h>
#include <Extensions/NRISwapChain.h>

#include "NriSmoke.hpp"

#include "NriCommon.hpp"
#include "NriDevice.hpp"
#include "NriSwapChain.hpp"

#include <Arcane/Assets/ImageIo.hpp>       // WritePngRgba (--screenshot)
#include <Arcane/Base/Diagnostics.hpp>     // Diagnostics::Heartbeat -- the hang watchdog's liveness signal
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>        // RenderDeviceDesc, RenderErrorCount, ToString(GraphicsBackend)
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry/kPsEntry/kVsProfile/kPsProfile

#include <SDL3/SDL_timer.h>

// wingdi.h (via spdlog -> windows.h, dragged in by Arcane/Base/Log.hpp and
// Diagnostics.hpp) unconditionally #defines ERROR. Undefine it after the last
// header that could define it, before any further code in this file.
#undef ERROR

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace Arcane
{
    namespace
    {
        // Documented on NriSmoke::Run -- a scripted desk run reads these.
        constexpr int kExitOk                = 0;
        constexpr int kExitSetupFailed       = 1;
        constexpr int kExitValidationErrors  = 2;
        constexpr int kExitScreenshotFailed  = 3;

        // Same 1ms SDL_DelayNS primitive NriSwapChain's pacing wait and both
        // hosts' minimized-window paths use -- NOT std::this_thread::sleep_for,
        // whose Windows quantum is ~15.6ms.
        constexpr Uint64 kIdleSleepNs = 1'000'000;

        // The clear colour is deliberately the engine's own canvas clear
        // (RuntimeApp::MainLoop's clearTextureFloat) so a desk screenshot from
        // this path and one from the NVRHI path share a background and the eye
        // can compare them directly.
        constexpr float kClearColor[4] = { 0.02f, 0.02f, 0.04f, 1.0f };

        // -----------------------------------------------------------------
        // The shader pair. ONE HLSL source, two entry points, compiled through
        // the engine's own in-process dxc service (ShaderCompiler::CompileNow),
        // which dual-targets DXIL and SPIR-V from the same text -- no new
        // shader infrastructure, no prebaked blobs, no files on disk.
        //
        // VERTEX PULLING, not a vertex buffer: SV_VertexID indexes two static
        // const arrays baked into the shader. That is the simplest shape NRI
        // supports for a coloured triangle -- it removes the vertex/index
        // buffer, its memory allocation, its upload, its upload barriers and
        // the whole VertexInputDesc from the pipeline (GraphicsPipelineDesc::
        // vertexInput is NriOptional, so it stays null). NRISamples' Triangle
        // sample uses a real vertex buffer because it also textures the
        // triangle; nothing here samples anything, so none of that machinery
        // has to exist.
        //
        // Clip space is D3D convention on BOTH backends: NRI's VK
        // CmdSetViewports flips the viewport for the default
        // Viewport::originBottomLeft == false (Source/VK/CommandBufferVK.hpp,
        // "Origin top-left requires flipping"), so this source needs no
        // per-backend Y handling and the triangle points the same way in both
        // screenshots.
        // -----------------------------------------------------------------
        constexpr const char* kTriangleHlsl = R"HLSL(
struct VsOut
{
    float4 position : SV_Position;
    float3 color    : COLOR;
};

static const float2 kPositions[3] =
{
    float2( 0.0f,  0.6f),   // apex, top-centre
    float2( 0.6f, -0.6f),   // bottom-right
    float2(-0.6f, -0.6f),   // bottom-left
};

static const float3 kColors[3] =
{
    float3(1.0f, 0.0f, 0.0f),
    float3(0.0f, 1.0f, 0.0f),
    float3(0.0f, 0.0f, 1.0f),
};

VsOut vs_main(uint vertexId : SV_VertexID)
{
    VsOut output;
    output.position = float4(kPositions[vertexId], 0.0f, 1.0f);
    output.color    = kColors[vertexId];
    return output;
}

float4 ps_main(VsOut input) : SV_Target
{
    return float4(input.color, 1.0f);
}
)HLSL";

        void LogShaderDiags(const char* stage, const std::vector<ShaderDiag>& diags)
        {
            for (const ShaderDiag& d : diags)
            {
                if (d.severity == ShaderDiagSeverity::Error)
                    ARC_ERROR("[nri-smoke] {}: {}:{}:{}: {}", stage, d.file, d.line, d.col, d.message);
                else
                    ARC_WARN("[nri-smoke] {}: {}:{}:{}: {}", stage, d.file, d.line, d.col, d.message);
            }
        }

        // Compiles one stage and hands back the bytecode for THIS backend's
        // target. Empty on failure (already logged).
        std::vector<std::uint8_t> CompileStage(ShaderCompiler& compiler, GraphicsBackend backend,
                                               const char* entry, const char* profile,
                                               const char* stageLabel)
        {
            ShaderCompileRequest req;
            req.debugName  = "nri-smoke-triangle.hlsl";
            req.sourceUtf8 = kTriangleHlsl;
            req.entry      = entry;
            req.profile    = profile;

            const ShaderCompileResult result = compiler.CompileNow(req);
            const ShaderTargetResult& target =
                (backend == GraphicsBackend::Vulkan) ? result.spirv : result.dxil;

            LogShaderDiags(stageLabel, target.diags);
            if (!target.succeeded || target.bytecode.empty())
            {
                ARC_ERROR("[nri-smoke] {} compile failed for {} ({}). Repro: {}",
                          stageLabel, ToString(backend),
                          result.crashed ? "compiler crashed"
                                         : (result.environmental ? "toolchain failure" : "source errors"),
                          target.reproCmdLine);
                return {};
            }
            return target.bytecode;
        }

        // The swapchain format is RESOLVED by NRI, never pinned (NriSwapChain::
        // Format()'s comment: BT709_G22_8BIT maps to RGBA8 on D3D12 and VK ranks
        // RGBA8 above BGRA8, but neither is guaranteed). WritePngRgba wants
        // RGBA, so a BGRA backbuffer has to be swizzled on the way out --
        // exactly the check Readback.cpp performs on its mapped pixel.
        bool IsBgraFormat(nri::Format format)
        {
            return format == nri::Format::BGRA8_UNORM || format == nri::Format::BGRA8_SRGB;
        }

        std::uint64_t AlignUp(std::uint64_t value, std::uint32_t alignment)
        {
            return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
        }

        // One recording slot per frame in flight (the shape NRISamples calls
        // QueuedFrame). kSwapchainFramesInFlight of them, recycled by frame
        // index, safe to reset once NriSwapChain::AcquireNextTexture's pacing
        // wait has returned for this frame.
        struct FrameSlot
        {
            nri::CommandAllocator* allocator = nullptr;
            nri::CommandBuffer*    cmd       = nullptr;
        };

        // A COLOR_ATTACHMENT view per swapchain texture. NriSwapChain
        // deliberately creates none (its header: texture views are frame-graph
        // machinery, not swapchain-wrapper machinery), so the smoke makes its
        // own, lazily, keyed by the nri::Texture* the acquire handed back.
        struct AttachmentView
        {
            nri::Texture*    texture = nullptr;
            nri::Descriptor* view    = nullptr;
        };

        // -----------------------------------------------------------------
        // Everything the smoke creates on the NRI device, with its teardown.
        //
        // Declared AFTER the swapchain in Run() so it destructs BEFORE it --
        // which is what the attachment views need (a VkImageView outliving its
        // VkSwapchainKHR is a validation error, and NriSwapChain's textures die
        // with the swapchain).
        //
        // The split in ~Resources is deliberate and is the smoke's only use of
        // the Graveyard:
        //   - attachment views are destroyed DIRECTLY, because their lifetime
        //     is bounded above by the swapchain, which is destroyed before the
        //     graveyard ever drains (~NriDevice drains it, and the swapchain is
        //     gone by then).
        //   - everything else is BURIED on the device's graveyard, which is the
        //     sanctioned way for ordinary engine code to hand an NRI Destroy*
        //     back (Graveyard.hpp). ~NriDevice does the DeviceWaitIdle and the
        //     Drain, honouring the graveyard's "caller has already made the GPU
        //     idle" precondition.
        // -----------------------------------------------------------------
        struct Resources
        {
            explicit Resources(NriDevice& d) : device(&d) {}
            Resources(const Resources&)            = delete;
            Resources& operator=(const Resources&) = delete;

            NriDevice*             device         = nullptr;
            nri::PipelineLayout*   pipelineLayout = nullptr;
            nri::Pipeline*         pipeline       = nullptr;
            nri::Buffer*           readback       = nullptr;
            std::vector<FrameSlot> frames;
            std::vector<AttachmentView> views;

            // Also called from the resize path, where the swapchain textures
            // these views name are about to be destroyed. The caller is
            // responsible for the GPU being idle first.
            void DestroyAttachmentViews()
            {
                const nri::CoreInterface& core = device->Core();
                for (AttachmentView& v : views)
                    if (v.view) core.DestroyDescriptor(v.view);
                views.clear();
            }

            ~Resources()
            {
                const nri::CoreInterface& core = device->Core();

                // The frame loop's last submit may still be in flight. Idle
                // before touching anything it referenced -- ~NriDevice will
                // idle again before draining, which is harmless.
                if (core.DeviceWaitIdle)
                    (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&device->Device()));

                DestroyAttachmentViews();

                // Buried against a single fence value: Graveyard requires
                // nondecreasing burial order, and one burst at one value
                // trivially satisfies it. The value itself is irrelevant here
                // -- Drain() (the teardown path ~NriDevice takes) runs every
                // thunk regardless of fence value.
                Graveyard& graves = device->Graves();
                constexpr std::uint64_t kTeardownFence = 0;

                for (FrameSlot& slot : frames)
                {
                    if (slot.cmd)
                        graves.Bury(kTeardownFence, [&core, cmd = slot.cmd] { core.DestroyCommandBuffer(cmd); });
                    if (slot.allocator)
                        graves.Bury(kTeardownFence, [&core, a = slot.allocator] { core.DestroyCommandAllocator(a); });
                }
                frames.clear();

                if (readback)
                    graves.Bury(kTeardownFence, [&core, b = readback] { core.DestroyBuffer(b); });
                if (pipeline)
                    graves.Bury(kTeardownFence, [&core, p = pipeline] { core.DestroyPipeline(p); });
                if (pipelineLayout)
                    graves.Bury(kTeardownFence, [&core, l = pipelineLayout] { core.DestroyPipelineLayout(l); });
            }
        };

        // Lazily creates (and caches) the COLOR_ATTACHMENT view for an acquired
        // swapchain texture. Null on failure (already logged + latched).
        nri::Descriptor* ViewFor(Resources& res, nri::Texture* texture, nri::Format format)
        {
            for (const AttachmentView& v : res.views)
                if (v.texture == texture)
                    return v.view;

            nri::TextureViewDesc desc = {};
            desc.texture = texture;
            desc.type    = nri::TextureView::COLOR_ATTACHMENT;
            desc.format  = format;

            nri::Descriptor* view = nullptr;
            if (!ARC_NRI_CHECK(res.device->Core().CreateTextureView(desc, view)) || !view)
            {
                ARC_ERROR("[nri-smoke] CreateTextureView(COLOR_ATTACHMENT) failed");
                return nullptr;
            }
            res.views.push_back({ texture, view });
            return view;
        }

        // Maps the readback buffer and writes the PNG. True on success.
        bool WriteScreenshot(const nri::CoreInterface& core, nri::Buffer& readback,
                             const std::string& path, std::uint32_t width, std::uint32_t height,
                             std::uint64_t rowPitch, bool swizzleBgra)
        {
            const auto* mapped = static_cast<const std::uint8_t*>(
                core.MapBuffer(readback, 0, nri::WHOLE_SIZE));
            if (!mapped)
            {
                ARC_ERROR("[nri-smoke] MapBuffer on the readback buffer returned null");
                return false;
            }

            std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4);
            for (std::uint32_t y = 0; y < height; ++y)
            {
                const std::uint8_t* src = mapped + static_cast<std::size_t>(y * rowPitch);
                unsigned char* dst = rgba.data() + static_cast<std::size_t>(y) * width * 4;
                std::memcpy(dst, src, static_cast<std::size_t>(width) * 4);
                if (swizzleBgra)
                {
                    for (std::uint32_t x = 0; x < width; ++x)
                    {
                        unsigned char* p = dst + static_cast<std::size_t>(x) * 4;
                        const unsigned char b = p[0];
                        p[0] = p[2];
                        p[2] = b;
                    }
                }
            }
            core.UnmapBuffer(readback);

            if (!WritePngRgba(path, width, height, rgba.data()))
            {
                ARC_ERROR("[nri-smoke] screenshot write FAILED: {}", path);
                return false;
            }
            ARC_INFO("[nri-smoke] screenshot written: {} ({}x{})", path, width, height);
            return true;
        }
        // The whole session: window, device, wrap, swapchain, frame loop,
        // screenshot, and the teardown of all of it. File-local because
        // NriSmoke::Run BRACKETS it with the RenderErrorCount latch -- this
        // function's scope is what destroys every NRI object, and the latch has
        // to be read after that has happened (see Run()).
        int RunSession(const HostConfig& config)
        {
            ARC_INFO("[nri-smoke] starting: backend={} frames={} vsync={} screenshot='{}'",
                     ToString(config.backend),
                     config.maxFrames,
                     config.vsync ? "on" : "off",
                     config.screenshotPath);

            // -------------------------------------------------------------
            // Window. Same shape GpuContext::Create builds for the NVRHI boot
            // (WindowDesc defaults: 1280x720, resizable) with two deliberate
            // differences: its own title, and NOT hidden -- GpuContext hides
            // its window until the boot sequence's first presented frame, and
            // the smoke has no boot sequence and no splash to hand the reveal
            // to. The Vulkan flag matters: SDL_WINDOW_VULKAN must be set for a
            // window a Vulkan surface will be created against.
            // -------------------------------------------------------------
            Window window;
            {
                WindowDesc wd;
                wd.title     = "Arcane NRI Smoke";
                wd.vulkan    = (config.backend == GraphicsBackend::Vulkan);
                wd.resizable = true;
                wd.hidden    = false;
                if (!window.Create(wd))
                {
                    ARC_ERROR("[nri-smoke] window create failed");
                    return kExitSetupFailed;
                }
            }

            // -------------------------------------------------------------
            // The creation half, with validation forced ON in Debug.
            //
            // Every channel a validation message can arrive through ends at
            // RenderErrorCount, which is what makes this run's exit code
            // meaningful:
            //   - VK core + sync validation -> DeviceVulkan.cpp's
            //     VkDebugCallback -> NvrhiMessageCallback -> the latch.
            //   - D3D12 debug layer -> DeviceD3D12.cpp's
            //     D3D12DebugLayerCallback (ID3D12InfoQueue1, contract item 12)
            //     -> the same latch. This one is why enableD3D12DebugLayer is
            //     forced true here: it defaults FALSE (Device.hpp -- the
            //     Nahimic-OSD fail-fast hazard), and with it off the D3D12
            //     half of the smoke would have no validation channel at all,
            //     which is precisely the weakness Task 7's report flagged
            //     about the [gpu] wrap smoke. DESK HAZARD, stated because it
            //     is the reason that flag defaults false: D3D12SDKLayers.dll
            //     raises RaiseFailFastException (0x87D) when third-party
            //     window hooks (Nahimic OSD and friends) are injected. If the
            //     dx12 smoke fail-fasts before drawing anything, check for an
            //     injected overlay FIRST -- that is a machine problem, not an
            //     NRI one.
            //   - NRI's own validation layer -> MakeNriCallbacks (NriDevice.cpp
            //     sets enableNRIValidation from enableValidation) -> the latch.
            // -------------------------------------------------------------
            RenderDeviceDesc dd;
            dd.backend = config.backend;
#if defined(ARCANE_DEBUG)
            dd.enableValidation      = true;
            dd.enableD3D12DebugLayer = true;
            dd.enableSyncValidation  = true;   // VK-only; see Device.hpp
#else
            // Release/Dist: leave RenderDeviceDesc's own defaults (validation
            // off) rather than forcing debug layers into an optimized build.
            // The plan asks for validation "in Debug config", and a Release
            // smoke is a performance/behaviour check, not a validation one --
            // its exit code still fails on any error the NRI callbacks report.
#endif

            std::unique_ptr<NativeDeviceOwner> native = NativeDeviceOwner::Create(dd);
            if (!native)
            {
                ARC_ERROR("[nri-smoke] native {} device creation failed", ToString(config.backend));
                return kExitSetupFailed;
            }

            // Destruction order (contract item 15) is encoded in declaration
            // order from here down: resources -> swapchain -> NriDevice ->
            // NativeDeviceOwner -> Window, i.e. the reverse of construction.
            std::unique_ptr<NriDevice> device = NriDevice::Wrap(*native);
            if (!device)
            {
                ARC_ERROR("[nri-smoke] wrapping the native device failed");
                return kExitSetupFailed;
            }

            std::unique_ptr<NriSwapChain> swap = NriSwapChain::Create(*device, window, config.vsync);
            if (!swap)
            {
                ARC_ERROR("[nri-smoke] swapchain create failed");
                return kExitSetupFailed;
            }

            Resources res(*device);
            const nri::CoreInterface& core = device->Core();

            // -------------------------------------------------------------
            // Shaders -> pipeline layout -> pipeline.
            // -------------------------------------------------------------
            const nri::Format swapFormat = swap->Format();
            if (swapFormat == nri::Format::UNKNOWN)
            {
                ARC_ERROR("[nri-smoke] the swapchain resolved no format (zero-sized window at create?)");
                return kExitSetupFailed;
            }

            std::vector<std::uint8_t> vsCode;
            std::vector<std::uint8_t> psCode;
            {
                // Scoped: the compile service owns a worker thread and a
                // dxcompiler instance, neither of which the frame loop needs.
                ShaderCompiler compiler;
                if (!compiler.Initialize(/*debounceSeconds=*/0.0) || !compiler.IsAvailable())
                {
                    ARC_ERROR("[nri-smoke] dxcompiler.dll unavailable -- cannot compile the "
                              "triangle shaders (it is postbuild-copied beside the exe; run "
                              "from the exe's own directory)");
                    return kExitSetupFailed;
                }
                vsCode = CompileStage(compiler, config.backend, kVsEntry, kVsProfile, "vs");
                psCode = CompileStage(compiler, config.backend, kPsEntry, kPsProfile, "ps");
            }
            if (vsCode.empty() || psCode.empty())
                return kExitSetupFailed;

            {
                // No descriptor sets, no root constants, no root samplers: the
                // triangle reads nothing. shaderStages still has to name the
                // two stages the layout is used by.
                nri::PipelineLayoutDesc layoutDesc = {};
                layoutDesc.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
                if (!ARC_NRI_CHECK(core.CreatePipelineLayout(device->Device(), layoutDesc, res.pipelineLayout))
                    || !res.pipelineLayout)
                {
                    ARC_ERROR("[nri-smoke] CreatePipelineLayout failed");
                    return kExitSetupFailed;
                }
            }

            {
                nri::ShaderDesc shaders[2] = {};
                shaders[0].stage          = nri::StageBits::VERTEX_SHADER;
                shaders[0].bytecode       = vsCode.data();
                shaders[0].size           = vsCode.size();
                shaders[0].entryPointName = kVsEntry;   // SPIR-V needs it by name (PipelineVK.hpp); DXIL ignores it
                shaders[1].stage          = nri::StageBits::FRAGMENT_SHADER;
                shaders[1].bytecode       = psCode.data();
                shaders[1].size           = psCode.size();
                shaders[1].entryPointName = kPsEntry;

                nri::ColorAttachmentDesc colorAttachment = {};
                colorAttachment.format         = swapFormat;
                colorAttachment.colorWriteMask = nri::ColorWriteBits::RGBA;
                // blendEnabled stays false: the triangle is opaque, and the
                // sample's alpha blend exists only for its transparency slider.

                nri::OutputMergerDesc outputMerger = {};
                outputMerger.colors   = &colorAttachment;
                outputMerger.colorNum = 1;

                nri::RasterizationDesc rasterization = {};
                rasterization.fillMode = nri::FillMode::SOLID;
                // NONE, like NRISamples' Triangle: with culling off, vertex
                // winding cannot turn the smoke's one triangle into an empty
                // screen that looks exactly like a broken device.
                rasterization.cullMode = nri::CullMode::NONE;

                nri::InputAssemblyDesc inputAssembly = {};
                inputAssembly.topology = nri::Topology::TRIANGLE_LIST;

                nri::GraphicsPipelineDesc pipelineDesc = {};
                pipelineDesc.pipelineLayout = res.pipelineLayout;
                pipelineDesc.vertexInput    = nullptr;   // vertex pulling; see kTriangleHlsl
                pipelineDesc.inputAssembly  = inputAssembly;
                pipelineDesc.rasterization  = rasterization;
                pipelineDesc.outputMerger   = outputMerger;
                pipelineDesc.shaders        = shaders;
                pipelineDesc.shaderNum      = 2;

                if (!ARC_NRI_CHECK(core.CreateGraphicsPipeline(device->Device(), pipelineDesc, res.pipeline))
                    || !res.pipeline)
                {
                    ARC_ERROR("[nri-smoke] CreateGraphicsPipeline failed");
                    return kExitSetupFailed;
                }
            }

            // -------------------------------------------------------------
            // Recording slots: one per frame in flight, recycled by frame
            // index (NRISamples' QueuedFrame shape).
            // -------------------------------------------------------------
            res.frames.resize(kSwapchainFramesInFlight);
            for (FrameSlot& slot : res.frames)
            {
                // Short-circuit order matters: a failed CreateCommandAllocator
                // leaves slot.allocator null, and the null check has to sit
                // between the two calls -- CreateCommandBuffer takes it by
                // REFERENCE.
                if (!ARC_NRI_CHECK(core.CreateCommandAllocator(*device->GraphicsQueue(), slot.allocator))
                    || !slot.allocator
                    || !ARC_NRI_CHECK(core.CreateCommandBuffer(*slot.allocator, slot.cmd))
                    || !slot.cmd)
                {
                    ARC_ERROR("[nri-smoke] command allocator/buffer creation failed");
                    return kExitSetupFailed;
                }
            }

            // -------------------------------------------------------------
            // Frame loop.
            // -------------------------------------------------------------
            // Anything the hang watchdog reports from here on belongs to the
            // smoke's loop, not to whatever phase the last Install() named --
            // same call both hosts' MainLoop makes for the same reason.
            Diagnostics::SetPhase("nri smoke frame loop");

            const bool wantScreenshot = !config.screenshotPath.empty();
            bool screenshotOk = true;
            std::uint64_t frameIndex = 0;
            bool running = true;

            // Readback staging, sized on the frame that actually needs it (the
            // swapchain extent is only final once no more resizes can land).
            std::uint64_t readbackRowPitch   = 0;
            std::uint64_t readbackSlicePitch = 0;
            std::uint32_t readbackWidth      = 0;
            std::uint32_t readbackHeight     = 0;
            bool          readbackRecorded   = false;

            while (running)
            {
                // FIRST statement in the frame, before anything can block --
                // mirrors both hosts' MainLoop.
                Diagnostics::Heartbeat();

                // Window events, and therefore Resize(), are handled HERE and
                // only here: NriSwapChain's caller contract forbids a Resize()
                // between AcquireNextTexture() and Present() (debug ARC_ASSERT,
                // release ARC_WARN + a dangling backbuffer), so the resize must
                // sit at the frame boundary, above the acquire.
                const WindowEvents events = window.PumpEvents();
                if (events.quitRequested)
                    break;
                if (events.resized)
                {
                    // Our attachment views name swapchain textures the resize
                    // is about to destroy, so they have to go first -- and the
                    // GPU has to be idle before either. NriSwapChain::Resize
                    // does its own QueueWaitIdle internally, but that happens
                    // AFTER this point, so the wait here is not redundant.
                    // TEMPORARY, like everything else in this file: Phase 2's
                    // frame graph owns view lifetimes and will not idle the
                    // queue to rebuild them.
                    (void)ARC_NRI_CHECK(core.QueueWaitIdle(device->GraphicsQueue()));
                    res.DestroyAttachmentViews();
                    swap->Resize(events.width, events.height);

                    // The pipeline was compiled against one attachment format.
                    // A rebuild that resolved a different one would draw
                    // through an incompatible PSO; say so instead of producing
                    // mystery corruption. (UNKNOWN just means "no swapchain
                    // right now" -- minimized; the acquire below skips.)
                    if (swap->Format() != nri::Format::UNKNOWN && swap->Format() != swapFormat)
                    {
                        ARC_ERROR("[nri-smoke] swapchain format changed across a resize "
                                  "(was {}, now {}) -- the pipeline is no longer valid",
                                  (int)swapFormat, (int)swap->Format());
                        break;
                    }
                }
                if (window.IsMinimized())
                {
                    SDL_DelayNS(kIdleSleepNs);
                    continue;
                }

                nri::Texture* backbuffer = swap->AcquireNextTexture();
                if (!backbuffer)
                {
                    // Zero-sized surface or an OUT_OF_DATE acquire -- both
                    // already logged by NriSwapChain, both self-healing on the
                    // next resize event. frameIndex deliberately does NOT
                    // advance: NriSwapChain's own frame counter does not
                    // either (Present() is a no-op with nothing acquired), and
                    // the two must stay in lockstep for the command-allocator
                    // recycling below to be safe.
                    SDL_DelayNS(kIdleSleepNs);
                    continue;
                }

                // Both bail-outs below leave the acquire OUTSTANDING, deliberately.
                // NriSwapChain::Present() hands the release fence to QueuePresent,
                // and that fence is signalled by the frame's own submission --
                // which is exactly the thing that did not happen here. Presenting
                // anyway would park the present engine on a fence nothing will
                // ever signal. Breaking instead leaves an un-presented acquire for
                // ~NriSwapChain to clean up behind its QueueWaitIdle: untidy, but
                // an untidy teardown on an already-failing path beats a hang.
                nri::Descriptor* colorView = ViewFor(res, backbuffer, swapFormat);
                if (!colorView)
                    break;

                const std::uint32_t width  = swap->Width();
                const std::uint32_t height = swap->Height();
                const bool lastFrame = (config.maxFrames != 0) && (frameIndex + 1 >= config.maxFrames);
                const bool captureThisFrame = lastFrame && wantScreenshot;

                // Readback staging for the capture frame. Created here, once,
                // because only now is the extent final.
                if (captureThisFrame && !res.readback)
                {
                    // Both pitches carry NRI's documented alignment rules
                    // (TextureDataLayoutDesc: rowPitch must be a multiple of
                    // uploadBufferTextureRow, slicePitch of
                    // uploadBufferTextureSlice) -- and the buffer is sized to
                    // the ALIGNED slice, so the aligned value is legal to hand
                    // to CmdReadbackTextureToBuffer as well.
                    const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(device->Device());
                    readbackWidth      = width;
                    readbackHeight     = height;
                    readbackRowPitch   = AlignUp(std::uint64_t(width) * 4,
                                                 deviceDesc.memoryAlignment.uploadBufferTextureRow);
                    readbackSlicePitch = AlignUp(readbackRowPitch * height,
                                                 deviceDesc.memoryAlignment.uploadBufferTextureSlice);

                    nri::BufferDesc bufferDesc = {};
                    bufferDesc.size  = readbackSlicePitch;
                    bufferDesc.usage = nri::BufferUsageBits::NONE;   // copy destination only, like Readback.cpp's
                    if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(device->Device(),
                                                                  nri::MemoryLocation::HOST_READBACK,
                                                                  0.0f, bufferDesc, res.readback))
                        || !res.readback)
                    {
                        ARC_ERROR("[nri-smoke] readback buffer creation failed -- no screenshot");
                        screenshotOk = false;
                    }
                }
                const bool recordReadback = captureThisFrame && res.readback != nullptr;

                // The pacing wait inside AcquireNextTexture has already
                // established that the frame kSwapchainFramesInFlight back is
                // retired, so this slot's recording is safe to reset.
                const std::size_t slotIndex = static_cast<std::size_t>(frameIndex % kSwapchainFramesInFlight);
                FrameSlot& slot = res.frames[slotIndex];
                core.ResetCommandAllocator(*slot.allocator);

                nri::CommandBuffer& cmd = *slot.cmd;
                if (!ARC_NRI_CHECK(core.BeginCommandBuffer(cmd, nullptr)))
                    break;   // outstanding acquire, deliberately -- see the note above

                // One reused barrier pair for the whole frame, exactly as both
                // adapted samples do it: mutate `before`/`after` and re-issue.
                nri::TextureBarrierDesc textureBarrier = {};
                textureBarrier.texture  = backbuffer;
                textureBarrier.mipNum   = 1;
                textureBarrier.layerNum = 1;

                nri::BarrierDesc barrier = {};
                barrier.textureNum = 1;
                barrier.textures   = &textureBarrier;

                // TEMPORARY BARRIER (1/3) -- acquire -> colour attachment.
                // Phase 2's frame graph replaces every barrier in this file;
                // hand-written CmdBarrier outside the graph becomes a review
                // defect at the port's merge -- spec ratification 1.
                textureBarrier.after = { nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT };
                core.CmdBarrier(cmd, barrier);

                {
                    nri::AttachmentDesc colorAttachment = {};
                    colorAttachment.descriptor = colorView;

                    nri::RenderingDesc rendering = {};
                    rendering.colors   = &colorAttachment;
                    rendering.colorNum = 1;

                    core.CmdBeginRendering(cmd, rendering);
                    {
                        nri::ClearAttachmentDesc clear = {};
                        clear.planes               = nri::PlaneBits::COLOR;
                        clear.colorAttachmentIndex = 0;
                        clear.value.color.f = { kClearColor[0], kClearColor[1], kClearColor[2], kClearColor[3] };
                        core.CmdClearAttachments(cmd, &clear, 1, nullptr, 0);

                        // "Initial state (mandatory)" per NRI.h -- a pipeline
                        // is not allowed to inherit either of these.
                        const nri::Viewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
                        core.CmdSetViewports(cmd, &viewport, 1);
                        const nri::Rect scissor = { 0, 0, (nri::Dim_t)width, (nri::Dim_t)height };
                        core.CmdSetScissors(cmd, &scissor, 1);

                        core.CmdSetPipelineLayout(cmd, nri::BindPoint::GRAPHICS, *res.pipelineLayout);
                        core.CmdSetPipeline(cmd, *res.pipeline);

                        nri::DrawDesc draw = {};
                        draw.vertexNum   = 3;
                        draw.instanceNum = 1;
                        core.CmdDraw(cmd, draw);
                    }
                    core.CmdEndRendering(cmd);
                }

                if (recordReadback)
                {
                    // TEMPORARY BARRIER (2/3) -- colour attachment -> copy
                    // source, for the --screenshot readback.
                    // Phase 2's frame graph replaces every barrier in this
                    // file; hand-written CmdBarrier outside the graph becomes a
                    // review defect at the port's merge -- spec ratification 1.
                    textureBarrier.before = textureBarrier.after;
                    textureBarrier.after  = { nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE };
                    core.CmdBarrier(cmd, barrier);

                    nri::TextureDataLayoutDesc layout = {};
                    layout.rowPitch   = (std::uint32_t)readbackRowPitch;
                    layout.slicePitch = (std::uint32_t)readbackSlicePitch;

                    nri::TextureRegionDesc region = {};
                    region.width  = (nri::Dim_t)readbackWidth;
                    region.height = (nri::Dim_t)readbackHeight;
                    region.depth  = 1;

                    core.CmdReadbackTextureToBuffer(cmd, *res.readback, layout, *backbuffer, region);
                    readbackRecorded = true;
                }

                // TEMPORARY BARRIER (3/3) -- whatever the last state was ->
                // present. ONE site for both paths: `before` picks up the
                // previous `after` (COPY_SOURCE on a capture frame,
                // COLOR_ATTACHMENT otherwise), which is exactly the reused-
                // barrier idiom both adapted samples use.
                // Phase 2's frame graph replaces every barrier in this file;
                // hand-written CmdBarrier outside the graph becomes a review
                // defect at the port's merge -- spec ratification 1.
                textureBarrier.before = textureBarrier.after;
                textureBarrier.after  = { nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE };
                core.CmdBarrier(cmd, barrier);

                (void)ARC_NRI_CHECK(core.EndCommandBuffer(cmd));

                // Submit: wait on the acquire fence, signal the release fence.
                // NriSwapChain owns both but submits no real work itself
                // (NriSwapChain.hpp), so wiring them into a submission is the
                // caller's job -- this is that caller.
                {
                    nri::FenceSubmitDesc waitFence = {};
                    waitFence.fence  = swap->CurrentAcquireFence();
                    waitFence.stages = nri::StageBits::COLOR_ATTACHMENT;

                    nri::FenceSubmitDesc signalFence = {};
                    signalFence.fence = swap->CurrentReleaseFence();

                    nri::CommandBuffer* commandBuffers[] = { &cmd };

                    nri::QueueSubmitDesc submit = {};
                    submit.waitFences       = &waitFence;
                    submit.waitFenceNum     = 1;
                    submit.commandBuffers   = commandBuffers;
                    submit.commandBufferNum = 1;
                    submit.signalFences     = &signalFence;
                    submit.signalFenceNum   = 1;
                    (void)ARC_NRI_CHECK(core.QueueSubmit(*device->GraphicsQueue(), submit));
                }

                swap->Present();
                ++frameIndex;

                if (lastFrame)
                    running = false;
            }

            // -------------------------------------------------------------
            // --screenshot: map + write, once the capture frame's copy has
            // actually landed. Same last-frame/post-Present timing the NVRHI
            // host's own --screenshot uses (RuntimeApp::MainLoop), and the same
            // "this stalls the device" caveat -- which costs nothing on a
            // frame the process is about to exit after.
            // -------------------------------------------------------------
            if (wantScreenshot)
            {
                if (!readbackRecorded)
                {
                    if (screenshotOk)   // not already reported as a creation failure
                    {
                        // Two ways to land here, both worth naming: no --frames
                        // at all (the capture is taken on the LAST frame, and an
                        // unbounded run has none -- the same shape the NVRHI
                        // host's --screenshot has, which simply exits 0 having
                        // written nothing), or a window closed early.
                        ARC_ERROR("[nri-smoke] --screenshot requested but no frame was captured: "
                                  "{}", config.maxFrames == 0
                                            ? "--screenshot needs --frames N (the capture is the LAST frame)"
                                            : "the run ended before reaching the last frame");
                    }
                    screenshotOk = false;
                }
                else
                {
                    (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&device->Device()));
                    screenshotOk = WriteScreenshot(core, *res.readback, config.screenshotPath,
                                                   readbackWidth, readbackHeight, readbackRowPitch,
                                                   IsBgraFormat(swapFormat));
                }
            }

            ARC_INFO("[nri-smoke] session finished: {} frame(s) rendered", frameIndex);
            return screenshotOk ? kExitOk : kExitScreenshotFailed;
        }
    }

    namespace NriSmoke
    {
        int Run(const HostConfig& config)
        {
            // The gate latch, sampled BEFORE anything can trip it. The smoke
            // never calls ResetRenderErrorCount (production code must not --
            // Device.hpp); it compares against this baseline instead, so a
            // count already nonzero at entry does not become this run's
            // failure.
            const std::uint64_t errorBaseline = RenderErrorCount();

            // Read AFTER RunSession returns, not inside it: RunSession's own
            // scope is what destroys every NRI object, and teardown ordering is
            // exactly the class of mistake a validation layer exists to catch.
            // A run whose only error fires while releasing resources must still
            // fail -- reading the latch from inside would have missed it.
            const int sessionCode = RunSession(config);

            const std::uint64_t errorsNow = RenderErrorCount();
            ARC_INFO("[nri-smoke] RenderErrorCount {} -> {}", errorBaseline, errorsNow);

            if (errorsNow > errorBaseline)
            {
                ARC_ERROR("[nri-smoke] FAILED: {} validation/render error(s) fired during the run "
                          "(teardown included)", errorsNow - errorBaseline);
                // Precedence 1 > 2 > 3 (documented on the .hpp): a setup failure
                // says WHERE the run died, which outranks the errors it produced
                // on the way out; and a validation error explains a bad capture,
                // not the other way round.
                return (sessionCode == kExitSetupFailed) ? kExitSetupFailed : kExitValidationErrors;
            }
            return sessionCode;
        }
    }
}
