#pragma once

// The D3D12 CREATION HALF. Sibling of DeviceCreationVulkan.hpp -- read its
// header comment first; the same shape applies here.
//
// ONE CONSUMER: `Nri/NriDevice.cpp`, which fills
// `nri::DeviceCreationD3D12Desc` and wraps via
// `nriCreateDeviceFromD3D12Device`.
//
// Render-internal, not exported: that consumer compiles into
// ArcaneClient.dll.

#include <Arcane/Render/RenderDeviceDesc.hpp>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace Arcane
{
    // MEMBER ORDER IS COM RELEASE ORDER IN REVERSE, and it is load-bearing:
    // graphics queue, then info queue, then device, then adapter, then
    // factory.
    struct D3D12DeviceCreation
    {
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        Microsoft::WRL::ComPtr<ID3D12Device>  device;

        // The reference ARMOR on `device`: this many extra AddRefs, taken at
        // creation and audited then released as the very last thing
        // DestroyD3D12NativeDevice does. It absorbs a module outside Arcane
        // releasing our device more than it acquired (an injected overlay:
        // see ArmorD3D12Device), so no teardown release can reach zero under
        // NRI, D3D12MA or this owner; the audit names any deficit.
        ULONG deviceArmorRefs = 0;

        // Contract item 12's registration, held so the owner can unregister:
        // the callback is a function in ArcaneClient.dll and must not outlive
        // it. Null (and cookie 0) unless the debug layer was requested AND
        // ID3D12InfoQueue1 resolved.
        Microsoft::WRL::ComPtr<ID3D12InfoQueue1> infoQueue;
        DWORD                                    infoQueueCookie = 0;

        // The BASE info queue, kept alive for the device's whole life: on a
        // layer without ID3D12InfoQueue1 (the Windows 10 in-box one) it is
        // the only reader of the D3D12 layer's stored messages, and it is
        // where the break-on-severity disarm was written. Null when the debug
        // layer is not on this device.
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueueBase;

        // Contract item 10: the ONE D3D12_COMMAND_LIST_TYPE_DIRECT queue.
        // It is what the DXGI swapchain binds to, and it is what the wrapper
        // desc MUST carry in QueueFamilyD3D12Desc::d3d12Queues -- leaving
        // that null makes NRI create its own and orphans the swapchain.
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue;

        std::string adapterName;

        // RenderDeviceDesc::enableValidation as it was at creation -- what
        // NRI's own validation layer (`enableNRIValidation`) keys off. Note
        // `enableGraphicsAPIValidation` is NOT a wrapper-desc field at all
        // (contract §2.1): the D3D12 debug layer stays entirely ours, which
        // is why item 12's InfoQueue1 callback lives in the creation half.
        bool enableValidation = false;
    };

    // The extracted prologue of `DeviceD3D12::Init`, verbatim. Returns false
    // (having logged via ARC_ERROR) exactly where Init used to.
    bool CreateD3D12NativeDevice(const RenderDeviceDesc& desc, D3D12DeviceCreation& out);

    // Contract item 12's teardown half, idempotent. Separate from the release
    // below because the callback must be unregistered BEFORE the device it
    // names is released, while the COM references go with the member, after
    // it.
    void UnregisterD3D12DebugCallback(D3D12DeviceCreation& creation);

    // Owner teardown: unregister, then drop every COM reference in the order
    // the member layout above encodes.
    void DestroyD3D12NativeDevice(D3D12DeviceCreation& creation);

    // The DXGI debug layer's messages, read out of its (store-only) info
    // queue into the log and the RenderErrorCount latch. No-op when the debug
    // layer was not requested or DXGIDebug.dll is absent. `moment` names the
    // drain site in each message. Called by the owner's teardown above and
    // by ~NriDevice around nriDestroyDevice; safe from any backend.
    void DrainDxgiDebugMessages(const char* moment);

    // The D3D12 debug layer's own stored messages (creation.infoQueueBase),
    // read into the log and the latch the same way. No-op without the layer.
    void DrainD3D12DebugMessages(const D3D12DeviceCreation& creation, const char* moment);

}
