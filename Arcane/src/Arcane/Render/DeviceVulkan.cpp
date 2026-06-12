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

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>

#include <vulkan/vulkan.hpp>

#include <algorithm>
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

        constexpr nvrhi::Format kSwapchainFormat    = nvrhi::Format::BGRA8_UNORM;
        constexpr vk::Format    kSwapchainFormatVk  = vk::Format::eB8G8R8A8Unorm;

        // Routes VK validation-layer output into the SAME latch as NVRHI
        // diagnostics: errors increment RenderErrorCount(), so a raw VUID
        // fails the GPU tests exactly like an [nvrhi] error would.
        VKAPI_ATTR VkBool32 VKAPI_CALL VkDebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT /*types*/,
            const VkDebugUtilsMessengerCallbackDataEXT* data,
            void* /*userData*/)
        {
            const char* text = (data && data->pMessage) ? data->pMessage : "";
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                NvrhiMessageCallback::Instance().message(
                    nvrhi::MessageSeverity::Error, text);
            else
                ARC_WARN("[vk] {}", text);
            return VK_FALSE;
        }

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

            std::unique_ptr<Swapchain> CreateSwapchain(Window& window,
                                                       bool vsync) override;

        private:
            vk::detail::DynamicLoader        m_loader;  // must outlive everything below
            vk::Instance                     m_instance;
            vk::DebugUtilsMessengerEXT       m_debugMessenger;
            vk::PhysicalDevice               m_physicalDevice;
            vk::Device         m_device;
            vk::Queue          m_graphicsQueue;
            int                m_graphicsQueueFamily = -1;
            nvrhi::DeviceHandle m_nvrhiBackend;  // the real vulkan device
            nvrhi::DeviceHandle m_nvrhi;         // possibly validation-wrapped
            std::string         m_adapterName;
        };

        // ----------------------------------------------------------------
        // Vulkan KHR swapchain: SDL surface, acquire/present semaphore
        // bridge into NVRHI, resize/out-of-date handling.
        // M2 pacing: kSwapchainFramesInFlight slots, EventQuery-gated.
        // ----------------------------------------------------------------

        class SwapchainVulkan final : public Swapchain
        {
        public:
            ~SwapchainVulkan() override;
            bool Init(DeviceVulkan& device, Window& window, bool vsync);

            nvrhi::ITexture* BeginFrame() override;
            void Present() override;
            void Resize(uint32_t width, uint32_t height) override;
            uint32_t Width() const override { return m_width; }
            uint32_t Height() const override { return m_height; }
            nvrhi::Format Format() const override { return kSwapchainFormat; }

        private:
            bool CreateSwapchainObjects();
            void ReleaseBackbufferHandles();

            DeviceVulkan* m_device = nullptr;
            Window*        m_window = nullptr;
            vk::SurfaceKHR m_surface;
            vk::SwapchainKHR m_swapchain;
            std::vector<nvrhi::TextureHandle> m_backbuffers;
            // Acquire semaphores are indexed by frame slot: they are consumed
            // (waited on) by the GPU in the same frame, and the EventQuery
            // ensures that submission is done before we reuse the slot.
            vk::Semaphore m_acquireSemaphores[kSwapchainFramesInFlight];
            // Present semaphores are indexed by swapchain image index: the
            // presentation engine holds them asynchronously until the image
            // is released, so slot-based reuse would fire VUID-00067.
            std::vector<vk::Semaphore> m_presentSemaphores;
            nvrhi::EventQueryHandle m_frameQueries[kSwapchainFramesInFlight];
            uint64_t m_frameCounter = 0;
            uint32_t m_currentImage = 0;
            uint32_t m_width = 0;
            uint32_t m_height = 0;
            bool m_vsync = true;
            bool m_acquired = false;
        };

        bool SwapchainVulkan::Init(DeviceVulkan& device, Window& window, bool vsync)
        {
            m_device = &device;
            m_window = &window;
            m_vsync = vsync;
            window.GetPixelSize(m_width, m_height);

            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(window.SdlWindow(), device.Instance(),
                                          nullptr, &surface))
            {
                ARC_ERROR("SDL_Vulkan_CreateSurface failed: {} "
                          "(was the window created with WindowDesc::vulkan?)",
                          SDL_GetError());
                return false;
            }
            m_surface = surface;

            if (!device.PhysicalDevice().getSurfaceSupportKHR(
                    device.GraphicsQueueFamily(), m_surface))
            {
                ARC_ERROR("Graphics queue family cannot present to this surface");
                return false;
            }

            for (uint32_t i = 0; i < kSwapchainFramesInFlight; ++i)
            {
                m_acquireSemaphores[i] = device.Device().createSemaphore({});
                m_frameQueries[i] = device.Nvrhi()->createEventQuery();
            }
            // Present semaphores are per-image and created in CreateSwapchainObjects.

            return CreateSwapchainObjects();
        }

        bool SwapchainVulkan::CreateSwapchainObjects()
        {
            vk::PhysicalDevice physical = m_device->PhysicalDevice();
            auto caps = physical.getSurfaceCapabilitiesKHR(m_surface);

            vk::Extent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX)  // surface lets us choose
            {
                extent.width = std::clamp(m_width, caps.minImageExtent.width,
                                          caps.maxImageExtent.width);
                extent.height = std::clamp(m_height, caps.minImageExtent.height,
                                           caps.maxImageExtent.height);
            }
            m_width = extent.width;
            m_height = extent.height;
            if (m_width == 0 || m_height == 0)
                return true;  // minimized; BeginFrame skips until restored

            vk::ColorSpaceKHR colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
            bool formatFound = false;
            for (const auto& format : physical.getSurfaceFormatsKHR(m_surface))
            {
                if (format.format == kSwapchainFormatVk)
                {
                    colorSpace = format.colorSpace;
                    formatFound = true;
                    break;
                }
            }
            if (!formatFound)
            {
                ARC_ERROR("Surface does not support B8G8R8A8_UNORM");
                return false;
            }

            vk::PresentModeKHR presentMode = vk::PresentModeKHR::eFifo;  // vsync
            if (!m_vsync)
            {
                presentMode = vk::PresentModeKHR::eImmediate;
                for (auto mode : physical.getSurfacePresentModesKHR(m_surface))
                {
                    if (mode == vk::PresentModeKHR::eMailbox)
                    {
                        presentMode = vk::PresentModeKHR::eMailbox;
                        break;
                    }
                }
            }

            uint32_t imageCount = std::max(3u, caps.minImageCount);
            if (caps.maxImageCount != 0)
                imageCount = std::min(imageCount, caps.maxImageCount);

            auto swapchainInfo = vk::SwapchainCreateInfoKHR()
                .setSurface(m_surface)
                .setMinImageCount(imageCount)
                .setImageFormat(kSwapchainFormatVk)
                .setImageColorSpace(colorSpace)
                .setImageExtent(extent)
                .setImageArrayLayers(1)
                .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment |
                               vk::ImageUsageFlagBits::eTransferDst)
                .setImageSharingMode(vk::SharingMode::eExclusive)
                .setPreTransform(caps.currentTransform)
                .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
                .setPresentMode(presentMode)
                .setClipped(true)
                .setOldSwapchain(m_swapchain);

            vk::SwapchainKHR newSwapchain =
                m_device->Device().createSwapchainKHR(swapchainInfo);
            if (m_swapchain)
                m_device->Device().destroySwapchainKHR(m_swapchain);
            m_swapchain = newSwapchain;

            auto images = m_device->Device().getSwapchainImagesKHR(m_swapchain);
            m_backbuffers.clear();
            m_backbuffers.reserve(images.size());
            for (size_t i = 0; i < images.size(); ++i)
            {
                auto texDesc = nvrhi::TextureDesc()
                    .setWidth(m_width)
                    .setHeight(m_height)
                    .setFormat(kSwapchainFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::Present)
                    .setKeepInitialState(true)
                    .setDebugName("SwapchainImage");
                nvrhi::TextureHandle handle =
                    m_device->Nvrhi()->createHandleForNativeTexture(
                        nvrhi::ObjectTypes::VK_Image,
                        nvrhi::Object((VkImage)images[i]), texDesc);
                if (!handle)
                {
                    ARC_ERROR("createHandleForNativeTexture failed for image {}", i);
                    return false;
                }
                m_backbuffers.push_back(handle);
            }

            // One present semaphore per swapchain image: the presentation
            // engine holds a semaphore asynchronously until the image is
            // released, so we cannot index by frame slot (VUID-00067).
            // Destroy any semaphores from a previous swapchain rebuild first.
            for (auto& sem : m_presentSemaphores)
                m_device->Device().destroySemaphore(sem);
            m_presentSemaphores.clear();
            m_presentSemaphores.reserve(images.size());
            for (size_t i = 0; i < images.size(); ++i)
                m_presentSemaphores.push_back(m_device->Device().createSemaphore({}));

            return true;
        }

        void SwapchainVulkan::ReleaseBackbufferHandles()
        {
            m_device->Nvrhi()->waitForIdle();
            m_backbuffers.clear();
            for (auto& sem : m_presentSemaphores)
                m_device->Device().destroySemaphore(sem);
            m_presentSemaphores.clear();
            m_device->Nvrhi()->runGarbageCollection();
        }

        nvrhi::ITexture* SwapchainVulkan::BeginFrame()
        {
            if (m_acquired)
            {
                // Double BeginFrame without Present: re-acquiring would reuse
                // the already-signaled binary semaphore (VUID 01286). Hand
                // back the image we already hold.
                return m_backbuffers[m_currentImage];
            }
            if (m_width == 0 || m_height == 0 || m_backbuffers.empty())
                return nullptr;

            const uint32_t slot = (uint32_t)(m_frameCounter % kSwapchainFramesInFlight);

            // Slot gating: frame N-2's submits (which consumed this slot's
            // acquire semaphore and queued its present signal) must have
            // retired before the slot's binary semaphores are reused.
            if (m_frameCounter >= kSwapchainFramesInFlight)
            {
                m_device->Nvrhi()->waitEventQuery(m_frameQueries[slot]);
                m_device->Nvrhi()->resetEventQuery(m_frameQueries[slot]);
            }

            try
            {
                auto acquired = m_device->Device().acquireNextImageKHR(
                    m_swapchain, UINT64_MAX, m_acquireSemaphores[slot], nullptr);
                if (acquired.result != vk::Result::eSuccess &&
                    acquired.result != vk::Result::eSuboptimalKHR)
                    return nullptr;
                m_currentImage = acquired.value;
            }
            catch (const vk::OutOfDateKHRError&)
            {
                // Surface changed under us: rebuild at the current size and
                // skip this frame. (A throwing acquire does NOT signal the
                // semaphore -- safe to reuse.)
                // (m_acquired is false here -- a held image returns at the
                // top -- so no pending acquire-wait needs draining.)
                m_device->Nvrhi()->waitForIdle();
                ReleaseBackbufferHandles();
                CreateSwapchainObjects();
                return nullptr;
            }

            m_device->VulkanNvrhi()->queueWaitForSemaphore(
                nvrhi::CommandQueue::Graphics, m_acquireSemaphores[slot], 0);
            m_acquired = true;
            return m_backbuffers[m_currentImage];
        }

        void SwapchainVulkan::Present()
        {
            if (!m_acquired)
                return;
            m_acquired = false;

            const uint32_t slot = (uint32_t)(m_frameCounter % kSwapchainFramesInFlight);

            // Present semaphore is per-image (not per-slot): the presentation
            // engine holds it asynchronously; slot-based reuse fires VUID-00067.
            m_device->VulkanNvrhi()->queueSignalSemaphore(
                nvrhi::CommandQueue::Graphics, m_presentSemaphores[m_currentImage], 0);
            // Empty submit flushes the queued semaphore signal. Must go
            // through the UNWRAPPED device: the validation wrapper
            // short-circuits executeCommandLists when numCommandLists == 0
            // and would leave the signal un-submitted.
            m_device->VulkanNvrhi()->executeCommandLists(nullptr, 0);

            auto presentInfo = vk::PresentInfoKHR()
                .setWaitSemaphoreCount(1)
                .setPWaitSemaphores(&m_presentSemaphores[m_currentImage])
                .setSwapchainCount(1)
                .setPSwapchains(&m_swapchain)
                .setPImageIndices(&m_currentImage);
            try
            {
                (void)m_device->GraphicsQueue().presentKHR(presentInfo);
            }
            catch (const vk::OutOfDateKHRError&)
            {
                // Rebuilt on the next BeginFrame/Resize.
            }

            // Completion point for this slot: covers the flush submit above,
            // i.e. every queue operation of this frame (the present signal is
            // folded into the flush submit; scan-out itself is outside the timeline).
            m_device->Nvrhi()->setEventQuery(m_frameQueries[slot],
                                             nvrhi::CommandQueue::Graphics);
            ++m_frameCounter;

            m_device->Nvrhi()->runGarbageCollection();
        }

        void SwapchainVulkan::Resize(uint32_t width, uint32_t height)
        {
            if (width == m_width && height == m_height)
                return;
            m_width = width;
            m_height = height;
            ReleaseBackbufferHandles();
            CreateSwapchainObjects();
        }

        SwapchainVulkan::~SwapchainVulkan()
        {
            if (!m_device)
                return;
            // If an acquire-wait was queued (BeginFrame) but never consumed
            // by a Present submit, drain it now: nvrhi's Queue outlives this
            // swapchain and waitForIdle does NOT clear pending wait
            // semaphores -- only a submit does. Without this, the destroyed
            // semaphore attaches to the queue's next submit (use-after-free).
            if (m_acquired)
            {
                m_device->VulkanNvrhi()->executeCommandLists(nullptr, 0);
                m_acquired = false;
            }
            // ReleaseBackbufferHandles drains the GPU, clears backbuffers,
            // and destroys per-image present semaphores.
            ReleaseBackbufferHandles();
            if (m_swapchain)
                m_device->Device().destroySwapchainKHR(m_swapchain);
            for (uint32_t i = 0; i < kSwapchainFramesInFlight; ++i)
            {
                if (m_acquireSemaphores[i])
                    m_device->Device().destroySemaphore(m_acquireSemaphores[i]);
            }
            if (m_surface)
                m_device->Instance().destroySurfaceKHR(m_surface);
        }

        std::unique_ptr<Swapchain> DeviceVulkan::CreateSwapchain(Window& window,
                                                                 bool vsync)
        {
            auto swapchain = std::make_unique<SwapchainVulkan>();
            if (!swapchain->Init(*this, window, vsync))
                return nullptr;
            return swapchain;
        }

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

            std::vector<const char*> instanceExtensions(
                std::begin(kInstanceExtensions), std::end(kInstanceExtensions));
            bool debugUtils = false;
            if (desc.enableValidation)
            {
                for (const auto& ext : vk::enumerateInstanceExtensionProperties())
                {
                    if (std::string_view(ext.extensionName) ==
                        VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
                    {
                        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                        debugUtils = true;
                        break;
                    }
                }
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
                    .setEnabledExtensionCount((uint32_t)instanceExtensions.size())
                    .setPpEnabledExtensionNames(instanceExtensions.data());

                m_instance = vk::createInstance(instanceInfo);
            }
            catch (const vk::SystemError& e)
            {
                ARC_ERROR("vk::createInstance failed: {}", e.what());
                return false;
            }

            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);

            if (debugUtils)
            {
                using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
                using Type = vk::DebugUtilsMessageTypeFlagBitsEXT;
                auto messengerInfo = vk::DebugUtilsMessengerCreateInfoEXT()
                    .setMessageSeverity(Severity::eError | Severity::eWarning)
                    .setMessageType(Type::eValidation | Type::eGeneral |
                                    Type::ePerformance)
                    .setPfnUserCallback(&VkDebugCallback);
                m_debugMessenger =
                    m_instance.createDebugUtilsMessengerEXT(messengerInfo);
            }

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
            nvrhiDesc.instanceExtensions = instanceExtensions.data();
            nvrhiDesc.numInstanceExtensions = instanceExtensions.size();
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
            if (m_debugMessenger)
                m_instance.destroyDebugUtilsMessengerEXT(m_debugMessenger);
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
