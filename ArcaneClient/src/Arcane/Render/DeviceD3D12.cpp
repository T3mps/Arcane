// D3D12 backend: the native creation half (DeviceCreationD3D12.cpp) wrapped
// by nvrhi::d3d12::createDevice. Swapchain: DXGI flip-discard, 3 backbuffers.
// Also the D3D12 side of F-3's ONE device-removed observation point, which is
// NOT NVRHI-shaped and outlives this file -- see DeviceFactories.hpp.

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/DeviceCreationD3D12.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
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
                RenderErrorLatch::Instance().SetDeviceRemovedHook(nullptr);
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
                RenderErrorLatch::Instance().SetDeviceRemovedHook(&ObserveDeviceRemoved);
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

    // The device-loss QUESTION, as opposed to the observers above which are
    // the ANSWER's delivery. See DeviceFactories.hpp for why NRI's D3D12
    // QueueWaitIdle cannot be asked instead.
    //
    // GetDeviceRemovedReason is the one D3D12 call that is defined ON a
    // removed device -- it is how DeviceD3D12::CaptureGpuSection above already
    // reads the removal reason into the report. S_OK means "not removed";
    // every other HRESULT (DXGI_ERROR_DEVICE_REMOVED / _HUNG / _RESET,
    // DXGI_ERROR_DRIVER_INTERNAL_ERROR) means it is gone. Deliberately not
    // once-only and deliberately silent: it observes nothing and reports
    // nothing, so it stays safe to call from a bail-out path.
    bool D3D12NativeDeviceRemoved(void* nativeDevice) noexcept
    {
        if (!nativeDevice)
            return false;
        return FAILED(static_cast<ID3D12Device*>(nativeDevice)->GetDeviceRemovedReason());
    }

    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc)
    {
        auto device = std::make_unique<DeviceD3D12>();
        if (!device->Init(desc))
            return nullptr;
        return device;
    }
}
