// See NriDevice.hpp for the wrapper-path rationale, and NriCommon.hpp for the
// include-order rule (nri::Message::ERROR vs wingdi.h's ERROR macro) -- the
// NRI headers MUST stay first in this file.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>
#include <Extensions/NRIWrapperD3D12.h>
#include <Extensions/NRIWrapperVK.h>

#include "NriDevice.hpp"

#include "NriCommon.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceCreationD3D12.hpp>
#include <Arcane/Render/DeviceCreationVulkan.hpp>
#include <Arcane/Render/ShaderConventions.hpp>

// wingdi.h (via spdlog -> windows.h, and via vulkan.hpp's WIN32 platform
// block) unconditionally #defines ERROR. Same treatment as NriCommon.cpp:
// undefine it after the last header that could define it.
#undef ERROR

#include <cstdint>
#include <string_view>

namespace Arcane
{
    namespace
    {
        // ---------------------------------------------------------------
        // Contract item 8: vkBindingOffsets.
        // ---------------------------------------------------------------
        // nri::VKBindingOffsets is {sRegister, tRegister, bRegister,
        // uRegister} and ours must equal the dxc -fvk-*-shift values every
        // SPIR-V shader in the tree is compiled with -- NOT NRI's reference
        // {0, 128, 32, 64}, which belongs to its samples' own convention.
        // Getting this wrong is a SILENT wrong-descriptor bug rather than a
        // create-time failure (contract §6 concern 6), so the values are
        // DERIVED from ShaderConventions.hpp (the documented single source of
        // truth, which also feeds the offline compile script) instead of being
        // copied here: edit the shifts there and this either follows or stops
        // compiling.
        constexpr std::uint32_t kNoShift = 0xFFFFFFFFu;

        constexpr std::uint32_t ParseShift(std::string_view text)
        {
            std::uint32_t value = 0;
            for (const char c : text)
            {
                if (c < '0' || c > '9')
                    return kNoShift;
                value = value * 10 + static_cast<std::uint32_t>(c - '0');
            }
            return text.empty() ? kNoShift : value;
        }

        // kSpirvArgs is a flat dxc argv: "-fvk-<x>-shift", <shift>, <space>.
        // Search by flag name rather than index so a reorder there cannot
        // silently re-map our descriptors.
        constexpr std::uint32_t ShiftFor(std::string_view flag)
        {
            for (std::size_t i = 0; i + 1 < kSpirvArgCount; ++i)
            {
                if (std::string_view(kSpirvArgs[i]) == flag)
                    return ParseShift(kSpirvArgs[i + 1]);
            }
            return kNoShift;
        }

        constexpr nri::VKBindingOffsets kVulkanBindingOffsets = {
            /* sRegister */ ShiftFor("-fvk-s-shift"),
            /* tRegister */ ShiftFor("-fvk-t-shift"),
            /* bRegister */ ShiftFor("-fvk-b-shift"),
            /* uRegister */ ShiftFor("-fvk-u-shift"),
        };

        // The values Task 6 verified against NRI's struct field order. Pinned
        // so a change in ShaderConventions.hpp is a deliberate, visible edit
        // here too (both halves have to move together -- the shaders and the
        // descriptors are two sides of one convention).
        static_assert(kVulkanBindingOffsets.sRegister == 128, "vkBindingOffsets: s (samplers) drifted from ShaderConventions.hpp");
        static_assert(kVulkanBindingOffsets.tRegister == 0,   "vkBindingOffsets: t (SRVs) drifted from ShaderConventions.hpp");
        static_assert(kVulkanBindingOffsets.bRegister == 256, "vkBindingOffsets: b (CBVs) drifted from ShaderConventions.hpp");
        static_assert(kVulkanBindingOffsets.uRegister == 384, "vkBindingOffsets: u (UAVs) drifted from ShaderConventions.hpp");
    }

    // -------------------------------------------------------------------
    // NativeDeviceOwner
    // -------------------------------------------------------------------

    struct NativeDeviceOwner::Impl
    {
        GraphicsBackend backend = GraphicsBackend::D3D12;
        // Only the one matching `backend` is allocated. Heap-held because
        // VulkanDeviceCreation carries the Vulkan loader (and both carry live
        // native handles) -- there is nothing to gain from constructing the
        // other backend's half just to leave it empty.
        std::unique_ptr<VulkanDeviceCreation> vulkan;
        std::unique_ptr<D3D12DeviceCreation>  d3d12;
    };

    NativeDeviceOwner::NativeDeviceOwner() : m_impl(std::make_unique<Impl>()) {}

    NativeDeviceOwner::~NativeDeviceOwner()
    {
        if (m_impl->vulkan)
            DestroyVulkanNativeDevice(*m_impl->vulkan);
        if (m_impl->d3d12)
            DestroyD3D12NativeDevice(*m_impl->d3d12);
    }

    std::unique_ptr<NativeDeviceOwner> NativeDeviceOwner::Create(const RenderDeviceDesc& desc)
    {
        auto owner = std::unique_ptr<NativeDeviceOwner>(new NativeDeviceOwner());
        owner->m_impl->backend = desc.backend;

        if (desc.backend == GraphicsBackend::Vulkan)
        {
            try
            {
                // VulkanDeviceCreation's loader member throws when
                // vulkan-1.dll is absent -- the same throw CreateDeviceVulkan
                // catches at make_unique<DeviceVulkan>(), caught here for the
                // same reason (null on a Vulkan-less host, never an escape).
                owner->m_impl->vulkan = std::make_unique<VulkanDeviceCreation>();
                if (!CreateVulkanNativeDevice(desc, *owner->m_impl->vulkan))
                    return nullptr;
            }
            catch (const std::exception& e)
            {
                ARC_ERROR("Native Vulkan device creation failed: {}", e.what());
                return nullptr;
            }
            return owner;
        }

        owner->m_impl->d3d12 = std::make_unique<D3D12DeviceCreation>();
        if (!CreateD3D12NativeDevice(desc, *owner->m_impl->d3d12))
            return nullptr;
        return owner;
    }

    GraphicsBackend NativeDeviceOwner::Backend() const { return m_impl->backend; }

    const VulkanDeviceCreation* NativeDeviceOwner::Vulkan() const { return m_impl->vulkan.get(); }

    const D3D12DeviceCreation* NativeDeviceOwner::D3D12() const { return m_impl->d3d12.get(); }

    // -------------------------------------------------------------------
    // NriDevice
    // -------------------------------------------------------------------

    std::unique_ptr<NriDevice> NriDevice::WrapVulkan(const VulkanDeviceCreation& creation)
    {
        // Pre-wrap guards. NRI answers a null physical device with a bare
        // INVALID_ARGUMENT and a bad family index with a queue that simply
        // does not exist, so name what is missing here instead (contract item
        // 14's rule: the message says WHICH input was wrong).
        if (!creation.device || !creation.instance || !creation.physicalDevice)
        {
            ARC_ERROR("[nri] cannot wrap Vulkan: the creation half has no "
                      "instance/physical device/device (was CreateVulkanNativeDevice run?)");
            return nullptr;
        }
        if (creation.graphicsQueueFamily < 0 || !creation.graphicsQueue)
        {
            ARC_ERROR("[nri] cannot wrap Vulkan: no graphics queue in the creation half");
            return nullptr;
        }
        // NRI's header requires >= 2; our device creation additionally
        // hard-requires 1.3 (contract item 5). Guard the header's floor.
        if (creation.apiMinorVersion < 2)
        {
            ARC_ERROR("[nri] cannot wrap Vulkan: recorded API minor is {} "
                      "(nri::DeviceCreationVKDesc::minorVersion requires >= 2)",
                      creation.apiMinorVersion);
            return nullptr;
        }

        nri::DeviceCreationVKDesc desc = {};

        // §1.1 callbackInterface: our log + the RenderErrorCount latch.
        desc.callbackInterface = MakeNriCallbacks();
        // §1.1 allocationCallbacks: left zeroed. In wrapper mode NRI does not
        // pass allocation callbacks down to Vulkan at all
        // (m_AllocationCallbackPtr stays null), so ours would not be honored
        // by the driver anyway.
        // §1.1 libraryPath: NULL on purpose (contract item 9). NRI loads its
        // OWN handle to the loader and builds its OWN dispatch table; leaving
        // this null makes both resolve the same vulkan-1.dll our
        // vk::detail::DynamicLoader opened.
        desc.libraryPath = nullptr;
        desc.vkBindingOffsets = kVulkanBindingOffsets;

        // §1.1 vkExtensions (contract item 6): the FILTERED lists the device
        // was actually created with, taken verbatim -- NRI never filters them
        // in wrapper mode (ProcessDeviceExtensions runs only when !isWrapper),
        // and they are its ONLY gate on function-pointer resolution and on
        // extension-feature struct chaining. Handed over unconditioned
        // because of Task 6's strong invariant: every name in the device list
        // had its feature bits enabled on the created device too.
        desc.vkExtensions.instanceExtensions    = creation.enabledInstanceExtensions.data();
        desc.vkExtensions.instanceExtensionNum  = (uint32_t)creation.enabledInstanceExtensions.size();
        desc.vkExtensions.deviceExtensions      = creation.enabledDeviceExtensions.data();
        desc.vkExtensions.deviceExtensionNum    = (uint32_t)creation.enabledDeviceExtensions.size();

        desc.vkInstance       = (VKHandle)(VkInstance)creation.instance;
        desc.vkDevice         = (VKHandle)(VkDevice)creation.device;
        desc.vkPhysicalDevice = (VKHandle)(VkPhysicalDevice)creation.physicalDevice;

        // §1.1 / item 7: ONE graphics family, queueNum EQUAL to the
        // queueCount we passed to vkCreateDevice (1). NRI clamps this against
        // physical family capacity, not against what we created -- declaring
        // more would have it retrieve queues that do not exist.
        // Non-const on purpose: nriCreateDeviceFromVKDevice writes the clamped
        // value back through a const_cast (Creation.cpp), so this must be
        // writable storage.
        nri::QueueFamilyVKDesc queueFamily = {};
        queueFamily.queueNum    = 1;
        queueFamily.queueType   = nri::QueueType::GRAPHICS;
        queueFamily.familyIndex = (uint32_t)creation.graphicsQueueFamily;
        desc.queueFamilies   = &queueFamily;
        desc.queueFamilyNum  = 1;

        // §1.1 / item 5: the ACTUAL created API minor -- never a constant.
        // NRI reads it verbatim (it drives promoted-feature chaining, the
        // copyCommands2/extendedDynamicState verdicts, and VMA's
        // vulkanApiVersion) and never re-queries it on the wrapper path.
        // `creation.apiMinorVersion` is min(physical minor, REQUESTED minor)
        // (DeviceVulkan.cpp's Init()), not min(instance minor, physical
        // minor) as §6 concern 8 phrases it -- see VulkanDeviceCreation::
        // apiMinorVersion's comment in DeviceCreationVulkan.hpp for why the
        // two agree today and the tripwire for when they might stop.
        desc.minorVersion = (uint8_t)creation.apiMinorVersion;

        // Not a field: `disableVKRayTracing` exists on nri::DeviceCreationDesc
        // but NOT on the wrapper desc, and it is consumed only inside
        // ProcessDeviceExtensions -- which the wrapper path skips (§6 concern
        // 5). Ray-tracing extensions are in or out purely by OUR list above,
        // and ours names none of them.

        // NRI's own validation layer IS available in wrapper mode; it tracks
        // the same switch the VK validation layer was created under.
        desc.enableNRIValidation = creation.enableValidation;
        // Item 9: false, and it must stay false while
        // VK_EXT_zero_initialize_device_memory is deliberately absent from our
        // extension lists -- enabling it flips every image's initialLayout to
        // ZERO_INITIALIZED_EXT.
        desc.enableMemoryZeroInitialization = false;

        nri::Device* device = nullptr;
        if (!ARC_NRI_CHECK(nriCreateDeviceFromVKDevice(desc, device)) || !device)
        {
            ARC_ERROR("[nri] nriCreateDeviceFromVKDevice failed on '{}'", creation.adapterName);
            return nullptr;
        }

        return FinishWrap(device, GraphicsBackend::Vulkan);
    }

    std::unique_ptr<NriDevice> NriDevice::WrapD3D12(const D3D12DeviceCreation& creation)
    {
        if (!creation.device)
        {
            ARC_ERROR("[nri] cannot wrap D3D12: the creation half has no device "
                      "(was CreateD3D12NativeDevice run?)");
            return nullptr;
        }
        if (!creation.graphicsQueue)
        {
            ARC_ERROR("[nri] cannot wrap D3D12: no graphics queue in the creation half");
            return nullptr;
        }

        nri::DeviceCreationD3D12Desc desc = {};
        desc.d3d12Device = creation.device.Get();

        // §2.1 / item 10: hand NRI OUR ID3D12CommandQueue. If d3d12Queues is
        // left null NRI creates its own, and the DXGI swapchain -- which is
        // bound to this queue -- would be presenting on a queue NRI never
        // submits to. Same non-const reasoning as the VK family above: NRI
        // clamps queueNum in place.
        ID3D12CommandQueue* const queues[] = { creation.graphicsQueue.Get() };
        nri::QueueFamilyD3D12Desc queueFamily = {};
        queueFamily.d3d12Queues = queues;
        queueFamily.queueNum    = 1;
        queueFamily.queueType   = nri::QueueType::GRAPHICS;
        desc.queueFamilies  = &queueFamily;
        desc.queueFamilyNum = 1;

        // §2.1 agsContext: null -- AMD AGS stays off in wrapper mode unless we
        // supply a context, and AGS is out of the vendoring by spec.
        desc.agsContext = nullptr;
        desc.callbackInterface = MakeNriCallbacks();
        // allocationCallbacks: left zeroed (NRI defaults them).
        // d3dShaderExtRegister / d3dZeroBufferSize: 0 means "NRI's default"
        // in both cases (NRI_SHADER_EXT_REGISTER, and a 4 MB zero buffer) --
        // we have no reason to move either yet.
        desc.d3dShaderExtRegister = 0;
        desc.d3dZeroBufferSize    = 0;

        desc.enableNRIValidation = creation.enableValidation;
        desc.enableMemoryZeroInitialization = false;

        // Item 13's desc half: enhanced barriers ON wherever the Agility
        // runtime reports them (this flag is a DISABLE, so false = on). Task
        // 3 vendored the redistributable precisely so this is not moot; the
        // "Using ID3D12Device10+" proof line lands at the desk milestone.
        desc.disableD3D12EnhancedBarriers = false;
        // §2.6.2: NRI's wrapper-mode NVAPI logic reads inverted (the default
        // DISABLES NVAPI when wrapping). Moot for us -- NVAPI is out of the
        // vendoring, so NRI_ENABLE_NVAPI is 0 and this flag reaches no code.
        // Left at the documented default rather than pretending otherwise.
        desc.disableNVAPIInitialization = false;

        nri::Device* device = nullptr;
        if (!ARC_NRI_CHECK(nriCreateDeviceFromD3D12Device(desc, device)) || !device)
        {
            ARC_ERROR("[nri] nriCreateDeviceFromD3D12Device failed on '{}'", creation.adapterName);
            return nullptr;
        }

        return FinishWrap(device, GraphicsBackend::D3D12);
    }

    std::unique_ptr<NriDevice> NriDevice::Wrap(const NativeDeviceOwner& native)
    {
        if (const VulkanDeviceCreation* vulkan = native.Vulkan())
            return WrapVulkan(*vulkan);
        if (const D3D12DeviceCreation* d3d12 = native.D3D12())
            return WrapD3D12(*d3d12);

        ARC_ERROR("[nri] cannot wrap: the native device owner holds no creation half");
        return nullptr;
    }

    std::unique_ptr<NriDevice> NriDevice::CreateNoneForTests()
    {
        nri::DeviceCreationDesc desc{};
        desc.graphicsAPI       = nri::GraphicsAPI::NONE;
        desc.callbackInterface = MakeNriCallbacks();

        nri::Device* device = nullptr;
        if (!ARC_NRI_CHECK(nriCreateDevice(desc, device)) || !device)
        {
            ARC_ERROR("[nri] NONE-backend device creation failed");
            return nullptr;
        }

        // Straight into the shared tail: ImplNONE answers GetQueue and
        // nriGetInterface with dummy-but-non-null objects, so both post-wrap
        // asserts hold and the identity log prints backend=NONE. Nothing
        // downstream needs to know this device was not wrapped.
        return FinishWrap(device, GraphicsBackend::D3D12);
    }

    std::unique_ptr<NriDevice> NriDevice::FinishWrap(nri::Device* device, GraphicsBackend backend)
    {
        auto wrapped = std::unique_ptr<NriDevice>(new NriDevice());
        wrapped->m_device  = device;
        wrapped->m_backend = backend;
        // From here on every failure returns null and lets ~NriDevice destroy
        // the device -- an NRI device we cannot use must not be leaked.

        // Contract item 14, post-wrap assert 1: the function table itself.
        if (!ARC_NRI_CHECK(nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &wrapped->m_core)))
        {
            ARC_ERROR("[nri] wrap failed: CoreInterface unavailable on the wrapped {} device",
                      ToString(backend));
            return nullptr;
        }

        // Contract item 14, post-wrap assert 2 (and the reason item 7 exists):
        // GetQueue(GRAPHICS, 0) must succeed IMMEDIATELY after wrapping.
        // nriCreateDeviceFrom*Device matches our adapter through a throwaway
        // instance/factory of its own; if that probe comes back empty the
        // adapter's queueNum is zero, every declared queue is clamped to zero,
        // and the failure otherwise surfaces as an UNSUPPORTED at the first
        // submit -- arbitrarily far from the cause (§1.6.3; this box's
        // virtual-display GPU hazard is exactly the environment that can do
        // it).
        if (!ARC_NRI_CHECK(wrapped->m_core.GetQueue(*device, nri::QueueType::GRAPHICS, 0,
                                                    wrapped->m_graphicsQueue))
            || !wrapped->m_graphicsQueue)
        {
            ARC_ERROR("[nri] wrap failed: GetQueue(GRAPHICS, 0) on the wrapped {} device -- "
                      "the declared graphics family was clamped away (NRI wrapper capability "
                      "contract item 7 / §1.6.3)", ToString(backend));
            return nullptr;
        }

        // Contract item 14, post-wrap assert 3: say what we ended up with.
        LogNriIdentity(*device);
        return wrapped;
    }

    NriDevice::~NriDevice()
    {
        if (!m_device)
            return;

        // Graveyard's contract is "the caller has already made the GPU idle";
        // on the teardown path this is the caller. Draining BEFORE
        // nriDestroyDevice matters: the thunks call NRI Destroy* entries
        // against this device.
        if (m_graveyard.Pending() != 0)
        {
            if (m_core.DeviceWaitIdle)
                (void)ARC_NRI_CHECK(m_core.DeviceWaitIdle(m_device));
            m_graveyard.Drain();
        }

        // Contract item 15: this destroys NRI's own objects (VMA allocator,
        // queues, render-pass/framebuffer caches) and NOT our VkDevice /
        // ID3D12Device -- NRI set m_OwnsNativeObjects = false for the wrapper
        // path. The native device must therefore still be alive here, and its
        // owner destroys it after us.
        nriDestroyDevice(m_device);
        m_device        = nullptr;
        m_graphicsQueue = nullptr;
    }
}
