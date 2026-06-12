// D3D12 backend: DXGI factory + adapter + device + direct queue, wrapped
// by nvrhi::d3d12::createDevice. Headless here; the swapchain half of this
// TU arrives with the windowed milestone task.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Arcane
{
    namespace
    {
        class DeviceD3D12 final : public RenderDevice
        {
        public:
            bool Init(const RenderDeviceDesc& desc);

            GraphicsBackend Backend() const override { return GraphicsBackend::D3D12; }
            nvrhi::IDevice* Nvrhi() const override { return m_nvrhi.Get(); }
            std::string AdapterName() const override { return m_adapterName; }

            IDXGIFactory6* Factory() const { return m_factory.Get(); }
            ID3D12CommandQueue* GraphicsQueue() const { return m_graphicsQueue.Get(); }

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
            if (desc.enableValidation)
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
    }

    std::unique_ptr<RenderDevice> CreateDeviceD3D12(const RenderDeviceDesc& desc)
    {
        auto device = std::make_unique<DeviceD3D12>();
        if (!device->Init(desc))
            return nullptr;
        return device;
    }
}
