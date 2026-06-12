// Vulkan backend: instance + physical device + device + graphics queue,
// wrapped by nvrhi::vulkan::createDevice. Uses the Vulkan-Hpp default
// dynamic dispatcher (storage TU: VulkanDispatchStorage.cpp). Headless
// here; the swapchain half of this TU arrives with the windowed task.
//
// NDEBUG alignment: NVRHI's Release build defines NDEBUG, which removes
// DispatchLoaderBase::m_valid and vkHeaderVersion from DispatchLoaderDynamic.
// All Arcane configs that link NVRHI must define NDEBUG in Release (done in
// premake5.lua) so both sides agree on the struct layout.

#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>

#include <vulkan/vulkan.hpp>

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    namespace
    {
        // Surface extensions are requested even for headless devices: they
        // cost nothing without a surface, and keep one code path for both
        // headless tests and windowed swapchains.
        const char* kInstanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        const char* kDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

        class DeviceVulkan final : public RenderDevice
        {
        public:
            ~DeviceVulkan() override;
            bool Init(const RenderDeviceDesc& desc);

            GraphicsBackend Backend() const override { return GraphicsBackend::Vulkan; }
            nvrhi::IDevice* Nvrhi() const override { return m_nvrhi.Get(); }
            std::string AdapterName() const override { return m_adapterName; }

            vk::Instance Instance() const { return m_instance; }
            vk::PhysicalDevice PhysicalDevice() const { return m_physicalDevice; }
            vk::Device Device() const { return m_device; }
            vk::Queue GraphicsQueue() const { return m_graphicsQueue; }
            uint32_t GraphicsQueueFamily() const { return (uint32_t)m_graphicsQueueFamily; }

            // The unwrapped backend device, for native queue-semaphore calls.
            // (The validation layer wraps Nvrhi(); the swapchain needs the
            // nvrhi::vulkan::IDevice interface underneath.)
            nvrhi::vulkan::IDevice* VulkanNvrhi() const
            {
                return static_cast<nvrhi::vulkan::IDevice*>(m_nvrhiBackend.Get());
            }

            // Vulkan swapchain not implemented yet -- replaced wholesale in the
            // next task (VK_KHR_swapchain surface/present path).
            std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                       bool vsync) override
            {
                (void)window; (void)vsync;
                ARC_ERROR("Vulkan swapchain not implemented yet (next task)");
                return nullptr;
            }

        private:
            vk::detail::DynamicLoader m_loader;  // must outlive everything below
            vk::Instance       m_instance;
            vk::PhysicalDevice m_physicalDevice;
            vk::Device         m_device;
            vk::Queue          m_graphicsQueue;
            int                m_graphicsQueueFamily = -1;
            nvrhi::DeviceHandle m_nvrhiBackend;  // the real vulkan device
            nvrhi::DeviceHandle m_nvrhi;         // possibly validation-wrapped
            std::string         m_adapterName;
        };

        bool DeviceVulkan::Init(const RenderDeviceDesc& desc)
        {
            auto vkGetInstanceProcAddr =
                m_loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
            if (!vkGetInstanceProcAddr)
            {
                ARC_ERROR("Vulkan loader not available (vulkan-1.dll missing?)");
                return false;
            }
            VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

            std::vector<const char*> layers;
            if (desc.enableValidation)
            {
                try
                {
                    for (const auto& layer : vk::enumerateInstanceLayerProperties())
                    {
                        if (std::string_view(layer.layerName) == kValidationLayer)
                        {
                            layers.push_back(kValidationLayer);
                            break;
                        }
                    }
                }
                catch (const vk::SystemError& e)
                {
                    ARC_WARN("vk::enumerateInstanceLayerProperties threw: {}; "
                             "continuing without validation layer", e.what());
                }
                if (layers.empty())
                    ARC_WARN("{} not installed; continuing without it", kValidationLayer);
            }

            try
            {
                auto appInfo = vk::ApplicationInfo()
                    .setPApplicationName("Arcane")
                    .setApiVersion(VK_API_VERSION_1_3);

                auto instanceInfo = vk::InstanceCreateInfo()
                    .setPApplicationInfo(&appInfo)
                    .setEnabledLayerCount((uint32_t)layers.size())
                    .setPpEnabledLayerNames(layers.data())
                    .setEnabledExtensionCount((uint32_t)std::size(kInstanceExtensions))
                    .setPpEnabledExtensionNames(kInstanceExtensions);

                m_instance = vk::createInstance(instanceInfo);
            }
            catch (const vk::SystemError& e)
            {
                ARC_ERROR("vk::createInstance failed: {}", e.what());
                return false;
            }

            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);

            std::vector<vk::PhysicalDevice> physicalDevices;
            try
            {
                physicalDevices = m_instance.enumeratePhysicalDevices();
            }
            catch (const vk::SystemError& e)
            {
                ARC_ERROR("vk::enumeratePhysicalDevices failed: {}", e.what());
                return false;
            }

            if (physicalDevices.empty())
            {
                ARC_ERROR("No Vulkan physical devices");
                return false;
            }
            m_physicalDevice = physicalDevices[0];
            for (auto& candidate : physicalDevices)
            {
                if (candidate.getProperties().deviceType ==
                    vk::PhysicalDeviceType::eDiscreteGpu)
                {
                    m_physicalDevice = candidate;
                    break;
                }
            }
            m_adapterName = std::string(
                m_physicalDevice.getProperties().deviceName.data());

            auto families = m_physicalDevice.getQueueFamilyProperties();
            for (uint32_t i = 0; i < (uint32_t)families.size(); ++i)
            {
                if (families[i].queueFlags & vk::QueueFlagBits::eGraphics)
                {
                    m_graphicsQueueFamily = (int)i;
                    break;
                }
            }
            if (m_graphicsQueueFamily < 0)
            {
                ARC_ERROR("No graphics queue family");
                return false;
            }

            const float priority = 1.0f;
            auto queueInfo = vk::DeviceQueueCreateInfo()
                .setQueueFamilyIndex((uint32_t)m_graphicsQueueFamily)
                .setQueueCount(1)
                .setPQueuePriorities(&priority);

            // NVRHI's Vulkan queue submits with vk::TimelineSemaphoreSubmitInfo
            // (vulkan-queue.cpp) -- timelineSemaphore is a hard requirement.
            // NVRHI also calls vkCmdPipelineBarrier2 (Vulkan 1.3 core /
            // KHR_synchronization2) -- synchronization2 must be enabled or the
            // validation layer fires on every command list barrier.
            auto vulkan13Features = vk::PhysicalDeviceVulkan13Features()
                .setSynchronization2(true);

            auto vulkan12Features = vk::PhysicalDeviceVulkan12Features()
                .setTimelineSemaphore(true)
                .setPNext(&vulkan13Features);

            auto deviceInfo = vk::DeviceCreateInfo()
                .setQueueCreateInfoCount(1)
                .setPQueueCreateInfos(&queueInfo)
                .setEnabledExtensionCount((uint32_t)std::size(kDeviceExtensions))
                .setPpEnabledExtensionNames(kDeviceExtensions)
                .setPNext(&vulkan12Features);

            try
            {
                m_device = m_physicalDevice.createDevice(deviceInfo);
            }
            catch (const vk::SystemError& e)
            {
                ARC_ERROR("vk::PhysicalDevice::createDevice failed: {}", e.what());
                return false;
            }

            // The per-module default dispatcher binds ONE VkDevice: a second
            // simultaneous Vulkan device would re-point the device-level
            // entry points and corrupt the first. Fine by design -- one
            // engine instance per process (see VulkanDispatchStorage.cpp).
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device);
            m_graphicsQueue = m_device.getQueue((uint32_t)m_graphicsQueueFamily, 0);

            nvrhi::vulkan::DeviceDesc nvrhiDesc;
            nvrhiDesc.errorCB = &NvrhiMessageCallback::Instance();
            nvrhiDesc.instance = m_instance;
            nvrhiDesc.physicalDevice = m_physicalDevice;
            nvrhiDesc.device = m_device;
            nvrhiDesc.graphicsQueue = m_graphicsQueue;
            nvrhiDesc.graphicsQueueIndex = m_graphicsQueueFamily;
            nvrhiDesc.instanceExtensions = kInstanceExtensions;
            nvrhiDesc.numInstanceExtensions = std::size(kInstanceExtensions);
            nvrhiDesc.deviceExtensions = kDeviceExtensions;
            nvrhiDesc.numDeviceExtensions = std::size(kDeviceExtensions);

            m_nvrhiBackend = nvrhi::vulkan::createDevice(nvrhiDesc);
            if (!m_nvrhiBackend)
            {
                ARC_ERROR("nvrhi::vulkan::createDevice failed");
                return false;
            }

            m_nvrhi = m_nvrhiBackend;
            if (desc.enableValidation)
                m_nvrhi = nvrhi::validation::createValidationLayer(m_nvrhi);

            ARC_INFO("Vulkan device created on '{}'", m_adapterName);
            return true;
        }

        DeviceVulkan::~DeviceVulkan()
        {
            if (m_nvrhi)
            {
                m_nvrhi->waitForIdle();
                m_nvrhi->runGarbageCollection();
            }
            m_nvrhi = nullptr;
            m_nvrhiBackend = nullptr;
            if (m_device)
                m_device.destroy();
            if (m_instance)
                m_instance.destroy();
        }
    }

    std::unique_ptr<RenderDevice> CreateDeviceVulkan(const RenderDeviceDesc& desc)
    {
        try
        {
            // DynamicLoader's constructor throws when vulkan-1.dll is
            // absent; honor RenderDevice::Create's nullptr-on-failure
            // contract on Vulkan-less hosts.
            auto device = std::make_unique<DeviceVulkan>();
            if (!device->Init(desc))
                return nullptr;
            return device;
        }
        catch (const std::exception& e)
        {
            ARC_ERROR("Vulkan device creation failed: {}", e.what());
            return nullptr;
        }
    }
}
