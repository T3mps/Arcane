// Vulkan backend: instance + physical device + device + graphics queue,
// wrapped by nvrhi::vulkan::createDevice. Uses the Vulkan-Hpp default
// dynamic dispatcher (storage TU: VulkanDispatchStorage.cpp). Headless
// here; the swapchain half of this TU arrives with the windowed task.
//
// NDEBUG alignment: NVRHI's Release build defines NDEBUG, which removes
// DispatchLoaderBase::m_valid and vkHeaderVersion from DispatchLoaderDynamic.
// All Arcane configs that link NVRHI must define NDEBUG in Release (done in
// premake5.lua) so both sides agree on the struct layout.

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/DeviceCreationVulkan.hpp>
#include <Arcane/Render/DeviceFactories.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/Swapchain.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>

#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    namespace
    {
        // ONE source for the API version: VkApplicationInfo below and the
        // minorVersion the NRI wrapper is handed must never drift apart
        // (NRI capability contract item 5 -- NRI reads that field verbatim in
        // wrapper mode and never re-queries the device).
        // TRIPWIRE: if this ever bumps to 1.4 (or beyond), fold an actual
        // vkEnumerateInstanceVersion query into the apiMinorVersion min()
        // below per §6.8 -- today's min(physical minor, kApiMinorVersion)
        // stands in for min(instance minor, physical minor) only because the
        // hard-fail guard right below that min() pins physicalApiMinor to
        // always be >= kApiMinorVersion for a 1.3 request.
        constexpr uint32_t kApiVersion      = VK_API_VERSION_1_3;
        constexpr uint32_t kApiMinorVersion = VK_API_VERSION_MINOR(kApiVersion);

        // Surface extensions are requested even for headless devices: they
        // cost nothing without a surface, and keep one code path for both
        // headless tests and windowed swapchains.
        const char* kInstanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            // NRI capability contract item 1 (hard): NRI's SwapChainVK::Create
            // calls GetPhysicalDeviceSurfaceFormats2KHR and
            // GetPhysicalDeviceSurfaceCapabilities2KHR through UNGUARDED
            // pointers, so its absence is a crash on the first swapchain.
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        };
        // F-5a: the REQUIRED device extensions. The optional GPU-crash
        // diagnostics extensions are appended to a copy of this list only when
        // the physical device actually enumerates them -- see F-5c's sweep in
        // Init. Requesting an unsupported device extension is a hard
        // vkCreateDevice failure, which is precisely what must never happen
        // for a diagnostics nicety.
        const char* kDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            // NRI capability contract item 2 (hard): NRI calls
            // vkCmdPushDescriptorSet unguarded for root descriptors and root
            // samplers, and reports rootDescriptorMaxNum = 0 without it.
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        };
        // NRI capability contract item 3 (quality): NRI takes a documented
        // slower path when these are absent (maintenance5/6 -> the v1 bind and
        // push forms; memory_budget -> VMA without budget awareness), so they
        // ride F-5c's availability sweep rather than the hard-required list
        // above -- by the same rule stated there, a driver that lacks
        // maintenance6 must not fail device creation over a quality nicety.
        //
        // Named indices, not bare subscripts: the maintenance pair also needs
        // its FEATURE struct chained at creation (see item 3's second half in
        // Init), and that code has to name the one it means.
        enum OptionalDeviceExtension
        {
            kOptMaintenance5,
            kOptMaintenance6,
            kOptMemoryBudget,   // pure list check -- no feature struct exists
            kOptionalDeviceExtensionCount
        };
        const char* kOptionalDeviceExtensions[kOptionalDeviceExtensionCount] = {
            VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
            VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
            VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        };
        // NRI capability contract item 9: VK_EXT_zero_initialize_device_memory
        // is deliberately NOT requested -- NRI's enableMemoryZeroInitialization
        // flips every image's initialLayout to ZERO_INITIALIZED_EXT and is only
        // legal with the extension enabled, so both stay off together.

        constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

        // F-3: the ONE device-removed observation point for this backend.
        // Both observables funnel here -- F-3b's cross-backend NVRHI message
        // hook (NVRHI catches vk::DeviceLostError on submit and reports
        // "Device Removed!", vulkan-queue.cpp:195-201) and F-3d's swapchain
        // sites, which until now let a DeviceLostError escape as an unhandled
        // C++ exception.
        //
        // Once-only per armed device: a lost device keeps reporting loss on
        // every submit and every present, and the second report is worthless
        // -- the marker buffer and fault state belong to the FIRST one. Reset
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
            // written). Same wording as the D3D12 backend. Do not reword.
            Diagnostics::WriteReport("gpu-crash: device removed");

            // AFTER the report, deliberately: hosts poll this latch and shut
            // down on it, and "observed" must always mean "the report exists".
            NoteGpuDeviceLost();
        }

        constexpr nvrhi::Format kSwapchainFormat    = nvrhi::Format::BGRA8_UNORM;
        constexpr vk::Format    kSwapchainFormatVk  = vk::Format::eB8G8R8A8Unorm;

        // Routes VK validation-layer output into the SAME latch as NVRHI
        // diagnostics: errors increment RenderErrorCount(), so a raw VUID
        // fails the GPU tests exactly like an [nvrhi] error would.
        // Typed vk:: signature (vk::PFN_DebugUtilsMessengerCallbackEXT), not the
        // raw C PFN_vkDebugUtilsMessengerCallbackEXT: setPfnUserCallback's
        // C-signature overload is deprecated in favor of this one.
        VKAPI_ATTR vk::Bool32 VKAPI_CALL VkDebugCallback(
            vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
            vk::DebugUtilsMessageTypeFlagsEXT /*types*/,
            const vk::DebugUtilsMessengerCallbackDataEXT* data,
            void* /*userData*/)
        {
            const char* text = (data && data->pMessage) ? data->pMessage : "";
            if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
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
            std::string AdapterName() const override { return m_creation.adapterName; }

            vk::Instance Instance() const { return m_creation.instance; }
            vk::PhysicalDevice PhysicalDevice() const { return m_creation.physicalDevice; }
            vk::Device Device() const { return m_creation.device; }
            vk::Queue GraphicsQueue() const { return m_creation.graphicsQueue; }
            uint32_t GraphicsQueueFamily() const { return (uint32_t)m_creation.graphicsQueueFamily; }

            // NRI capability contract items 5 + 6: the three values a wrapper
            // desc may not guess. minorVersion is the created API minor, and
            // the two lists are the FILTERED ones the device was actually
            // created with (NRI takes both verbatim in wrapper mode).
            uint32_t ApiMinorVersion() const { return m_creation.apiMinorVersion; }
            const std::vector<const char*>& EnabledInstanceExtensions() const
            {
                return m_creation.enabledInstanceExtensions;
            }
            const std::vector<const char*>& EnabledDeviceExtensions() const
            {
                return m_creation.enabledDeviceExtensions;
            }

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
            // Task 7: the creation half -- loader, instance, messenger,
            // physical device, device, queue, family index, API minor, the
            // filtered extension lists, adapter name, and the GPU-crash
            // record. It is FIRST for the same reason m_loader was: it owns
            // the Vulkan loader module, which must outlive every handle
            // (including everything NVRHI holds) and therefore has to be
            // destroyed last. Same values, same lifetimes, one owner --
            // DeviceCreationVulkan.hpp says what each field is for.
            VulkanDeviceCreation m_creation;
            nvrhi::DeviceHandle m_nvrhiBackend;  // the real vulkan device
            nvrhi::DeviceHandle m_nvrhi;         // possibly validation-wrapped
            // Holds a VkBuffer + VkDeviceMemory created off m_creation.device,
            // so it must be released BEFORE that device is destroyed --
            // ~DeviceVulkan resets it explicitly rather than relying on member
            // order, because that destructor tears the device down in its BODY.
            std::unique_ptr<IGpuCrashBackend> m_crashBackend;
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
            GpuFrameSlot m_frameSlots[kSwapchainFramesInFlight];
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
                if (!m_frameSlots[i].Init(device.Nvrhi()))
                {
                    ARC_ERROR("Swapchain: frame-slot event query creation failed");
                    return false;
                }
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
                // Same completion semantics as the waitEventQuery this replaced
                // -- never returns early -- but a STAMPED slot is waited on by
                // polling, republishing the diagnostics beats, which is what
                // makes a wedged GPU observable as `gpu-stall` instead of
                // parking the main thread where the watchdog cannot see it.
                //
                // The UNSTAMPED case is not theoretical here, it is THIS
                // function: the acquire below throws OutOfDateKHRError on an
                // ordinary window resize and bails without advancing
                // m_frameCounter, so Present -- the only stamp site -- is
                // skipped, and the next BeginFrame meets this same slot holding
                // a query nvrhi reports as incomplete forever. GpuFrameSlot
                // takes the instant wait for that case. ONE shared
                // implementation with the D3D12 path, deliberately.
                m_frameSlots[slot].WaitAndReset(m_device->Nvrhi());
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
            catch (const vk::DeviceLostError&)
            {
                // F-3d: acquireNextImageKHR caught ONLY OutOfDateKHRError, so
                // a VK_ERROR_DEVICE_LOST here left Present()'s caller staring
                // at an unhandled C++ exception and no report at all. Capture
                // instead. Once-guarded inside, and a no-op if F-3b's
                // submit-side hook already fired for the same loss. Log-once
                // for the same reason as the D3D12 Present site: repeats
                // between observation and the host's latch-driven exit are
                // pure spam.
                if (!g_deviceRemovedReported.load(std::memory_order_acquire))
                    ARC_ERROR("acquireNextImageKHR failed: device lost");
                ObserveDeviceRemoved();
                return nullptr;
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
            catch (const vk::DeviceLostError&)
            {
                // F-3d: presentKHR caught ONLY OutOfDateKHRError, so a
                // DeviceLostError from present escaped Present() entirely --
                // Vulkan's analogue of F-3c's D3D12 present-path observable,
                // which the D3D12 side already routes into capture. Same
                // treatment here: log (once), then collect while markers and
                // fault state still describe THIS loss.
                if (!g_deviceRemovedReported.load(std::memory_order_acquire))
                    ARC_ERROR("presentKHR failed: device lost");
                ObserveDeviceRemoved();
            }
            catch (const vk::OutOfDateKHRError&)
            {
                // Rebuilt on the next BeginFrame/Resize.
            }

            // Completion point for this slot: covers the flush submit above,
            // i.e. every queue operation of this frame (the present signal is
            // folded into the flush submit; scan-out itself is outside the timeline).
            m_frameSlots[slot].Stamp(m_device->Nvrhi(), nvrhi::CommandQueue::Graphics);
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
    }

    // ----------------------------------------------------------------
    // The CREATION HALF (NRI Phase 1, Task 7).
    // ----------------------------------------------------------------
    // This function IS the former prologue of DeviceVulkan::Init, moved out
    // whole so the NRI wrapper can reuse it: the same calls in the same order
    // with the same parameters, the same log lines, and the same early
    // returns. The only textual change is that what used to be written into
    // DeviceVulkan's members is written into `out` -- which is now where
    // DeviceVulkan keeps them anyway (m_creation), so the NVRHI path reads
    // exactly the values it read before.
    //
    // It sits at namespace scope (outside the anonymous namespace above)
    // because DeviceCreationVulkan.hpp declares it for the other consumers;
    // DeviceVulkan's own out-of-class member definitions follow it, still
    // internal-linkage by virtue of their class.
    //
    // Failure leaves `out` holding whatever was created; the caller's
    // teardown (DestroyVulkanNativeDevice, reached through ~DeviceVulkan or
    // NativeDeviceOwner) releases it, exactly as ~DeviceVulkan always did
    // when Init bailed. Exceptions still propagate -- CreateDeviceVulkan's
    // catch below is unchanged and is still the one that reports them.
    bool CreateVulkanNativeDevice(const RenderDeviceDesc& desc, VulkanDeviceCreation& out)
    {
        // Recorded, not acted on: NRI's own validation layer is available in
        // wrapper mode (contract 1.1) and keys off the same switch the
        // instance's VK_LAYER_KHRONOS_validation below does. Pure member
        // write -- no call, no branch, nothing the NVRHI path can observe.
        out.enableValidation = desc.enableValidation;

        auto vkGetInstanceProcAddr =
            out.loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
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

        // Enumerated once and shared by the two checks below; previously
        // this call sat inside the validation-only debug_utils loop.
        const auto availableInstanceExtensions =
            vk::enumerateInstanceExtensionProperties();
        const auto instanceExtensionAvailable = [&](std::string_view wanted) {
            for (const auto& ext : availableInstanceExtensions)
            {
                if (std::string_view(ext.extensionName) == wanted)
                    return true;
            }
            return false;
        };

        // NRI capability contract item 1: a hard-required instance
        // extension, named here rather than left to vk::createInstance to
        // report as a bare ErrorExtensionNotPresent (contract item 14's
        // rule -- the message must say WHICH capability is missing).
        if (!instanceExtensionAvailable(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
        {
            ARC_ERROR("Vulkan instance extension '{}' is unavailable; it is "
                      "hard-required (NRI wrapper capability contract item 1)",
                      VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            return false;
        }

        bool debugUtils = false;
        if (desc.enableValidation &&
            instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debugUtils = true;
        }

        // OPT-IN, DEFAULT OFF, AND INERT TO THE NVRHI BOOT: synchronization
        // validation. `RenderDeviceDesc::enableSyncValidation` is false for
        // every engine path -- RenderDevice::Create's callers all leave it
        // defaulted -- so with the flag off this block is one false branch
        // and the instance is created byte-identically to before
        // (`syncFeatures` stays unreferenced and `instanceInfo.pNext` stays
        // null). Its one caller is the `--nri-graph` frame-graph vehicle,
        // which needs the hazard checks core validation does not perform:
        // catching a missing or misplaced barrier on the graph's derived
        // barrier chain, which is silent to core validation.
        //
        // Mechanism: VK_EXT_validation_features + a VkValidationFeaturesEXT
        // chained onto VkInstanceCreateInfo, the layer-configuration path the
        // Vulkan SDK documents for turning sync validation on from inside the
        // application (rather than via vk_layer_settings.txt or the
        // VK_LAYER_* environment variables, neither of which a scripted desk
        // command can rely on being set). Every miss degrades with a WARN --
        // losing a diagnostic must never fail device creation.
        bool syncValidation = false;
        // Both must outlive vk::createInstance below (the struct is reachable
        // from instanceInfo.pNext, and it points at this array) -- hence
        // function scope rather than inside the `if`.
        const vk::ValidationFeatureEnableEXT syncFeatureList[] = {
            vk::ValidationFeatureEnableEXT::eSynchronizationValidation
        };
        vk::ValidationFeaturesEXT syncFeatures;
        if (desc.enableSyncValidation)
        {
            if (layers.empty())
            {
                ARC_WARN("sync validation requested but {} is not loaded; "
                         "continuing without it", kValidationLayer);
            }
            else
            {
                // ENUMERATE WITH THE LAYER NAME. VK_EXT_validation_features is
                // provided by VK_LAYER_KHRONOS_validation, an EXPLICIT layer --
                // no ICD implements it. Per the Vulkan spec,
                // vkEnumerateInstanceExtensionProperties(pLayerName = NULL) --
                // which is what the `instanceExtensionAvailable` helper above
                // wraps -- returns ONLY extensions provided by the Vulkan
                // implementation and by IMPLICIT layers. Asking it about this
                // extension therefore answers "unavailable" on every stock SDK
                // machine, silently dropping sync validation while the vehicle
                // still exits clean and proves nothing. DO NOT re-route this
                // check through `instanceExtensionAvailable`.
                //
                // Requesting a layer-provided extension in the SAME
                // vkCreateInstance call that enables the layer is legal
                // precisely because `layers` is non-empty on this branch.
                bool featuresExtAvailable = false;
                try
                {
                    const std::string layerName(kValidationLayer);
                    for (const auto& ext : vk::enumerateInstanceExtensionProperties(layerName))
                    {
                        if (std::string_view(ext.extensionName) == VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)
                        {
                            featuresExtAvailable = true;
                            break;
                        }
                    }
                }
                catch (const vk::SystemError& e)
                {
                    // Catch-and-degrade, matching the layer enumeration above.
                    // This call sits OUTSIDE the vk::createInstance try below,
                    // whose catch returns false -- letting a throw reach that
                    // one would fail device creation over a diagnostic, which
                    // is exactly what this block must never do.
                    ARC_WARN("vk::enumerateInstanceExtensionProperties('{}') threw: {}; "
                             "continuing without sync validation", kValidationLayer, e.what());
                }

                if (!featuresExtAvailable)
                {
                    ARC_WARN("sync validation requested but layer '{}' does not provide '{}'; "
                             "continuing without it", kValidationLayer,
                             VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                }
                else
                {
                    instanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                    syncFeatures.setEnabledValidationFeatureCount(1)
                                .setPEnabledValidationFeatures(syncFeatureList);
                    syncValidation = true;
                }
            }
        }

        try
        {
            auto appInfo = vk::ApplicationInfo()
                .setPApplicationName("Arcane")
                .setApiVersion(kApiVersion);

            auto instanceInfo = vk::InstanceCreateInfo()
                .setPApplicationInfo(&appInfo)
                .setEnabledLayerCount((uint32_t)layers.size())
                .setPpEnabledLayerNames(layers.data())
                .setEnabledExtensionCount((uint32_t)instanceExtensions.size())
                .setPpEnabledExtensionNames(instanceExtensions.data());
            // Set only when the opt-in above actually took: an untouched
            // pNext is what every pre-existing caller produces.
            if (syncValidation)
            {
                instanceInfo.setPNext(&syncFeatures);
                ARC_INFO("Vulkan synchronization validation ENABLED");
            }

            out.instance = vk::createInstance(instanceInfo);
        }
        catch (const vk::SystemError& e)
        {
            ARC_ERROR("vk::createInstance failed: {}", e.what());
            return false;
        }

        VULKAN_HPP_DEFAULT_DISPATCHER.init(out.instance);

        if (debugUtils)
        {
            using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
            using Type = vk::DebugUtilsMessageTypeFlagBitsEXT;
            auto messengerInfo = vk::DebugUtilsMessengerCreateInfoEXT()
                .setMessageSeverity(Severity::eError | Severity::eWarning)
                .setMessageType(Type::eValidation | Type::eGeneral |
                                Type::ePerformance)
                .setPfnUserCallback(&VkDebugCallback);
            out.debugMessenger =
                out.instance.createDebugUtilsMessengerEXT(messengerInfo);
        }

        std::vector<vk::PhysicalDevice> physicalDevices;
        try
        {
            physicalDevices = out.instance.enumeratePhysicalDevices();
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
        out.physicalDevice = physicalDevices[0];
        for (auto& candidate : physicalDevices)
        {
            if (candidate.getProperties().deviceType ==
                vk::PhysicalDeviceType::eDiscreteGpu)
            {
                out.physicalDevice = candidate;
                break;
            }
        }
        out.adapterName = std::string(
            out.physicalDevice.getProperties().deviceName.data());

        // NRI capability contract item 5. Two halves:
        //   (a) VK 1.3 was already an implicit requirement -- chaining
        //       VkPhysicalDeviceVulkan13Features below is illegal on an
        //       older device -- but nothing asserted it, so a 1.2 GPU
        //       failed as an opaque createDevice error. Say it plainly.
        //   (b) the minor recorded here is what the wrapper desc must
        //       carry: NRI reads minorVersion verbatim (it drives promoted-
        //       feature chaining AND VMA's vulkanApiVersion) and the
        //       correct value is min(instance minor, physical minor)
        //       -- contract §6 concern 8.
        const uint32_t physicalApiMinor =
            VK_API_VERSION_MINOR(out.physicalDevice.getProperties().apiVersion);
        if (physicalApiMinor < kApiMinorVersion)
        {
            ARC_ERROR("Vulkan physical device '{}' reports API 1.{}; 1.{} is "
                      "required (Vulkan 1.3 core feature structs are chained "
                      "at device creation)",
                      out.adapterName, physicalApiMinor, kApiMinorVersion);
            return false;
        }
        out.apiMinorVersion = std::min(physicalApiMinor, kApiMinorVersion);

        auto families = out.physicalDevice.getQueueFamilyProperties();
        for (uint32_t i = 0; i < (uint32_t)families.size(); ++i)
        {
            if (families[i].queueFlags & vk::QueueFlagBits::eGraphics)
            {
                out.graphicsQueueFamily = (int)i;
                break;
            }
        }
        if (out.graphicsQueueFamily < 0)
        {
            ARC_ERROR("No graphics queue family");
            return false;
        }

        // ------------------------------------------------------------
        // F-5c step 1+2: device-extension availability sweep.
        // ------------------------------------------------------------
        // Device extensions were previously passed through UNFILTERED --
        // fine for VK_KHR_swapchain (required anyway; its absence should
        // fail loudly) but fatal as a policy for optional diagnostics.
        // This mirrors the instance-extension shape already used above.
        std::vector<const char*> deviceExtensions(
            std::begin(kDeviceExtensions), std::end(kDeviceExtensions));

        bool haveBufferMarker  = false;
        bool haveDeviceFaultExt = false;
        bool haveDeviceFaultKhr = false;
        // Contract items 2 + 3 ride the SAME enumeration -- one pass, and
        // the diagnostics branches below are untouched.
        bool havePushDescriptor = false;
        bool haveOptional[kOptionalDeviceExtensionCount] = {};
        bool deviceExtensionsEnumerated = true;
        try
        {
            for (const auto& ext : out.physicalDevice.enumerateDeviceExtensionProperties())
            {
                const std::string_view name(ext.extensionName);
                if (name == VK_AMD_BUFFER_MARKER_EXTENSION_NAME)  haveBufferMarker   = true;
                // F-5d lists only the EXT spelling, but the KHR promotion
                // is in the vendored headers too (vulkan_core.h:14372) and
                // a newer driver may expose only that one. Accept EITHER;
                // the feature struct and the query differ per spelling, so
                // which one won is recorded and carried to the backend.
                else if (name == VK_EXT_DEVICE_FAULT_EXTENSION_NAME) haveDeviceFaultExt = true;
                else if (name == VK_KHR_DEVICE_FAULT_EXTENSION_NAME) haveDeviceFaultKhr = true;

                // NRI capability contract item 2: hard-required, and
                // already in kDeviceExtensions -- this only decides whether
                // the failure is named or an opaque createDevice error.
                if (name == VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)
                    havePushDescriptor = true;
                // NRI capability contract item 3: quality extensions, taken
                // only where advertised (see kOptionalDeviceExtensions).
                for (int i = 0; i < kOptionalDeviceExtensionCount; ++i)
                {
                    if (name == kOptionalDeviceExtensions[i])
                        haveOptional[i] = true;
                }
            }
        }
        catch (const vk::SystemError& e)
        {
            ARC_WARN("vk::PhysicalDevice::enumerateDeviceExtensionProperties threw: {}; "
                     "continuing without GPU-crash diagnostics extensions", e.what());
            // The item-2 check below is a better error message, not the
            // enforcement: VK_KHR_push_descriptor stays in the required
            // list, so createDevice still fails loudly on a device without
            // it. Suppress the named check rather than reject a device we
            // simply could not enumerate -- that is a regression this TU
            // has never had.
            deviceExtensionsEnumerated = false;
        }

        // NRI capability contract item 2 (contract item 14's rule: name the
        // capability). NRI calls vkCmdPushDescriptorSet through a pointer
        // it never null-checks, and the frame-graph design puts per-frame
        // data behind CmdSetRootDescriptor -- this is load-bearing.
        if (deviceExtensionsEnumerated && !havePushDescriptor)
        {
            ARC_ERROR("Vulkan device extension '{}' is unavailable on '{}'; it is "
                      "hard-required (NRI wrapper capability contract item 2)",
                      VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, out.adapterName);
            return false;
        }

        // NRI capability contract item 3: absence is a documented slower
        // path inside NRI, never a failure -- so append, don't require.
        // haveOptional[] stays the single predicate: what lands in
        // deviceExtensions here is exactly what gets its feature struct
        // chained below, so the list can never claim a capability the
        // device was not actually created with.
        for (int i = 0; i < kOptionalDeviceExtensionCount; ++i)
        {
            if (haveOptional[i])
                deviceExtensions.push_back(kOptionalDeviceExtensions[i]);
            else
                ARC_INFO("Vulkan quality extension '{}' unavailable; NRI will take "
                         "its documented fallback path",
                         kOptionalDeviceExtensions[i]);
        }

        // An extension being present is not the same as its feature being
        // supported, and vkCreateDevice fails if a chained feature struct
        // asks for a VK_FALSE feature. Ask before enabling.
        vk::PhysicalDeviceFaultFeaturesEXT faultFeaturesExt;
        vk::PhysicalDeviceFaultFeaturesKHR faultFeaturesKhr;
        if (haveDeviceFaultExt)
        {
            vk::PhysicalDeviceFeatures2 supported;
            supported.pNext = &faultFeaturesExt;
            out.physicalDevice.getFeatures2(&supported);
            faultFeaturesExt.pNext = nullptr;  // queried as a leaf; re-chained below
            if (!faultFeaturesExt.deviceFault) haveDeviceFaultExt = false;
        }
        if (haveDeviceFaultKhr)
        {
            vk::PhysicalDeviceFeatures2 supported;
            supported.pNext = &faultFeaturesKhr;
            out.physicalDevice.getFeatures2(&supported);
            faultFeaturesKhr.pNext = nullptr;
            if (!faultFeaturesKhr.deviceFault) haveDeviceFaultKhr = false;
        }
        // Prefer EXT when both are advertised: it is the mature spelling
        // (spec version 2 vs 1) and the one F-5d binds against.
        if (haveDeviceFaultExt) haveDeviceFaultKhr = false;

        auto deviceFaultSpelling = VulkanCrashDesc::DeviceFault::None;
        if (haveDeviceFaultExt)
        {
            deviceExtensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            deviceFaultSpelling = VulkanCrashDesc::DeviceFault::Ext;
        }
        else if (haveDeviceFaultKhr)
        {
            deviceExtensions.push_back(VK_KHR_DEVICE_FAULT_EXTENSION_NAME);
            deviceFaultSpelling = VulkanCrashDesc::DeviceFault::Khr;
        }
        if (haveBufferMarker)
            deviceExtensions.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);

        ARC_INFO("Vulkan GPU-crash extensions: VK_AMD_buffer_marker={}, device_fault={}",
                 haveBufferMarker ? "yes" : "no",
                 haveDeviceFaultExt ? "EXT" : (haveDeviceFaultKhr ? "KHR" : "no"));

        // NRI capability contract item 7: exactly one GRAPHICS family with
        // queueCount = 1. NRI clamps the queueNum a wrapper desc declares
        // against PHYSICAL family capacity, not against what we created, so
        // this count and the desc's must stay equal -- declaring more would
        // hand back queues that do not exist.
        const float priority = 1.0f;
        auto queueInfo = vk::DeviceQueueCreateInfo()
            .setQueueFamilyIndex((uint32_t)out.graphicsQueueFamily)
            .setQueueCount(1)
            .setPQueuePriorities(&priority);

        // NRI capability contract item 4: query-then-pass-back-verbatim,
        // the pattern NVIDIA's own wrapper sample uses
        // (NRISamples/Source/Wrapper.cpp:340-360). In wrapper mode NRI
        // derives its ENTIRE capability model from
        // vkGetPhysicalDeviceFeatures2 on the PHYSICAL device -- it never
        // asks the logical device what was actually enabled, and has no way
        // to. So anything the GPU reports and we fail to enable becomes an
        // over-reported nri::DeviceDesc and, for the subset NRI uses
        // unconditionally, invalid Vulkan usage on the first call. Enabling
        // exactly the supported set is the only shape that cannot lie.
        //
        // This replaces the previous hand-picked pair of structs, which
        // enabled three bits and left the whole core-1.0 block VK_FALSE
        // while NRI would have reported the physically-supported ones as
        // available. It subsumes their reasoning, which still holds for
        // NVRHI and is worth keeping: NVRHI's queue submits with
        // vk::TimelineSemaphoreSubmitInfo (timelineSemaphore), calls
        // vkCmdPipelineBarrier2 (synchronization2) and builds pipelines
        // with VkPipelineRenderingCreateInfo instead of a VkRenderPass
        // (dynamicRendering, or vkCreateGraphicsPipelines fails
        // VUID-06576) -- all three are in the hard set asserted below.
        vk::PhysicalDeviceFeatures2        enabledFeatures2;
        vk::PhysicalDeviceVulkan11Features vulkan11Features;
        vk::PhysicalDeviceVulkan12Features vulkan12Features;
        vk::PhysicalDeviceVulkan13Features vulkan13Features;
        enabledFeatures2.setPNext(&vulkan11Features);
        vulkan11Features.setPNext(&vulkan12Features);
        vulkan12Features.setPNext(&vulkan13Features);
        vulkan13Features.setPNext(nullptr);

        // The chain grows past vulkan13Features below (item 3's feature
        // structs, then the device-fault struct), so the tail is tracked
        // rather than assumed -- appending to a stale tail would silently
        // drop everything already attached there.
        void** chainTail = &vulkan13Features.pNext;
        const auto chainAppend = [&chainTail](auto& feature) {
            feature.pNext = nullptr;
            *chainTail = &feature;
            chainTail = &feature.pNext;
        };

        // NRI capability contract item 3, second half. At VK 1.3 the
        // maintenance5/6 FEATURE bits live in their own KHR structs, which
        // none of the promoted-to-core structs above carries -- so taking
        // the extension in the sweep without chaining these would create a
        // device with the extension enabled and its feature VK_FALSE. That
        // is precisely the meta-rule's failure mode, and NRI walks straight
        // into it: it gates feature-struct chaining on OUR extension-name
        // list (SharedVK.h:28-31) but sets m_IsSupported.maintenance5/6
        // from PHYSICAL support (DeviceVK.hpp:657-658, :673-674), then
        // calls vkCmdBindIndexBuffer2 / vkCmdBindDescriptorSets2 /
        // vkCmdPushConstants2 (CommandBufferVK.hpp:872-876, :928, :966) and
        // sets VMA's KHR_MAINTENANCE5 bit (DeviceVK.hpp:225) -- all of
        // which need the feature enabled at creation. Chained BEFORE the
        // query so they are filled and passed back verbatim, exactly as
        // item 4 does for the core structs. (VK_EXT_memory_budget needs
        // nothing here: it has no feature struct at all.)
        vk::PhysicalDeviceMaintenance5FeaturesKHR maintenance5Features;
        vk::PhysicalDeviceMaintenance6FeaturesKHR maintenance6Features;
        if (haveOptional[kOptMaintenance5])
            chainAppend(maintenance5Features);
        if (haveOptional[kOptMaintenance6])
            chainAppend(maintenance6Features);

        out.physicalDevice.getFeatures2(&enabledFeatures2);

        // NRI capability contract item 14's rule applied to item 4's hard
        // set: keyed off the PHYSICAL query, so the message names the
        // capability the GPU does not offer rather than surfacing as a
        // VUID on the first submit/buffer/query-reset. Every one of these
        // is either used unconditionally by NRI or, worse, invisible to it
        // (timelineSemaphore and hostQueryReset appear nowhere in NRI's
        // source -- there is no capability bit that could degrade).
        const struct { bool supported; const char* name; } kHardFeatures[] = {
            { (bool)vulkan12Features.timelineSemaphore,   "timelineSemaphore"   },
            { (bool)vulkan12Features.bufferDeviceAddress, "bufferDeviceAddress" },
            { (bool)vulkan12Features.hostQueryReset,      "hostQueryReset"      },
            { (bool)vulkan13Features.synchronization2,    "synchronization2"    },
            { (bool)vulkan13Features.dynamicRendering,    "dynamicRendering"    },
            { (bool)vulkan13Features.maintenance4,        "maintenance4"        },
        };
        bool hardFeaturesPresent = true;
        for (const auto& feature : kHardFeatures)
        {
            if (feature.supported)
                continue;
            ARC_ERROR("Vulkan feature '{}' is not supported by '{}'; it is "
                      "hard-required (NRI wrapper capability contract item 4)",
                      feature.name, out.adapterName);
            hardFeaturesPresent = false;
        }
        if (!hardFeaturesPresent)
            return false;

        // F-5c step 4: VK_EXT/KHR_device_fault requires its feature to be
        // enabled at device creation, so the matching struct is appended
        // to the TAIL of the existing chain (deviceInfo -> features2 ->
        // vulkan11 -> vulkan12 -> vulkan13 -> [maintenance5] ->
        // [maintenance6] -> fault; contract item 4 keeps it at the tail,
        // and it is queried as a leaf above so it never rides the
        // enable-what-is-supported chain -- which is why it appends AFTER
        // the query rather than before it like the maintenance structs).
        // The two structs are distinct types, not aliases -- chain the one
        // whose spelling was enabled above.
        // deviceFaultVendorBinary is opt-in on top and is what makes the
        // opaque vendor blob retrievable; take it only where advertised.
        if (deviceFaultSpelling == VulkanCrashDesc::DeviceFault::Ext)
        {
            faultFeaturesExt.deviceFault = VK_TRUE;
            chainAppend(faultFeaturesExt);
        }
        else if (deviceFaultSpelling == VulkanCrashDesc::DeviceFault::Khr)
        {
            faultFeaturesKhr.deviceFault = VK_TRUE;
            chainAppend(faultFeaturesKhr);
        }

        // pEnabledFeatures stays null (Vulkan-Hpp's default): the core-1.0
        // block travels in enabledFeatures2.features, and the two are
        // mutually exclusive.
        auto deviceInfo = vk::DeviceCreateInfo()
            .setQueueCreateInfoCount(1)
            .setPQueueCreateInfos(&queueInfo)
            .setEnabledExtensionCount((uint32_t)deviceExtensions.size())
            .setPpEnabledExtensionNames(deviceExtensions.data())
            .setPNext(&enabledFeatures2);

        try
        {
            out.device = out.physicalDevice.createDevice(deviceInfo);
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
        VULKAN_HPP_DEFAULT_DISPATCHER.init(out.device);
        out.graphicsQueue = out.device.getQueue((uint32_t)out.graphicsQueueFamily, 0);

        // NRI capability contract item 6: the extension lists become
        // members the moment the device they describe exists, so every
        // consumer reads ONE list rather than a copy that can drift. NRI
        // takes the device list verbatim in wrapper mode -- it is the sole
        // gate on which device-level entry points it resolves and which
        // extension feature structs it chains -- so "what we actually
        // enabled" has to survive past this function.
        //
        // NRI capability contract item 8 (the value, not the wiring -- the
        // field lives in the wrapper desc): nri::VKBindingOffsets is
        // {sRegister, tRegister, bRegister, uRegister} and ours must equal
        // the dxc -fvk-*-shift values in ShaderConventions.hpp's
        // kSpirvArgs, i.e. {s=128, t=0, b=256, u=384} -- NOT NRI's
        // reference {0, 128, 32, 64}. Those shifts are also
        // nvrhi::VulkanBindingOffsets' defaults, which is why the SPIR-V
        // half of every shader works today; change them in
        // ShaderConventions.hpp first and fan out, as that header says.
        out.enabledInstanceExtensions = instanceExtensions;
        out.enabledDeviceExtensions   = deviceExtensions;
        // The GPU-crash record travels with the rest of "what this device was
        // created with", for the same reason: it is decided here and consumed
        // after the split (VulkanCrashDesc, below). Neither value changes
        // after the sweep above.
        out.deviceFault  = deviceFaultSpelling;
        out.bufferMarker = haveBufferMarker;

        return true;
    }

    // The tail of ~DeviceVulkan, verbatim and in the same order: device,
    // then debug messenger, then instance. Handles are nulled as they go so
    // an owner can call this at the point its ordering requires (contract
    // item 15: the NRI device is destroyed BEFORE this runs) and still hold
    // the struct afterwards.
    void DestroyVulkanNativeDevice(VulkanDeviceCreation& creation)
    {
        if (creation.device)
        {
            creation.device.destroy();
            creation.device = nullptr;
        }
        if (creation.debugMessenger)
        {
            creation.instance.destroyDebugUtilsMessengerEXT(creation.debugMessenger);
            creation.debugMessenger = nullptr;
        }
        if (creation.instance)
        {
            creation.instance.destroy();
            creation.instance = nullptr;
        }
    }

    bool DeviceVulkan::Init(const RenderDeviceDesc& desc)
    {
        // Everything above the NVRHI desc used to live inline here; it is the
        // creation half now, and this call is the ONLY thing between the two
        // versions of this function.
        if (!CreateVulkanNativeDevice(desc, m_creation))
            return false;

        nvrhi::vulkan::DeviceDesc nvrhiDesc;
        nvrhiDesc.errorCB = &NvrhiMessageCallback::Instance();
        nvrhiDesc.instance = m_creation.instance;
        nvrhiDesc.physicalDevice = m_creation.physicalDevice;
        nvrhiDesc.device = m_creation.device;
        nvrhiDesc.graphicsQueue = m_creation.graphicsQueue;
        nvrhiDesc.graphicsQueueIndex = m_creation.graphicsQueueFamily;
        nvrhiDesc.instanceExtensions = m_creation.enabledInstanceExtensions.data();
        nvrhiDesc.numInstanceExtensions = m_creation.enabledInstanceExtensions.size();
        // F-5c step 3: BOTH consumers of the extension list get the
        // filtered vector. Leaving this at the constant array would tell
        // NVRHI the diagnostics extensions are absent even though the
        // device was created with them.
        nvrhiDesc.deviceExtensions = m_creation.enabledDeviceExtensions.data();
        nvrhiDesc.numDeviceExtensions = m_creation.enabledDeviceExtensions.size();
        // Deliberately left at its default false even though contract item
        // 4 now enables features12.bufferDeviceAddress: this flag is
        // "does the APP want NVRHI to use device addresses", and flipping
        // it would change NVRHI's own buffer-usage flags. The NVRHI path
        // must stay byte-identical through this task; NRI reads the
        // feature off the physical device and needs nothing from here.
        nvrhiDesc.bufferDeviceAddressSupported = false;

        m_nvrhiBackend = nvrhi::vulkan::createDevice(nvrhiDesc);
        if (!m_nvrhiBackend)
        {
            ARC_ERROR("nvrhi::vulkan::createDevice failed");
            return false;
        }

        m_nvrhi = m_nvrhiBackend;
        if (desc.enableValidation)
            m_nvrhi = nvrhi::validation::createValidationLayer(m_nvrhi);

        // GPU crash backend (F-5/F-4). Built against the OUTER nvrhi
        // device for getNativeObject (the validation layer forwards it
        // verbatim), plus the UNWRAPPED one for queueGetCompletedInstance,
        // which is declared on nvrhi::vulkan::IDevice only.
        VulkanCrashDesc crashDesc;
        crashDesc.device        = m_nvrhi.Get();
        crashDesc.backendDevice = VulkanNvrhi();
        crashDesc.deviceFault   = m_creation.deviceFault;
        crashDesc.bufferMarker  = m_creation.bufferMarker;
        m_crashBackend = MakeVulkanCrashBackend(crashDesc);
        if (m_crashBackend)
        {
            g_deviceRemovedReported.store(false, std::memory_order_release);
            // A new device means the loss the latch described is over; a
            // stale latch would quit the host the moment this healthy
            // device started presenting.
            ResetGpuDeviceLost();
            // F-3b: the cross-backend observable, armed now that there is
            // something to collect with.
            NvrhiMessageCallback::Instance().SetDeviceRemovedHook(&ObserveDeviceRemoved);
            // The ONE SetGpuSectionProvider call per host lifetime. Only
            // one RenderDevice backend is live per process, so this and
            // DeviceD3D12's call are two TEXTUAL sites of a single runtime
            // install. The device layer owns the slot because it owns the
            // backend the slot's `user` pointer names; ~DeviceVulkan
            // clears it.
            Diagnostics::SetGpuSectionProvider(&VulkanGpuSectionProvider, m_crashBackend.get());
            // Task 7's pass scopes read the backend from here. Same owner,
            // same lifetime, same clear site as the provider slot above --
            // F-8e's three command-list owners sit in layers that cannot be
            // handed a backend pointer.
            SetActiveGpuCrashBackend(m_crashBackend.get());
            ARC_INFO("GPU crash backend armed: {}", m_crashBackend->Name());
        }

        ARC_INFO("Vulkan device created on '{}'", m_creation.adapterName);
        return true;
    }

    DeviceVulkan::~DeviceVulkan()
    {
        if (m_crashBackend)
        {
            // Symmetric with Init's install. All three slots are
            // process-wide and point INTO m_crashBackend, so they must be
            // emptied before it goes away. The pass-scope slot clears
            // conditionally: a second device that installed after this one
            // must keep its registration.
            Diagnostics::ClearGpuSectionProvider();
            // Wait out a report already mid-flight against this backend
            // (watchdog thread) before m_crashBackend.reset() below --
            // clearing the provider slot above only stops the NEXT
            // report from seeing it. See Diagnostics::FenceReports.
            Diagnostics::FenceReports();
            (void)ClearActiveGpuCrashBackendIfCurrent(m_crashBackend.get());
            NvrhiMessageCallback::Instance().SetDeviceRemovedHook(nullptr);
        }
        if (m_nvrhi)
        {
            m_nvrhi->waitForIdle();
            m_nvrhi->runGarbageCollection();
        }
        // BEFORE m_creation.device.destroy(): the backend owns a VkBuffer and a
        // VkDeviceMemory allocated from that device. Member-declaration
        // order cannot save us here -- this destructor destroys the device
        // in its own body, which runs first.
        m_crashBackend.reset();
        m_nvrhi = nullptr;
        m_nvrhiBackend = nullptr;
        // The same three destroys this function always ended with, in the
        // same order, now expressed once so the wrapper path's owner can run
        // them at the same point (DeviceCreationVulkan.hpp).
        DestroyVulkanNativeDevice(m_creation);
    }

    // The narrow export (DeviceFactories.hpp, NRI Phase 3 Task 5): the SAME
    // observer above, reachable by address from the Render module's other
    // installer. One line, no state, no second observation point -- the
    // once-only `g_deviceRemovedReported` latch, the "gpu-crash: device
    // removed" wording and the NoteGpuDeviceLost ordering all stay in
    // ObserveDeviceRemoved, unchanged and file-local.
    void ObserveDeviceRemovedVulkan()
    {
        ObserveDeviceRemoved();
    }

    // Its twin (DeviceFactories.hpp): the SAME store DeviceVulkan::Init makes
    // one line above its own ResetGpuDeviceLost(), for the other arming site.
    // The latch stays file-local and keeps its single meaning -- this only
    // lets NriDiagnostics::Arm re-arm it, exactly as Init does.
    void ResetDeviceRemovedLatchVulkan()
    {
        g_deviceRemovedReported.store(false, std::memory_order_release);
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
