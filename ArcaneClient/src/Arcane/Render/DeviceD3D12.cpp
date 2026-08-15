// D3D12 backend: DXGI factory + adapter + device + direct queue, wrapped
// by nvrhi::d3d12::createDevice. Swapchain: DXGI flip-discard, 3 backbuffers.

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/DeviceCreationD3D12.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Arcane
{
    namespace
    {
        constexpr uint32_t kBackbufferCount = 3;
        constexpr DXGI_FORMAT kSwapchainFormatDxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
        constexpr nvrhi::Format kSwapchainFormat = nvrhi::Format::BGRA8_UNORM;

        // F-3: the ONE device-removed observation point. Both observables
        // funnel here -- F-3b's cross-backend NVRHI message hook (fires on
        // submit, the earliest signal) and F-3c's Present, the only
        // first-party DXGI_ERROR_DEVICE_REMOVED site in the tree.
        //
        // Once-only per armed device: a removed device keeps reporting removal
        // on every submit and every Present, and the second report is worthless
        // -- the marker buffer and DRED state belong to the FIRST one. Reset
        // when a new backend arms (project switch recreates the device).
        std::atomic<bool> g_deviceRemovedReported{ false };

        // ------------------------------------------------------------------
        // Process-global D3D12 debug-layer state (NRI Phase 2, D1 shakedown).
        // ------------------------------------------------------------------
        // ID3D12Debug::EnableDebugLayer is a BEFORE-ANY-DEVICE call, and the
        // documentation is explicit about what happens otherwise: "To enable
        // the debug layers using this API, it must be called before the D3D12
        // device is created. Calling this API after creating the D3D12 device
        // will cause the D3D12 runtime to remove the device."
        // (learn.microsoft.com, ID3D12Debug::EnableDebugLayer, Remarks.)
        //
        // That is exactly what the `--nri-graph` vehicle did on its first desk
        // run: the engine boots its NVRHI device with the layer OFF (the flag
        // defaults false -- Device.hpp, the Nahimic-OSD fail-fast hazard), then
        // the vehicle runs THIS SAME creation half a second time with
        // enableD3D12DebugLayer forced true, so EnableDebugLayer landed on a
        // process that already owned a live device. Every dx12 vehicle run then
        // failed at D3D12CreateDevice and exited 1 after 0 frames.
        //
        // These two flags make the sequencing structural rather than a rule
        // somebody has to remember at each new call site.
        //
        // MONOTONE ON PURPOSE, and the conservative direction: `g_d3d12DeviceCreated`
        // is never cleared, because the NVRHI owner (DeviceD3D12) releases its
        // device through MEMBER DESTRUCTION rather than DestroyD3D12NativeDevice,
        // so there is no single point that could decrement a live count without
        // moving that teardown -- and the failure mode of being wrong here is
        // asymmetric. Skipping an enable that would have been legal costs one
        // diagnostic channel; making the call when it is NOT legal removes a live
        // device. Nothing in the tree recreates a device WITH the layer requested
        // anyway (only --nri-graph ever sets the flag, and it never recreates
        // one), so today this costs nothing at all.
        std::atomic<bool> g_d3d12DeviceCreated{ false };
        std::atomic<bool> g_d3d12DebugLayerEnabled{ false };

        void ObserveDeviceRemoved()
        {
            if (g_deviceRemovedReported.exchange(true, std::memory_order_acq_rel))
                return;

            // The reason string is load-bearing: Diagnostics::DeriveKind
            // classifies the .arcdiag "kind" by case-sensitive substring, and
            // only a reason containing lowercase "gpu" resolves to a gpu kind
            // (here "gpu-crash", which is what makes the .gpudump sibling get
            // written). Do not reword.
            Diagnostics::WriteReport("gpu-crash: device removed");

            // AFTER the report, deliberately: hosts poll this latch and shut
            // down on it, and "observed" must always mean "the report exists".
            NoteGpuDeviceLost();
        }

        // Phase 2, Task 1 instrumentation. WHICH d3d12SDKLayers.dll is
        // servicing the debug layer is the one fact that separates the two
        // ways ID3D12InfoQueue1 can be missing, and it is observable only in
        // a live run -- hence logged at the failure site rather than assumed.
        //
        // Background, because the old WARN here guessed wrong: the vendored
        // Agility redistributable (ThirdParty/AgilitySDK 1.619.3, copied to
        // <exedir>/D3D12/ beside D3D12Core.dll) DOES implement the interface,
        // while the Windows 10 in-box layer (C:\Windows\System32\
        // d3d12SDKLayers.dll, 10.0.19041.x) does not carry it at all -- the
        // IID does not appear anywhere in that binary. So a failed QI means
        // the in-box layer answered, NOT that the D3D12 runtime is
        // "pre-Agility" (NRI logs "Using ID3D12Device15" in the same run,
        // which only the Agility runtime can satisfy).
        std::string LoadedD3D12SDKLayersModule()
        {
            const HMODULE module = GetModuleHandleW(L"d3d12SDKLayers.dll");
            if (!module)
                return "d3d12SDKLayers.dll not loaded";

            wchar_t wide[MAX_PATH]{};
            if (GetModuleFileNameW(module, wide, static_cast<DWORD>(std::size(wide))) == 0)
                return "d3d12SDKLayers.dll loaded, path unavailable";

            char narrow[MAX_PATH * 2]{};
            size_t converted = 0;
            wcstombs_s(&converted, narrow, wide, _TRUNCATE);
            return narrow;
        }

        // NRI capability contract item 12: the D3D12 debug layer's own channel
        // into our log and the RenderErrorCount latch.
        //
        // Why it has to be OURS: NRI never copies enableGraphicsAPIValidation
        // into its internal desc on the wrapper path, so its whole info-queue
        // block -- including ID3D12InfoQueue1::RegisterMessageCallback -- is
        // dead code for us, and its CallbackInterface carries NRI's own
        // messages only. Without this, D3D12 validation text reaches nothing:
        // the block below merely turns break-on-severity off. This is the one
        // channel by which a D3D12 VUID can fail the 0/0 gate, and it is the
        // exact counterpart of DeviceVulkan.cpp's VkDebugCallback -- same sink,
        // same severity split, so "an error happened" means one thing on both
        // backends.
        //
        // __stdcall by D3D12MessageFunc's typedef (d3d12sdklayers.h); the
        // calling convention must match exactly.
        void __stdcall D3D12DebugLayerCallback(D3D12_MESSAGE_CATEGORY /*category*/,
                                               D3D12_MESSAGE_SEVERITY severity,
                                               D3D12_MESSAGE_ID /*id*/,
                                               LPCSTR description,
                                               void* /*context*/)
        {
            const char* text = description ? description : "";
            switch (severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                // Same latch NVRHI's own errors increment, so a raw D3D12
                // message fails the GPU tests exactly like an [nvrhi] error --
                // but through NoteError, which tags it "[d3d12]" (this text is
                // the debug layer's, not NVRHI's) and skips the device-removed
                // substring hook. This callback runs on whatever thread tripped
                // the error, from inside a D3D12 call; ObserveDeviceRemoved
                // writes a report + minidump and belongs on NVRHI's submit-time
                // feed, which stays the ONE observation point (F-3b). See
                // NvrhiMessageCallback::NoteError for the full argument.
                NvrhiMessageCallback::Instance().NoteError("d3d12", text);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                ARC_WARN("[d3d12] {}", text);
                break;
            default:
                // INFO/MESSAGE: the debug layer emits one per resource create
                // and destroy. The Vulkan messenger subscribes to Error and
                // Warning only (DeviceVulkan.cpp) -- match it rather than
                // drown the log.
                break;
            }
        }

        class DeviceD3D12 final : public RenderDevice
        {
        public:
            ~DeviceD3D12() override;

            bool Init(const RenderDeviceDesc& desc);

            GraphicsBackend Backend() const override { return GraphicsBackend::D3D12; }
            nvrhi::IDevice* Nvrhi() const override { return m_nvrhi.Get(); }
            std::string AdapterName() const override { return m_creation.adapterName; }

            IDXGIFactory6* Factory() const { return m_creation.factory.Get(); }
            ID3D12CommandQueue* GraphicsQueue() const { return m_creation.graphicsQueue.Get(); }
            ID3D12Device* D3D12Device() const { return m_creation.device.Get(); }

            std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                       bool vsync) override;

        private:
            // Declaration order is destruction order in reverse: the nvrhi
            // device must release its D3D12 references before the queue,
            // device, adapter, and factory go away.
            //
            // Task 7: those five COM handles (plus the adapter name and the
            // info-queue registration) are the creation half now. It is FIRST
            // here, and its own member order reproduces the order they were
            // declared in -- factory, adapter, device, info queue, cookie,
            // queue -- so the release sequence this class always produced is
            // unchanged: nvrhi first, then the natives in that order.
            D3D12DeviceCreation m_creation;
            nvrhi::DeviceHandle m_nvrhi;
            // LAST on purpose, so it is destroyed FIRST: the crash backend
            // holds an ID3D12Heap + placed resource created off
            // m_creation.device, and those must release before the device does.
            std::unique_ptr<IGpuCrashBackend> m_crashBackend;
        };

        DeviceD3D12::~DeviceD3D12()
        {
            if (m_crashBackend)
            {
                // Symmetric with Init's install. All three slots are
                // process-wide and point INTO m_crashBackend, so they must be
                // emptied before the member below goes away. The pass-scope
                // slot clears conditionally: a second device that installed
                // after this one must keep its registration.
                Diagnostics::ClearGpuSectionProvider();
                // Wait out a report already mid-flight against this backend
                // (watchdog thread) before m_crashBackend is destroyed below
                // -- clearing the provider slot above only stops the NEXT
                // report from seeing it. See Diagnostics::FenceReports.
                Diagnostics::FenceReports();
                (void)ClearActiveGpuCrashBackendIfCurrent(m_crashBackend.get());
                NvrhiMessageCallback::Instance().SetDeviceRemovedHook(nullptr);
            }
            // Contract item 12, symmetric with Init's registration: the
            // debug layer holds a raw pointer to a function in this module.
            // Same call, same position -- unregistering here rather than with
            // the COM handles keeps the NVRHI device's own teardown messages
            // behaving exactly as they did before the creation-half split.
            UnregisterD3D12DebugCallback(m_creation);
        }

        bool DeviceD3D12::Init(const RenderDeviceDesc& desc)
        {
            // Everything above the NVRHI desc used to live inline here; it
            // is the creation half now, and this call is the ONLY thing
            // between the two versions of this function.
            if (!CreateD3D12NativeDevice(desc, m_creation))
                return false;

            nvrhi::d3d12::DeviceDesc nvrhiDesc;
            nvrhiDesc.errorCB = &NvrhiMessageCallback::Instance();
            nvrhiDesc.pDevice = m_creation.device.Get();
            nvrhiDesc.pGraphicsCommandQueue = m_creation.graphicsQueue.Get();
            m_nvrhi = nvrhi::d3d12::createDevice(nvrhiDesc);
            if (!m_nvrhi)
            {
                ARC_ERROR("nvrhi::d3d12::createDevice failed");
                return false;
            }

            if (desc.enableValidation)
                m_nvrhi = nvrhi::validation::createValidationLayer(m_nvrhi);

            // GPU crash backend (F-1/F-4). Built against the OUTER nvrhi
            // device: the validation layer forwards getNativeObject verbatim
            // (validation-device.cpp:83, validation-commandlist.cpp:131), so
            // the native ID3D12Device and the native command lists resolve the
            // same either way, and every command list a caller hands to
            // WriteMarker is the one this device handed out.
            m_crashBackend = MakeD3D12CrashBackend(m_nvrhi.Get());
            if (m_crashBackend)
            {
                g_deviceRemovedReported.store(false, std::memory_order_release);
                // A new device means the loss the latch described is over; a
                // stale latch would quit the host the moment this healthy
                // device started presenting.
                ResetGpuDeviceLost();
                // F-3b: the cross-backend observable, armed now that there is
                // something to collect with.
                NvrhiMessageCallback::Instance().SetDeviceRemovedHook(&ObserveDeviceRemoved);
                // The ONE SetGpuSectionProvider call per host lifetime. The
                // device layer owns the slot because it owns the backend the
                // slot's `user` pointer names; ~DeviceD3D12 clears it.
                Diagnostics::SetGpuSectionProvider(&D3D12GpuSectionProvider, m_crashBackend.get());
                // Task 7's pass scopes read the backend from here. Same owner,
                // same lifetime, same clear site as the provider slot above --
                // F-8e's three command-list owners sit in layers that cannot be
                // handed a backend pointer.
                SetActiveGpuCrashBackend(m_crashBackend.get());
                ARC_INFO("GPU crash backend armed: {}", m_crashBackend->Name());
            }

            ARC_INFO("D3D12 device created on '{}'", m_creation.adapterName);
            return true;
        }

        // ----------------------------------------------------------------
        // DXGI flip-discard swapchain: 3 backbuffers, BGRA8_UNORM.
        // M2 pacing: kSwapchainFramesInFlight slots, EventQuery-gated.
        // ----------------------------------------------------------------

        class SwapchainD3D12 final : public Swapchain
        {
        public:
            ~SwapchainD3D12() override;
            bool Init(DeviceD3D12& device, Window& window, bool vsync);

            nvrhi::ITexture* BeginFrame() override;
            void Present() override;
            void Resize(uint32_t width, uint32_t height) override;
            uint32_t Width() const override { return m_width; }
            uint32_t Height() const override { return m_height; }
            nvrhi::Format Format() const override { return kSwapchainFormat; }

        private:
            bool CreateBackbufferHandles();
            void ReleaseBackbufferHandles();

            DeviceD3D12* m_device = nullptr;
            ComPtr<IDXGISwapChain3> m_swapchain;
            std::vector<nvrhi::TextureHandle> m_backbuffers;
            GpuFrameSlot m_frameSlots[kSwapchainFramesInFlight];
            uint64_t m_frameCounter = 0;
            uint32_t m_width = 0;
            uint32_t m_height = 0;
            bool m_vsync = true;
        };

        bool SwapchainD3D12::Init(DeviceD3D12& device, Window& window, bool vsync)
        {
            m_device = &device;
            m_vsync = vsync;
            window.GetPixelSize(m_width, m_height);

            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            scDesc.Width = m_width;
            scDesc.Height = m_height;
            scDesc.Format = kSwapchainFormatDxgi;
            scDesc.SampleDesc = { 1, 0 };
            scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scDesc.BufferCount = kBackbufferCount;
            scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            HWND hwnd = static_cast<HWND>(window.NativeHandle());
            ComPtr<IDXGISwapChain1> swapchain1;
            if (FAILED(device.Factory()->CreateSwapChainForHwnd(
                    device.GraphicsQueue(), hwnd, &scDesc, nullptr, nullptr,
                    &swapchain1)))
            {
                ARC_ERROR("CreateSwapChainForHwnd failed");
                return false;
            }
            // Prevent DXGI from intercepting Alt+Enter; the engine manages
            // presentation mode explicitly.
            device.Factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            if (FAILED(swapchain1.As(&m_swapchain)))
            {
                ARC_ERROR("IDXGISwapChain3 not available");
                return false;
            }
            if (!CreateBackbufferHandles())
                return false;
            for (uint32_t i = 0; i < kSwapchainFramesInFlight; ++i)
            {
                if (!m_frameSlots[i].Init(m_device->Nvrhi()))
                {
                    ARC_ERROR("Swapchain: frame-slot event query creation failed");
                    return false;
                }
            }
            return true;
        }

        bool SwapchainD3D12::CreateBackbufferHandles()
        {
            m_backbuffers.resize(kBackbufferCount);
            for (uint32_t i = 0; i < kBackbufferCount; ++i)
            {
                ComPtr<ID3D12Resource> buffer;
                if (FAILED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer))))
                {
                    ARC_ERROR("Swapchain GetBuffer({}) failed", i);
                    return false;
                }

                auto texDesc = nvrhi::TextureDesc()
                    .setWidth(m_width)
                    .setHeight(m_height)
                    .setFormat(kSwapchainFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::Present)
                    .setKeepInitialState(true)
                    .setDebugName("SwapchainBuffer");
                m_backbuffers[i] = m_device->Nvrhi()->createHandleForNativeTexture(
                    nvrhi::ObjectTypes::D3D12_Resource,
                    nvrhi::Object(buffer.Get()), texDesc);
                if (!m_backbuffers[i])
                {
                    ARC_ERROR("createHandleForNativeTexture failed for buffer {}", i);
                    return false;
                }
            }
            return true;
        }

        void SwapchainD3D12::ReleaseBackbufferHandles()
        {
            m_device->Nvrhi()->waitForIdle();
            m_backbuffers.clear();
            m_device->Nvrhi()->runGarbageCollection();
        }

        nvrhi::ITexture* SwapchainD3D12::BeginFrame()
        {
            if (m_width == 0 || m_height == 0 || m_backbuffers.empty())
                return nullptr;

            // Slot gating: before reusing this slot's per-frame resources,
            // wait until the frame that last used it (N - framesInFlight)
            // has retired on the GPU.
            if (m_frameCounter >= kSwapchainFramesInFlight)
            {
                // Same completion semantics as the waitEventQuery this replaced
                // -- never returns early -- but a STAMPED slot is waited on by
                // polling, republishing the diagnostics beats, which is what
                // makes a wedged GPU observable as `gpu-stall` instead of
                // parking the main thread where the watchdog cannot see it. An
                // UNSTAMPED slot (a frame that bailed before Present) falls
                // through to nvrhi's instant wait -- polling one would never
                // complete. Both rules live in GpuFrameSlot, not here.
                m_frameSlots[m_frameCounter % kSwapchainFramesInFlight]
                    .WaitAndReset(m_device->Nvrhi());
            }
            return m_backbuffers[m_swapchain->GetCurrentBackBufferIndex()];
        }

        void SwapchainD3D12::Present()
        {
            if (m_backbuffers.empty())
                return;  // nothing acquired (zero-size window); see BeginFrame

            const HRESULT hr = m_swapchain->Present(m_vsync ? 1 : 0, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                // Log-once: the host quits on the latch a frame later, but a
                // per-frame ERROR in the window between observation and exit
                // (or in any future recovery path) is pure spam -- the first
                // one said everything this one would.
                if (!g_deviceRemovedReported.load(std::memory_order_acquire))
                    ARC_ERROR("Present failed: device removed/reset (0x{:08X}), reason 0x{:08X}",
                              (uint32_t)hr,
                              (uint32_t)m_device->D3D12Device()->GetDeviceRemovedReason());
                // F-3c: this is the only first-party device-removed site in
                // the tree, and it used to log and continue. It now runs the
                // capture path -- markers + DRED are read while they still
                // describe THIS removal. Once-guarded inside, and a no-op if
                // F-3b's submit-side hook already fired for the same removal.
                ObserveDeviceRemoved();
            }

            // Mark this slot's command-work completion point (the display-side
            // headroom comes from the backbuffer count, not this fence);
            // BeginFrame N+2 waits on it.
            m_frameSlots[m_frameCounter % kSwapchainFramesInFlight]
                .Stamp(m_device->Nvrhi(), nvrhi::CommandQueue::Graphics);
            ++m_frameCounter;

            // Recycle retired command-list instances and upload regions.
            m_device->Nvrhi()->runGarbageCollection();
        }

        void SwapchainD3D12::Resize(uint32_t width, uint32_t height)
        {
            if (width == m_width && height == m_height)
                return;
            m_width = width;
            m_height = height;
            ReleaseBackbufferHandles();
            if (width == 0 || height == 0)
                return;  // minimized; BeginFrame returns null until restored
            if (FAILED(m_swapchain->ResizeBuffers(kBackbufferCount, width, height,
                                                  kSwapchainFormatDxgi, 0)))
            {
                ARC_ERROR("ResizeBuffers({}x{}) failed", width, height);
                return;
            }
            CreateBackbufferHandles();
        }

        SwapchainD3D12::~SwapchainD3D12()
        {
            if (m_device)
                ReleaseBackbufferHandles();
        }

        std::unique_ptr<Swapchain> DeviceD3D12::CreateSwapchain(Window& window,
                                                                bool vsync)
        {
            auto swapchain = std::make_unique<SwapchainD3D12>();
            if (!swapchain->Init(*this, window, vsync))
                return nullptr;
            return swapchain;
        }
    }

    // ----------------------------------------------------------------
    // The CREATION HALF (NRI Phase 1, Task 7).
    // ----------------------------------------------------------------
    // This function IS the former prologue of DeviceD3D12::Init, moved out
    // whole so the NRI wrapper can reuse it: the same calls in the same order
    // with the same parameters, the same log lines, and the same early
    // returns. The only textual change is that what used to be written into
    // DeviceD3D12's members is written into `out` -- which is now where
    // DeviceD3D12 keeps them anyway (m_creation), so the NVRHI path reads
    // exactly the values it read before, and the member ORDER inside
    // D3D12DeviceCreation reproduces the COM release order this class had.
    //
    // It sits at namespace scope (outside the anonymous namespace above)
    // because DeviceCreationD3D12.hpp declares it for the other consumers.
    //
    // Failure leaves `out` holding whatever was created; the caller's
    // teardown releases it, exactly as ~DeviceD3D12 always did when Init
    // bailed.
    bool CreateD3D12NativeDevice(const RenderDeviceDesc& desc, D3D12DeviceCreation& out)
    {
        // Recorded, not acted on: NRI's own validation layer is available in
        // wrapper mode (contract 2.1) and keys off the same switch the NVRHI
        // validation layer does. Pure member write -- no call, no branch,
        // nothing the NVRHI path can observe.
        out.enableValidation = desc.enableValidation;

        // The debug layer is PROCESS-GLOBAL state with a one-shot window (see
        // g_d3d12DeviceCreated above): it can only be turned on while this
        // process owns no D3D12 device, and turning it on later removes the
        // device that already exists. So this asks three questions in order --
        // is it already on, is it too late, otherwise turn it on -- rather than
        // enabling unconditionally. `debugLayerActive` is what the rest of this
        // function keys off, because "the caller asked for the layer" and "the
        // layer is servicing this device" stopped being the same thing here.
        UINT factoryFlags     = 0;
        bool debugLayerActive = false;
        if (desc.enableD3D12DebugLayer)
        {
            if (g_d3d12DebugLayerEnabled.load(std::memory_order_acquire))
            {
                // Already on process-wide, from an earlier device's creation.
                // Re-calling EnableDebugLayer is the illegal post-device call
                // AND would buy nothing: the layer that is already loaded is
                // the one that will service this device too.
                debugLayerActive = true;
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
            else if (g_d3d12DeviceCreated.load(std::memory_order_acquire))
            {
                // THE `--nri-graph` CASE, and the tradeoff stated out loud.
                // This device gets NO D3D12 debug layer: enabling it now would
                // remove the engine's live NVRHI device (and, observed at the
                // desk, makes the D3D12CreateDevice below fail outright), so
                // the choice is "one device short of a validation channel" vs
                // "no working device at all".
                //
                // WHAT IS LOST: D3D12 CPU validation messages for THIS device
                // cannot reach D3D12DebugLayerCallback and therefore cannot fail
                // the RenderErrorCount latch. NRI's own validation layer and (on
                // Vulkan) the VK validation layers are unaffected -- they are
                // per-device, not process-global. On the dev box the loss is
                // currently nil: the in-box Win10 D3D12SDKLayers.dll implements
                // no ID3D12InfoQueue1, so those messages reach nothing anyway
                // (see the WARN further down).
                //
                // HOW TO GET IT BACK, when it is worth having: the FIRST device
                // in the process has to be the one that turns the layer on --
                // i.e. the host would set RenderDeviceDesc::enableD3D12DebugLayer
                // on its own boot device when a --nri-graph run is requested,
                // and this branch would then never be reached (the first branch
                // above would take it instead). Deliberately NOT done as part of
                // this fix: it would newly subject the engine's NVRHI half to a
                // debug layer it has never run under, which can only ADD ways
                // for the vehicle run to exit nonzero for reasons that have
                // nothing to do with the graph. Phase 3's one-device flip
                // dissolves the question entirely.
                ARC_WARN("D3D12 debug layer NOT enabled for this device: a D3D12 device already "
                         "exists in this process and EnableDebugLayer is documented to remove it "
                         "when called after device creation. This device's D3D12 validation "
                         "messages will not reach the log or the error latch.");
            }
            else
            {
                ComPtr<ID3D12Debug> debug;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                {
                    debug->EnableDebugLayer();
                    // Set BEFORE the create below, so the flag means "this
                    // process has called EnableDebugLayer" and not "a device
                    // came up afterwards" -- a failed create must not leave the
                    // next caller thinking the layer is still enablable.
                    g_d3d12DebugLayerEnabled.store(true, std::memory_order_release);
                    debugLayerActive = true;
                    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                }
                else
                {
                    ARC_WARN("D3D12 debug layer unavailable; continuing without it");
                }
            }
        }

        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&out.factory))))
        {
            ARC_ERROR("CreateDXGIFactory2 failed");
            return false;
        }

        if (FAILED(out.factory->EnumAdapterByGpuPreference(
                0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&out.adapter))))
        {
            ARC_ERROR("No DXGI adapter found");
            return false;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        out.adapter->GetDesc1(&adapterDesc);
        char name[128]{};
        size_t converted = 0;
        wcstombs_s(&converted, name, adapterDesc.Description, _TRUNCATE);
        out.adapterName = name;

        // F-2b: DRED settings are process-global and "you must configure
        // them prior to creating a Direct3D 12 Device" -- modifications
        // have no effect on devices already created. This must therefore
        // sit BEFORE D3D12CreateDevice, and it is deliberately independent
        // of enableD3D12DebugLayer (D3D12GetDebugInterface fetches the DRED
        // settings object without enabling the debug layer). Never fatal:
        // every failure inside degrades the tier and logs one WARN.
        //
        // NRI capability contract item 13: stays exactly here. NRI v180
        // contains no DRED code at all (zero matches for DRED /
        // AutoBreadcrumb / PageFault across its Source and Include), and it
        // never creates the device in wrapper mode, so it cannot clobber
        // this -- but only if the call keeps its position ahead of create.
        EnableD3D12Dred();

        // The HRESULT is in the message because this call's failure mode is
        // otherwise indistinguishable at the desk: D1 hit it three times in a
        // row with no way to tell "no 12_0 adapter" from "the runtime is in a
        // state that refuses to create one" -- SUSPECTED to be an
        // EnableDebugLayer-after-device call (see g_d3d12DeviceCreated), per
        // the MS docs cited above, but never confirmed beyond that one desk
        // repro; treat it as a working theory, not a diagnosed mechanism.
        // That theory also leans on an UNSTATED assumption: the check-then-act
        // read of g_d3d12DebugLayerEnabled/g_d3d12DeviceCreated above is two
        // independent atomic loads, not one transaction, so it is only race-
        // free if CreateD3D12NativeDevice is never entered from more than one
        // thread at a time. Nothing in the tree calls this off the main
        // thread today; a concurrent caller would need its own serialization.
        const HRESULT createHr = D3D12CreateDevice(out.adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                                   IID_PPV_ARGS(&out.device));
        if (FAILED(createHr))
        {
            ARC_ERROR("D3D12CreateDevice failed (feature level 12_0, hr=0x{:08X}) on '{}'",
                      static_cast<uint32_t>(createHr), out.adapterName);
            return false;
        }

        // From here on this process owns a device, so the EnableDebugLayer
        // window above is CLOSED for every later creation.
        g_d3d12DeviceCreated.store(true, std::memory_order_release);

        // The device-side half of the debug layer. BOTH QueryInterface results
        // are kept, because which one fails IS the diagnosis: the base
        // ID3D12InfoQueue is implemented by the debug layer's device wrapper,
        // so failing it means the debug layer is not on this device at all,
        // while failing only ID3D12InfoQueue1 means the layer that answered is
        // too old for the callback interface. The old WARN here could tell
        // neither apart -- it discarded both HRESULTs -- and asserted a cause
        // ("pre-Agility D3D12 runtime") that the same run's "Using
        // ID3D12Device15" disproves. Each branch below now states only what it
        // actually knows.
        //
        // Keyed on `debugLayerActive`, NOT on desc.enableD3D12DebugLayer: when
        // the enable above was skipped because a device already existed, the
        // layer is genuinely absent from this device and every WARN in here
        // would be reporting our own decision back to us as a mystery.
        if (debugLayerActive)
        {
            ComPtr<ID3D12InfoQueue> infoQueue;
            const HRESULT infoQueueHr = out.device.As(&infoQueue);
            if (SUCCEEDED(infoQueueHr))
            {
                // The D3D12 debug layer defaults to break-on-error, which calls
                // __fastfail when the info queue receives a
                // D3D12_MESSAGE_SEVERITY_ERROR or CORRUPTION message. Route all
                // validation through the callback below (which logs at the
                // appropriate level and bumps the latch) rather than aborting
                // the process on first error.
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);

                // Phase 2, Task 1 (Phase 1 Task 6's deferred minor): deny
                // INFO/MESSAGE at the info queue instead of dropping them in
                // the callback. The debug layer emits one of each per resource
                // create and destroy; filtering here means they are never
                // stored and never cross into D3D12DebugLayerCallback at all.
                // Same subscription as the Vulkan messenger, which takes Error
                // and Warning only (DeviceVulkan.cpp) -- so "an error happened"
                // and "the log is quiet" mean one thing on both backends.
                D3D12_MESSAGE_SEVERITY denied[]{ D3D12_MESSAGE_SEVERITY_INFO,
                                                 D3D12_MESSAGE_SEVERITY_MESSAGE };
                D3D12_INFO_QUEUE_FILTER filter{};
                filter.DenyList.NumSeverities = static_cast<UINT>(std::size(denied));
                filter.DenyList.pSeverityList = denied;
                if (FAILED(infoQueue->PushStorageFilter(&filter)))
                {
                    ARC_WARN("ID3D12InfoQueue::PushStorageFilter failed; D3D12 INFO/MESSAGE "
                             "chatter will reach the debug-layer callback");
                }
            }

            // NRI capability contract item 12: turning break-off is all the
            // block above ever did -- the messages themselves went nowhere.
            // ID3D12InfoQueue1::RegisterMessageCallback is what actually
            // delivers them (see D3D12DebugLayerCallback). Missing it is a
            // diagnostics degradation, never a create failure.
            ComPtr<ID3D12InfoQueue1> infoQueue1;
            const HRESULT infoQueue1Hr = out.device.As(&infoQueue1);
            if (SUCCEEDED(infoQueue1Hr))
            {
                DWORD         cookie     = 0;
                const HRESULT registerHr = infoQueue1->RegisterMessageCallback(
                    &D3D12DebugLayerCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
                if (SUCCEEDED(registerHr))
                {
                    out.infoQueue       = infoQueue1;
                    out.infoQueueCookie = cookie;
                }
                else
                {
                    ARC_WARN("ID3D12InfoQueue1::RegisterMessageCallback failed (hr=0x{:08X}); "
                             "D3D12 debug-layer messages will not reach the log",
                             static_cast<uint32_t>(registerHr));
                }
            }
            else if (FAILED(infoQueueHr))
            {
                // TRUE failure case 1: no info queue of any generation, i.e.
                // the debug layer is not attached to this device. Either
                // EnableDebugLayer above did not take effect for the runtime
                // that created the device, or the device predates the enable.
                ARC_WARN("the D3D12 debug layer is not active on this device (ID3D12InfoQueue "
                         "QueryInterface failed, hr=0x{:08X}); D3D12 debug-layer messages will "
                         "not reach the log",
                         static_cast<uint32_t>(infoQueueHr));
            }
            else
            {
                // TRUE failure case 2: the debug layer IS attached (the base
                // interface resolved) but the SDK layers servicing it predate
                // ID3D12InfoQueue1. The module path is the actionable half --
                // <exedir>/D3D12/ is the vendored Agility layer, which has the
                // interface; System32 is the Windows 10 in-box layer, which
                // does not carry the IID at all.
                ARC_WARN("the loaded D3D12 debug layer does not implement ID3D12InfoQueue1 "
                         "(hr=0x{:08X}); D3D12 debug-layer messages will not reach the log. "
                         "Layer servicing this device: {}",
                         static_cast<uint32_t>(infoQueue1Hr), LoadedD3D12SDKLayersModule());
            }
        }

        // NRI capability contract item 10 (creation half): this ONE direct
        // queue is what the wrapper desc must carry in
        // QueueFamilyD3D12Desc::d3d12Queues -- leaving that null makes NRI
        // create its own, and the DXGI swapchain below is bound to THIS
        // one, so it would be presenting on a queue NRI never submits to.
        // Reachable for the wrap through GraphicsQueue() above.
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(out.device->CreateCommandQueue(&queueDesc,
                                                 IID_PPV_ARGS(&out.graphicsQueue))))
        {
            ARC_ERROR("CreateCommandQueue failed");
            return false;
        }

        return true;
    }

    // Contract item 12's teardown half, idempotent: the debug layer holds a
    // raw pointer to a function in THIS module and must not outlive it. Split
    // out of the release below because the NVRHI path has to unregister at
    // one specific point -- before its nvrhi device is released, which is
    // exactly where ~DeviceD3D12 called it before this refactor.
    void UnregisterD3D12DebugCallback(D3D12DeviceCreation& creation)
    {
        if (creation.infoQueue && creation.infoQueueCookie != 0)
        {
            creation.infoQueue->UnregisterMessageCallback(creation.infoQueueCookie);
            creation.infoQueueCookie = 0;
        }
    }

    // Owner teardown (contract item 15: the NRI device is destroyed BEFORE
    // this runs). Releases in the order D3D12DeviceCreation's member layout
    // encodes -- queue, info queue, device, adapter, factory -- which is the
    // order ~DeviceD3D12's member destruction has always produced.
    void DestroyD3D12NativeDevice(D3D12DeviceCreation& creation)
    {
        UnregisterD3D12DebugCallback(creation);
        creation.graphicsQueue.Reset();
        creation.infoQueue.Reset();
        creation.device.Reset();
        creation.adapter.Reset();
        creation.factory.Reset();
    }

    // The narrow export (DeviceFactories.hpp, NRI Phase 3 Task 5): the SAME
    // observer above, reachable by address from the Render module's other
    // installer. One line, no state, no second observation point -- the
    // once-only `g_deviceRemovedReported` latch, the "gpu-crash: device
    // removed" wording and the NoteGpuDeviceLost ordering all stay in
    // ObserveDeviceRemoved, unchanged and file-local.
    void ObserveDeviceRemovedD3D12()
    {
        ObserveDeviceRemoved();
    }

    // Its twin (DeviceFactories.hpp): the SAME store DeviceD3D12::Init makes
    // one line above its own ResetGpuDeviceLost(), for the other arming site.
    // The latch stays file-local and keeps its single meaning -- this only
    // lets NriDiagnostics::Arm re-arm it, exactly as Init does.
    void ResetDeviceRemovedLatchD3D12()
    {
        g_deviceRemovedReported.store(false, std::memory_order_release);
    }

    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc)
    {
        auto device = std::make_unique<DeviceD3D12>();
        if (!device->Init(desc))
            return nullptr;
        return device;
    }
}
