// Canary for the process-wide D3D12 debug layer in ArcaneTests.
//
// The per-device flag stays at its default (false). What arms the layer is
// ForceD3D12DebugLayerForProcess, called from test_main before any test --
// a case that set enableD3D12DebugLayer itself would pass on the first device
// in the process and would not prove the structural path.
//
// The invalid call is CreateCommittedResource with Width == 0. The runtime
// returns E_INVALIDARG and the device stays. The debug layer stores that as a
// CPU ERROR. Where ID3D12InfoQueue1 registers, the callback latches it during
// the call; where that interface does not, DrainD3D12DebugMessages latches the
// stored message when the NRI device is destroyed. The count is read after
// both owners are released, so either delivery moves it. With the layer off
// the HRESULT still fails and the latch does not move.

#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "Helpers/GpuCapability.hpp"

#include <cstdint>

#if ARC_PLATFORM_WINDOWS
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <d3d12.h>
#    ifdef ERROR
#        undef ERROR
#    endif

namespace
{
    struct ResetRenderErrors
    {
        ~ResetRenderErrors() { Arcane::ResetRenderErrorCount(); }
    };
}

TEST_CASE("D3D12 debug layer: an invalid CreateCommittedResource increments RenderErrorCount",
          "[gpu][d3d12][validation]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);

    // Destroyed last, after the device, so a message raised on the way out
    // cannot leak into the next case.
    ResetRenderErrors reset;

    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;

    auto native = Arcane::NativeDeviceOwner::Create(desc);
    REQUIRE(native != nullptr);
    auto nri = Arcane::NriDevice::Wrap(*native);
    REQUIRE(nri != nullptr);
    REQUIRE(nri->Core().GetDeviceNativeObject != nullptr);

    void* const nativeObject = nri->Core().GetDeviceNativeObject(&nri->Device());
    REQUIRE(nativeObject != nullptr);
    auto* const device = static_cast<ID3D12Device*>(nativeObject);

    // The assertion is about the deliberate call, not about whatever device
    // creation itself reported.
    Arcane::ResetRenderErrorCount();
    REQUIRE(Arcane::RenderErrorCount() == 0);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width            = 0;
    resourceDesc.Height           = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels        = 1;
    resourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* resource = nullptr;
    const HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (resource)
        resource->Release();

    CHECK(FAILED(hr));

    // ~NriDevice drains the info queue (NriDevice.cpp, before nriDestroyDevice).
    // Releasing it first covers the callback path and the storage-drain path.
    nri.reset();
    native.reset();
    CHECK(Arcane::RenderErrorCount() > 0);
    (void)reset;
}
#endif
