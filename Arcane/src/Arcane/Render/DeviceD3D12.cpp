// D3D12 backend: DXGI factory + adapter + device + direct queue, wrapped
// by nvrhi::d3d12::createDevice. Swapchain: DXGI flip-discard, 3 backbuffers.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
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

        class DeviceD3D12 final : public RenderDevice
        {
        public:
            bool Init(const RenderDeviceDesc& desc);

            GraphicsBackend Backend() const override { return GraphicsBackend::D3D12; }
            nvrhi::IDevice* Nvrhi() const override { return m_nvrhi.Get(); }
            std::string AdapterName() const override { return m_adapterName; }

            IDXGIFactory6* Factory() const { return m_factory.Get(); }
            ID3D12CommandQueue* GraphicsQueue() const { return m_graphicsQueue.Get(); }
            ID3D12Device* D3D12Device() const { return m_device.Get(); }

            std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                       bool vsync) override;

        private:
            // Declaration order is destruction order in reverse: the nvrhi
            // device must release its D3D12 references before the queue,
            // device, adapter, and factory go away.
            ComPtr<IDXGIFactory6>      m_factory;
            ComPtr<IDXGIAdapter1>      m_adapter;
            ComPtr<ID3D12Device>       m_device;
            ComPtr<ID3D12CommandQueue> m_graphicsQueue;
            nvrhi::DeviceHandle        m_nvrhi;
            std::string                m_adapterName;
        };

        bool DeviceD3D12::Init(const RenderDeviceDesc& desc)
        {
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

            if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory))))
            {
                ARC_ERROR("CreateDXGIFactory2 failed");
                return false;
            }

            if (FAILED(m_factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&m_adapter))))
            {
                ARC_ERROR("No DXGI adapter found");
                return false;
            }

            DXGI_ADAPTER_DESC1 adapterDesc{};
            m_adapter->GetDesc1(&adapterDesc);
            char name[128]{};
            size_t converted = 0;
            wcstombs_s(&converted, name, adapterDesc.Description, _TRUNCATE);
            m_adapterName = name;

            if (FAILED(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                         IID_PPV_ARGS(&m_device))))
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
                if (SUCCEEDED(m_device.As(&infoQueue)))
                {
                    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
                }
            }

            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(m_device->CreateCommandQueue(&queueDesc,
                                                    IID_PPV_ARGS(&m_graphicsQueue))))
            {
                ARC_ERROR("CreateCommandQueue failed");
                return false;
            }

            nvrhi::d3d12::DeviceDesc nvrhiDesc;
            nvrhiDesc.errorCB = &NvrhiMessageCallback::Instance();
            nvrhiDesc.pDevice = m_device.Get();
            nvrhiDesc.pGraphicsCommandQueue = m_graphicsQueue.Get();
            m_nvrhi = nvrhi::d3d12::createDevice(nvrhiDesc);
            if (!m_nvrhi)
            {
                ARC_ERROR("nvrhi::d3d12::createDevice failed");
                return false;
            }

            if (desc.enableValidation)
                m_nvrhi = nvrhi::validation::createValidationLayer(m_nvrhi);

            ARC_INFO("D3D12 device created on '{}'", m_adapterName);
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
            nvrhi::EventQueryHandle m_frameQueries[kSwapchainFramesInFlight];
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
                m_frameQueries[i] = m_device->Nvrhi()->createEventQuery();
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
                nvrhi::IEventQuery* slotQuery =
                    m_frameQueries[m_frameCounter % kSwapchainFramesInFlight];
                m_device->Nvrhi()->waitEventQuery(slotQuery);
                m_device->Nvrhi()->resetEventQuery(slotQuery);
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
                ARC_ERROR("Present failed: device removed/reset (0x{:08X}), reason 0x{:08X}",
                          (uint32_t)hr,
                          (uint32_t)m_device->D3D12Device()->GetDeviceRemovedReason());
            }

            // Mark this slot's GPU completion point; BeginFrame N+2 waits on it.
            m_device->Nvrhi()->setEventQuery(
                m_frameQueries[m_frameCounter % kSwapchainFramesInFlight],
                nvrhi::CommandQueue::Graphics);
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

    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc)
    {
        auto device = std::make_unique<DeviceD3D12>();
        if (!device->Init(desc))
            return nullptr;
        return device;
    }
}
