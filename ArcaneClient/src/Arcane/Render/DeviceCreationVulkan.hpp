#pragma once

// The Vulkan CREATION HALF (NRI Phase 1, Task 7).
//
// Everything `DeviceVulkan::Init` used to produce before it started talking
// to NVRHI -- loader, instance, debug messenger, physical device, logical
// device, the one graphics queue, and the choices made along the way -- lifted
// out of that class into a struct with TWO consumers:
//
//   (a) the existing NVRHI device path (`DeviceVulkan`), byte-identically:
//       same call order, same parameters, same logging, same failure paths.
//       The extraction is textual -- `m_x` became `out.x` and nothing else.
//   (b) `Nri/NriDevice.cpp`, which fills `nri::DeviceCreationVKDesc` from it
//       and wraps the device via `nriCreateDeviceFromVKDevice`.
//
// EXTRACT, DON'T REDESIGN: this header adds no behavior. If a field here
// looks like it wants a better shape, that is a Phase 2 conversation -- the
// Phase 0 goldens are this phase's regression floor and the NVRHI path may
// not shift a pixel.
//
// Render-internal, exactly like `DeviceFactories.hpp`: not part of the
// engine's public API surface, and deliberately NOT exported -- both
// consumers compile into ArcaneClient.dll. (Callers outside the DLL own a
// creation half through `NativeDeviceOwner` in `Nri/NriDevice.hpp`, which is
// backend-header-free: the Vulkan-Hpp dynamic-dispatcher storage lives in
// this module alone -- see VulkanDispatchStorage.cpp.)

#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>   // VulkanCrashDesc::DeviceFault

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Arcane
{
    // Every field is either read by the NVRHI path today or required by
    // nri::DeviceCreationVKDesc (capability contract 1.1). Nothing else.
    struct VulkanDeviceCreation
    {
        // FIRST, because it must outlive every handle below -- it owns
        // vulkan-1.dll's module handle, and the default dispatcher's function
        // pointers are resolved through it. (It was `DeviceVulkan::m_loader`,
        // declared first for this exact reason.) Its constructor THROWS when
        // the loader is absent: the same throw `CreateDeviceVulkan`'s
        // try/catch has always caught at `make_unique<DeviceVulkan>()`.
        vk::detail::DynamicLoader loader;

        vk::Instance               instance;
        vk::DebugUtilsMessengerEXT debugMessenger;   // validation builds only
        vk::PhysicalDevice         physicalDevice;
        vk::Device                 device;
        vk::Queue                  graphicsQueue;

        // -1 until a graphics family is found (creation fails if none is).
        // NRI's queue declaration needs the index; contract item 7 requires
        // its declared queueNum to equal the queueCount we created (1).
        int graphicsQueueFamily = -1;

        // Contract item 5 / §6 concern 8: the API minor version the device
        // was ACTUALLY created with. What DeviceVulkan.cpp's Init() actually
        // computes is min(physical minor, REQUESTED minor / kApiMinorVersion)
        // -- not min(instance minor, physical minor) as §6.8 phrases it,
        // because the instance version is never separately queried anywhere
        // in this file. The two are equivalent TODAY only because the
        // hard-fail guard immediately above that min() in Init() already
        // requires physicalApiMinor >= kApiMinorVersion, so the min() always
        // resolves to kApiMinorVersion (currently 1.3) -- the same answer
        // §6.8 wants, since requesting 1.3 at vkCreateInstance implies the
        // instance supports >= 1.3 too. See DeviceVulkan.cpp's kApiVersion
        // comment for the tripwire if that request ever changes. NRI reads
        // `minorVersion` verbatim in wrapper mode and never re-queries it, so
        // this may never be replaced by a constant.
        std::uint32_t apiMinorVersion = 0;

        // Contract item 6: the FILTERED lists `vkCreateInstance` /
        // `vkCreateDevice` were actually given, including the diagnostics
        // sweep's winners. NRI takes both verbatim -- they are its only gate
        // on function-pointer resolution and extension-feature chaining.
        // Every element points at a string literal, so both vectors stay
        // valid for the process lifetime.
        //
        // STRONG INVARIANT (Task 6): every name in `enabledDeviceExtensions`
        // had its feature bits enabled on the created device too, so the
        // wrapper desc consumes them unconditioned -- there is no residual
        // "enabled the name but not the capability" case to guard against.
        std::vector<const char*> enabledInstanceExtensions;
        std::vector<const char*> enabledDeviceExtensions;

        std::string adapterName;

        // The GPU-crash record the NVRHI path feeds into VulkanCrashDesc.
        // Inert to NRI (it names none of the three extensions -- contract
        // §1.3's last row), but part of what this device was created with.
        VulkanCrashDesc::DeviceFault deviceFault  = VulkanCrashDesc::DeviceFault::None;
        bool                         bufferMarker = false;

        // RenderDeviceDesc::enableValidation as it was at creation: the
        // instance carries VK_LAYER_KHRONOS_validation and the debug
        // messenger because of it, and it is what NRI's own validation layer
        // (`DeviceCreationVKDesc::enableNRIValidation`, available in wrapper
        // mode -- contract §1.1) keys off.
        bool enableValidation = false;
    };

    // The extracted prologue of `DeviceVulkan::Init`, verbatim. Returns false
    // (having logged via ARC_ERROR) exactly where Init used to; leaves `out`
    // holding whatever was created so the caller's teardown path can release
    // it. Throws what Init threw -- `CreateDeviceVulkan`'s catch is unchanged.
    bool CreateVulkanNativeDevice(const RenderDeviceDesc& desc, VulkanDeviceCreation& out);

    // The tail of `~DeviceVulkan`, verbatim: device, then debug messenger,
    // then instance. Idempotent (each handle is nulled as it goes) so an
    // owner may call it explicitly at the point the ordering requires and
    // still hold the struct afterwards.
    void DestroyVulkanNativeDevice(VulkanDeviceCreation& creation);
}
