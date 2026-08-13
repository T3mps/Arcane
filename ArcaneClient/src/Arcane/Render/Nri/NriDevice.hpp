#pragma once

// NRI substrate (Phase 1, Task 7): the WRAPPER PATH.
//
// We create the D3D12/VK device ourselves -- keeping DRED configuration, the
// VK device-fault sweep, our own debug-message routing, and every capability
// the contract requires -- and hand the finished thing to NRI through
// `nriCreateDeviceFromVKDevice` / `nriCreateDeviceFromD3D12Device`. NRI then
// derives its capability model from the physical device plus the extension
// name lists in the desc, and does NOT take ownership of our native objects.
//
// A call to `nriCreateDevice` for a REAL backend (D3D12/VK) is a review
// defect (spec ratification 2) -- the only sanctioned use of that entry point
// in this tree is the NONE backend in the [nri] tests, which has no native
// device to wrap. Nothing in this header or its .cpp calls it.
//
// Include order: NRI's Extensions/NRIDeviceCreation.h declares nri::Message
// with an enumerator literally named ERROR, and <windows.h> (reachable
// through spdlog) #defines ERROR via wingdi.h. Keep the NRI includes first,
// here and in NriDevice.cpp -- same rule as NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>

#include <memory>

namespace Arcane
{
    // The creation halves. Defined in Render/DeviceCreationVulkan.hpp and
    // Render/DeviceCreationD3D12.hpp, which carry vk:: / D3D12 types and are
    // therefore Render-internal -- forward-declared here so this header stays
    // consumable by code outside ArcaneClient.dll (ArcaneTests has no Vulkan
    // headers and no Vulkan-Hpp dispatcher storage of its own).
    struct VulkanDeviceCreation;
    struct D3D12DeviceCreation;

    // -------------------------------------------------------------------
    // NativeDeviceOwner -- the caller-owns half of the ownership contract.
    // -------------------------------------------------------------------
    // NriDevice WRAPS a creation half; it never owns one. Something else has
    // to, and outlive it (contract item 15: the NRI device is destroyed
    // BEFORE the native device, because ~DeviceVK/~DeviceD3D12 release NRI's
    // own objects -- VMA allocator, queues, render-pass caches -- against it,
    // while our VkDevice/ID3D12Device is left untouched).
    //
    // For the engine's own device that owner is `DeviceVulkan`/`DeviceD3D12`
    // (they hold their creation half as a member). This class is the owner
    // for everything else: the Phase-1 smoke, and the [gpu] wrap-smoke test,
    // which lives in ArcaneTests and cannot name a creation half's type.
    class ARCANE_API NativeDeviceOwner
    {
    public:
        // Runs the SAME creation half the NVRHI RenderDevice path runs --
        // literally the same function -- so what gets wrapped is what the
        // engine boots with. Returns null on failure (reason already logged).
        //
        // One live Vulkan device per process: the Vulkan-Hpp default
        // dispatcher binds ONE VkDevice (see VulkanDispatchStorage.cpp), so
        // do not hold one of these alongside a live Vulkan RenderDevice.
        static std::unique_ptr<NativeDeviceOwner> Create(const RenderDeviceDesc& desc);

        ~NativeDeviceOwner();

        NativeDeviceOwner(const NativeDeviceOwner&)            = delete;
        NativeDeviceOwner& operator=(const NativeDeviceOwner&) = delete;

        [[nodiscard]] GraphicsBackend Backend() const;

        // Exactly one of these is non-null, per Backend(). Both types are
        // incomplete outside the Render module -- in-module callers include
        // the matching DeviceCreation*.hpp; everyone else just passes the
        // owner to NriDevice::Wrap.
        [[nodiscard]] const VulkanDeviceCreation* Vulkan() const;
        [[nodiscard]] const D3D12DeviceCreation*  D3D12() const;

    private:
        NativeDeviceOwner();

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // -------------------------------------------------------------------
    // NriDevice -- the wrapped device.
    // -------------------------------------------------------------------
    class ARCANE_API NriDevice
    {
    public:
        // Wrapper path only. Each returns null (loudly, naming what failed)
        // if the desc could not be filled, if NRI refused the wrap, or if a
        // post-wrap assert did not hold. The creation half must outlive the
        // returned object.
        static std::unique_ptr<NriDevice> WrapVulkan(const VulkanDeviceCreation& creation);
        static std::unique_ptr<NriDevice> WrapD3D12(const D3D12DeviceCreation& creation);
        // Backend-dispatching convenience over the two above.
        static std::unique_ptr<NriDevice> Wrap(const NativeDeviceOwner& native);

        // Destroys the NRI device ONLY. The native device it wrapped is the
        // caller's and is untouched. Drains the graveyard first, after an
        // NRI DeviceWaitIdle -- Graveyard's contract is "the caller has
        // already made the GPU idle", and this is the one place that can
        // honor it on the way out.
        ~NriDevice();

        NriDevice(const NriDevice&)            = delete;
        NriDevice& operator=(const NriDevice&) = delete;

        [[nodiscard]] nri::Device& Device() const { return *m_device; }
        [[nodiscard]] const nri::CoreInterface& Core() const { return m_core; }
        [[nodiscard]] GraphicsBackend Backend() const { return m_backend; }

        // The GRAPHICS queue proven reachable at wrap time (contract item 7 /
        // §1.6.3: NRI clamps a declared queueNum against PHYSICAL family
        // capacity, and a failed throwaway-instance probe clamps it to ZERO,
        // which would otherwise surface as an UNSUPPORTED far from the cause).
        [[nodiscard]] nri::Queue* GraphicsQueue() const { return m_graphicsQueue; }

        // Fence-tagged deferred destruction for this device's queue timeline.
        // Per the plan's resolved plan-input, the graveyard is a per-device
        // object owned HERE, one per queue timeline -- Phase 1 has a single
        // graphics timeline, so there is one. (Named Graves() because a
        // member function called Graveyard() would shadow the type inside
        // this class.)
        [[nodiscard]] Graveyard& Graves() { return m_graveyard; }

    private:
        NriDevice() = default;

        // Shared tail of both Wrap* entry points: CoreInterface, the queue
        // assert, and the identity log (contract item 14).
        static std::unique_ptr<NriDevice> FinishWrap(nri::Device* device,
                                                     GraphicsBackend backend);

        Graveyard          m_graveyard;
        nri::CoreInterface m_core{};
        nri::Device*       m_device        = nullptr;
        nri::Queue*        m_graphicsQueue = nullptr;
        GraphicsBackend    m_backend       = GraphicsBackend::D3D12;
    };
}
