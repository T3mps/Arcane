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
                // message fails the GPU tests exactly like an [nvrhi] error.
                // (Routing through message() also means a debug-layer text
                // containing "Device Removed" reaches ObserveDeviceRemoved --
                // additive, and the same treatment the NVRHI feed gets.)
                NvrhiMessageCallback::Instance().message(
                    nvrhi::MessageSeverity::Error, text);
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

        UINT factoryFlags = 0;
        if (desc.enableD3D12DebugLayer)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
            else
            {
                ARC_WARN("D3D12 debug layer unavailable; continuing without it");
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

        if (FAILED(D3D12CreateDevice(out.adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                     IID_PPV_ARGS(&out.device))))
        {
            ARC_ERROR("D3D12CreateDevice failed (feature level 12_0)");
            return false;
        }

        // The D3D12 debug layer defaults to break-on-error, which calls
        // __fastfail when the info queue receives a D3D12_MESSAGE_SEVERITY_ERROR
        // or CORRUPTION message. Route all validation through NVRHI's message
        // callback (which logs them at the appropriate level) rather than
        // aborting the process on first error.
        if (desc.enableD3D12DebugLayer)
        {
            ComPtr<ID3D12InfoQueue> infoQueue;
            if (SUCCEEDED(out.device.As(&infoQueue)))
            {
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            }

            // NRI capability contract item 12: turning break-off is all the
            // block above ever did -- the messages themselves went nowhere.
            // Register the callback that actually delivers them (see
            // D3D12DebugLayerCallback). ID3D12InfoQueue1 is an Agility-era
            // interface, so the QueryInterface can fail on an old runtime:
            // that is a diagnostics degradation, never a create failure.
            // (In practice the Agility redistributable makes it available;
            // the proof line lands at the desk milestone.)
            ComPtr<ID3D12InfoQueue1> infoQueue1;
            if (SUCCEEDED(out.device.As(&infoQueue1)))
            {
                DWORD cookie = 0;
                if (SUCCEEDED(infoQueue1->RegisterMessageCallback(
                        &D3D12DebugLayerCallback,
                        D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie)))
                {
                    out.infoQueue = infoQueue1;
                    out.infoQueueCookie = cookie;
                }
                else
                {
                    ARC_WARN("ID3D12InfoQueue1::RegisterMessageCallback failed; "
                             "D3D12 debug-layer messages will not reach the log");
                }
            }
            else
            {
                ARC_WARN("ID3D12InfoQueue1 unavailable (pre-Agility D3D12 runtime); "
                         "D3D12 debug-layer messages will not reach the log");
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

    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc)
    {
        auto device = std::make_unique<DeviceD3D12>();
        if (!device->Init(desc))
            return nullptr;
        return device;
    }
}
