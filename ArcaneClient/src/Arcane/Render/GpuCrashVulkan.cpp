// GPU crash diagnostics arc (Task 6): the Vulkan crash backend.
//
// The D3D12 sibling (GpuCrashD3D12.cpp) can count on its two layers being
// there -- WriteBufferImmediate is core D3D12 and DRED ships in the OS
// runtime. Vulkan's equivalents are OPTIONAL EXTENSIONS, so every layer here
// is request-if-available and each one degrades on its own:
//
//   1. MARKERS. `VK_AMD_buffer_marker` writes a per-scope begin/end value
//      into a host-visible + host-coherent, persistently-mapped VkBuffer
//      (F-5d) with vkCmdWriteBufferMarkerAMD -- the Vulkan analogue of F-1's
//      system-memory marker buffer, TOP_OF_PIPE for begin / BOTTOM_OF_PIPE
//      for end mirroring F-1b's MARKER_IN / MARKER_OUT. When the extension is
//      absent the layer degrades to CPU-side FENCE CORRELATION: the ring
//      already carries the scope names, so OnMarkerWritten is driven from the
//      graphics queue's completed-instance counter instead of GPU writes.
//      That is submission granularity rather than pass granularity, and it
//      says so in activeLayers as `breadcrumbs:fence`.
//   2. DEVICE FAULT. `VK_EXT_device_fault` (or its `VK_KHR_device_fault`
//      promotion) is the rough analogue of DRED's page-fault output: fault
//      address, instruction address, vendor codes, and an opaque vendor
//      binary. Absent => `devicefault:off` and the report still ships
//      everything else.
//   3. The `.gpudump` container (Diag::GpuDumpWriter, IGpuCrashBackend.hpp) --
//      the SHARED writer, not a fork. Raw marker bytes (or the fence table),
//      the flattened fault report, and the vendor blob, written for every gpu
//      kind even on partial collection, because the section table doubles as
//      the capture inventory.
//
// NVRHI boundary rules: no ICommandList is ever wrapped and no barrier is
// ever issued here. The only native access is getNativeObject at the seams
// F-4 names (VK_Device, VK_PhysicalDevice, VK_CommandBuffer) plus the one
// backend-interface call queueGetCompletedInstance. vkCmdWriteBufferMarkerAMD
// and vkGetDeviceFaultInfoEXT appear in this file and nowhere else in the
// engine.
//
// Entry-point resolution follows the file it is armed from: DeviceVulkan uses
// the Vulkan-Hpp default dynamic dispatcher and re-inits it against the
// VkDevice AFTER device creation (DeviceVulkan.cpp's
// VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device)), which is exactly a
// vkGetDeviceProcAddr sweep of every extension entry point -- including these
// two -- and it resolves them to null when the extension was not enabled.
// A null function pointer is therefore the authoritative "not available"
// answer and is checked at every call site.

#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/GpuCrashReport.hpp>

#include <nvrhi/vulkan.h>

#include <vulkan/vulkan.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    namespace
    {
        // ------------------------------------------------------------------
        // Marker geometry -- SHARED with the D3D12 backend by construction
        // ------------------------------------------------------------------
        // Diag::kGpuMarker* (GpuCrashReport.hpp) is the single definition;
        // Diag::ReplayMarkerBuffer reads this backend's region back with those
        // same constants. Local aliases only, to keep the code below readable.
        constexpr std::uint32_t kMarkerSlots     = Diag::kGpuMarkerSlots;
        constexpr std::uint32_t kValuesPerSlot   = Diag::kGpuMarkerValuesPerSlot;
        constexpr std::size_t   kMarkerBytes     = Diag::kGpuMarkerBytes;
        constexpr std::uint32_t kMarkerUnwritten = Diag::kGpuMarkerUnwritten;

        // Hex formatting is shared with the D3D12 backend. Only HexU64 is used
        // here -- Vulkan reports 64-bit device addresses and vendor codes, and
        // has no 32-bit HRESULT analogue to format.
        using Diag::HexU64;

        // Vulkan hands back fixed-size char arrays that the spec says are
        // NUL-terminated. A crash-time reader trusts nothing: bounded, and
        // non-printable bytes become '?' so a corrupt driver string cannot
        // smuggle control characters into the report text.
        [[nodiscard]] std::string AsciiField(const char* text, std::size_t capacity)
        {
            std::string out;
            if (!text) return out;
            for (std::size_t i = 0; i < capacity && text[i] != '\0'; ++i)
            {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
            }
            return out;
        }

        // VkDeviceFaultAddressTypeEXT is a typedef OF the KHR enum
        // (vulkan_core.h:20913), so one table serves both spellings.
        [[nodiscard]] const char* FaultAddressTypeName(VkDeviceFaultAddressTypeKHR type)
        {
            switch (type)
            {
            case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR:                        return "none";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_KHR:                return "read-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_KHR:               return "write-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_KHR:             return "execute-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_KHR: return "ip-unknown";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_KHR: return "ip-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_KHR:   return "ip-fault";
            default:                                                           return "unrecognized";
            }
        }

        // Envelope Fault::type vocabulary. Deliberately the SAME words the
        // D3D12 backend produces ("page-fault", "device-hung", ...) so a
        // renderer reading an .arcdiag does not need to know which backend
        // wrote it. Null when this address type says nothing about the kind.
        [[nodiscard]] const char* FaultAddressKind(VkDeviceFaultAddressTypeKHR type)
        {
            switch (type)
            {
            case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_KHR:
            case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_KHR:
            case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_KHR:
                return "page-fault";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_KHR:
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_KHR:
                return "instruction-fault";
            default:
                return nullptr;
            }
        }

        // KHR-only: the EXT report struct carries no flags field.
        [[nodiscard]] std::string DescribeFaultFlags(VkDeviceFaultFlagsKHR flags)
        {
            std::string out;
            auto add = [&out](const char* name) {
                if (!out.empty()) out += "|";
                out += name;
            };
            if (flags & VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR)         add("device-lost");
            if (flags & VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR)      add("memory-address");
            if (flags & VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR) add("instruction-address");
            if (flags & VK_DEVICE_FAULT_FLAG_VENDOR_KHR)              add("vendor");
            if (flags & VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR)    add("watchdog-timeout");
            if (flags & VK_DEVICE_FAULT_FLAG_OVERFLOW_KHR)            add("overflow");
            if (out.empty()) out = "none";
            return out;
        }

        [[nodiscard]] const char* VkResultName(VkResult result)
        {
            // nvrhi already owns a full VkResult table; reuse it rather than
            // maintaining a second one (F-3d cites the same file for the
            // DEVICE_LOST spelling).
            return nvrhi::vulkan::resultToString(result);
        }

        // ------------------------------------------------------------------
        // The backend
        // ------------------------------------------------------------------

        class VulkanCrashBackend final : public IGpuCrashBackend
        {
        public:
            explicit VulkanCrashBackend(const VulkanCrashDesc& desc);
            ~VulkanCrashBackend() override;

            bool WriteMarker(nvrhi::ICommandList* commandList, std::uint32_t id, bool begin) override;
            bool WriteMarkerNative(void* nativeCommandList, std::uint32_t id, bool begin) override;
            void CollectFault(Diag::Envelope& envelope) override;
            GpuBreadcrumbs& Breadcrumbs() override { return m_breadcrumbs; }
            const char* Name() const override { return "Vulkan"; }
            // The VkDevice m_markerBuffer was created on. THE check that keeps
            // vkCmdWriteBufferMarkerAMD's commonparent VUID satisfied when more
            // than one VkDevice is alive (Phase 2's --nri-graph vehicle): a
            // command buffer from any other device may not name this buffer.
            [[nodiscard]] void* NativeDevice() const override
            {
                return static_cast<VkDevice>(m_device);
            }

            // The Diagnostics GPU-section body: CollectFault plus the human
            // block plus the `.gpudump` sibling. Not on the interface -- the
            // seam's CollectFault deliberately takes only an Envelope.
            void FillReport(Diag::Envelope& envelope,
                            std::string& humanText,
                            const std::filesystem::path& reportStem);

        private:
            bool ArmMarkerBuffer();
            void ReleaseMarkerBuffer();

            // The graphics queue's last COMPLETED submission instance, or 0
            // when there is no backend device / the device is already lost
            // (which latches m_deviceLost).
            std::uint64_t QueueCompletedInstance();

            void CollectMarkers(Diag::Envelope& envelope, std::uint64_t completedInstance);

            // Both return whether the query ACTUALLY ANSWERED -- i.e. the
            // entry point resolved and the driver returned SUCCESS/INCOMPLETE.
            // False means the layer engaged nothing, however healthily it was
            // enabled; `out` still records why. An answer with zero faults in
            // it is still an answer, so that returns true (same rule as the
            // D3D12 backend's dred-data tier, which reports which interface
            // answered rather than whether it had content).
            [[nodiscard]] bool CollectDeviceFaultExt(Diag::Envelope& envelope, std::string& out);
            [[nodiscard]] bool CollectDeviceFaultKhr(Diag::Envelope& envelope, std::string& out);

            [[nodiscard]] const char* DeviceFaultLayer() const
            {
                switch (m_deviceFault)
                {
                case VulkanCrashDesc::DeviceFault::Ext: return "devicefault:ext";
                case VulkanCrashDesc::DeviceFault::Khr: return "devicefault:khr";
                default:                                return "devicefault:off";
                }
            }

            nvrhi::IDevice*         m_nvrhi   = nullptr;
            nvrhi::vulkan::IDevice* m_backend = nullptr;   // non-owning: DeviceVulkan owns it
            vk::Device              m_device;              // non-owning: nvrhi/DeviceVulkan own it
            vk::PhysicalDevice      m_physicalDevice;

            VulkanCrashDesc::DeviceFault m_deviceFault = VulkanCrashDesc::DeviceFault::None;

            // --- layer 1a: the GPU-written marker buffer -------------------
            vk::Buffer        m_markerBuffer;
            vk::DeviceMemory  m_markerHeap;
            void*             m_markerMapped = nullptr;
            std::atomic<bool> m_markersArmed{ false };
            std::atomic<bool> m_markerFailureLogged{ false };

            // --- layer 1b: the fence-correlated degrade --------------------
            // Same slot geometry as the marker buffer, but each slot records
            // the queue's completed-instance counter AS OBSERVED WHEN THE
            // MARKER WAS RECORDED. Atomics rather than a mutex: CollectFault
            // runs from the crash/hang path, and a lock held by the wedged
            // thread this exists to describe would deadlock the report.
            struct FenceSlot
            {
                std::atomic<std::uint32_t> id{ kMarkerUnwritten };  // scope id + 1
                std::atomic<std::uint64_t> stamp{ 0 };
            };
            std::unique_ptr<FenceSlot[]> m_fenceSlots;
            std::atomic<bool>            m_fenceArmed{ false };

            std::atomic<bool>   m_deviceLost{ false };
            GpuBreadcrumbs      m_breadcrumbs;
            Diag::GpuDumpWriter m_raw;
            std::string         m_humanText;
        };

        VulkanCrashBackend::VulkanCrashBackend(const VulkanCrashDesc& desc)
            : m_nvrhi(desc.device)
            , m_backend(desc.backendDevice)
            , m_deviceFault(desc.deviceFault)
        {
            // F-4a/F-4b: the native handles through NVRHI's accessor.
            // DeviceVulkan's own Device()/PhysicalDevice() are TU-local
            // (F-4c), and the validation layer forwards getNativeObject, so
            // this reaches the real handles either way.
            if (m_nvrhi)
            {
                m_device = vk::Device(static_cast<VkDevice>(
                    m_nvrhi->getNativeObject(nvrhi::ObjectTypes::VK_Device).pointer));
                m_physicalDevice = vk::PhysicalDevice(static_cast<VkPhysicalDevice>(
                    m_nvrhi->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer));
            }

            if (!m_device)
            {
                ARC_WARN("GPU crash backend: no native VkDevice from the nvrhi device; "
                         "markers and device-fault retrieval are both disabled");
                return;
            }

            if (desc.bufferMarker && ArmMarkerBuffer())
            {
                m_markersArmed.store(true, std::memory_order_release);
                return;
            }

            // Whatever went wrong above, the marker buffer is not usable --
            // release the partial state before deciding on the degrade rung.
            ReleaseMarkerBuffer();

            // F-5 degrade: the ring already carries the scope names, so the
            // only thing the missing extension costs is per-scope GPU
            // evidence. Fence progress substitutes at SUBMISSION granularity.
            if (m_backend)
            {
                m_fenceSlots = std::make_unique<FenceSlot[]>(kMarkerSlots);
                m_fenceArmed.store(true, std::memory_order_release);
                ARC_INFO("GPU markers degraded to fence granularity: {} "
                         "(scope completion derived from the graphics queue's "
                         "completed-instance counter, not from GPU writes)",
                         desc.bufferMarker ? "VK_AMD_buffer_marker armed but the buffer failed"
                                           : "VK_AMD_buffer_marker not available");
            }
            else
            {
                ARC_WARN("GPU markers disabled: neither VK_AMD_buffer_marker nor an "
                         "unwrapped nvrhi::vulkan::IDevice for fence correlation");
            }
        }

        VulkanCrashBackend::~VulkanCrashBackend()
        {
            ReleaseMarkerBuffer();
        }

        bool VulkanCrashBackend::ArmMarkerBuffer()
        {
            if (!m_physicalDevice)
            {
                ARC_WARN("GPU markers disabled: no native VkPhysicalDevice "
                         "(cannot pick a host-visible memory type)");
                return false;
            }

            try
            {
                // F-5d: "VK_AMD_buffer_marker prefers HOST_VISIBLE |
                // HOST_COHERENT host memory for the marker buffer -- the
                // Vulkan analogue of F-1a's system-memory requirement. There
                // is no OpenExistingHeapFromAddress equivalent; a
                // host-coherent, host-visible, persistently-mapped VkBuffer is
                // the recipe." Coherent specifically so the read-back below
                // needs no vkInvalidateMappedMemoryRanges -- an extra API call
                // against an already-lost device is a call that can fail.
                m_markerBuffer = m_device.createBuffer(
                    vk::BufferCreateInfo()
                        .setSize(kMarkerBytes)
                        .setUsage(vk::BufferUsageFlagBits::eTransferDst)
                        .setSharingMode(vk::SharingMode::eExclusive));

                const vk::MemoryRequirements requirements =
                    m_device.getBufferMemoryRequirements(m_markerBuffer);
                const vk::PhysicalDeviceMemoryProperties memoryProperties =
                    m_physicalDevice.getMemoryProperties();

                constexpr vk::MemoryPropertyFlags wanted =
                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

                std::uint32_t typeIndex = UINT32_MAX;
                for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
                {
                    if ((requirements.memoryTypeBits & (1u << i)) == 0) continue;
                    if ((memoryProperties.memoryTypes[i].propertyFlags & wanted) != wanted) continue;
                    typeIndex = i;
                    break;
                }
                if (typeIndex == UINT32_MAX)
                {
                    ARC_WARN("GPU markers disabled: no HOST_VISIBLE|HOST_COHERENT memory type "
                             "accepts the marker buffer (F-5d)");
                    return false;
                }

                m_markerHeap = m_device.allocateMemory(
                    vk::MemoryAllocateInfo()
                        .setAllocationSize(requirements.size)
                        .setMemoryTypeIndex(typeIndex));
                m_device.bindBufferMemory(m_markerBuffer, m_markerHeap, 0);

                // Persistently mapped: at crash time the device may be wedged,
                // and vkMapMemory then is one more call that can fail. The
                // mapping is process memory the driver hands back once.
                m_markerMapped = m_device.mapMemory(m_markerHeap, 0, VK_WHOLE_SIZE);
                if (!m_markerMapped)
                {
                    ARC_WARN("GPU markers disabled: vkMapMemory returned null for the marker buffer");
                    return false;
                }

                std::memset(m_markerMapped, 0, kMarkerBytes);  // 0 == kMarkerUnwritten
                ARC_INFO("GPU markers armed: {} slots in a {}-byte host-coherent buffer "
                         "(VK_AMD_buffer_marker)", kMarkerSlots, kMarkerBytes);
                return true;
            }
            catch (const vk::SystemError& e)
            {
                ARC_WARN("GPU markers disabled: marker-buffer creation threw ({})", e.what());
                return false;
            }
        }

        void VulkanCrashBackend::ReleaseMarkerBuffer()
        {
            m_markersArmed.store(false, std::memory_order_release);
            if (m_device)
            {
                if (m_markerHeap)
                {
                    if (m_markerMapped) m_device.unmapMemory(m_markerHeap);
                    m_device.freeMemory(m_markerHeap);
                }
                if (m_markerBuffer) m_device.destroyBuffer(m_markerBuffer);
            }
            m_markerMapped = nullptr;
            m_markerHeap   = vk::DeviceMemory{};
            m_markerBuffer = vk::Buffer{};
        }

        std::uint64_t VulkanCrashBackend::QueueCompletedInstance()
        {
            if (!m_backend) return 0;
            try
            {
                return m_backend->queueGetCompletedInstance(nvrhi::CommandQueue::Graphics);
            }
            catch (const vk::SystemError&)
            {
                // vkGetSemaphoreCounterValue on a lost device throws
                // vk::DeviceLostError. That is itself the answer -- latch it
                // and hand back 0, which reads as "no submission has retired",
                // i.e. every recorded scope stays in flight. Exactly right.
                m_deviceLost.store(true, std::memory_order_release);
                return 0;
            }
        }

        bool VulkanCrashBackend::WriteMarker(nvrhi::ICommandList* commandList, std::uint32_t id, bool begin)
        {
            if (!commandList) return false;

            // F-4b: NVRHI hands out the raw VkCommandBuffer; nothing here
            // wraps or owns the command list. Resolving it is the only thing
            // this overload does that the native one cannot -- everything
            // past here is shared, so the NVRHI passes and the NRI frame
            // graph write into one marker buffer through one code path.
            return WriteMarkerNative(
                commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer, id, begin);
        }

        bool VulkanCrashBackend::WriteMarkerNative(void* nativeCommandList, std::uint32_t id, bool begin)
        {
            if (m_markersArmed.load(std::memory_order_acquire))
            {
                const VkCommandBuffer commandBuffer = static_cast<VkCommandBuffer>(nativeCommandList);
                const PFN_vkCmdWriteBufferMarkerAMD write =
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteBufferMarkerAMD;

                if (!commandBuffer || !write)
                {
                    // One WARN, then the layer is off for good: a per-draw log
                    // from a failing marker path would bury the crash it
                    // exists to find.
                    if (!m_markerFailureLogged.exchange(true, std::memory_order_acq_rel))
                    {
                        ARC_WARN("GPU markers disabled: {} -- no vkCmdWriteBufferMarkerAMD path",
                                 commandBuffer ? "the entry point did not resolve"
                                               : "no native VkCommandBuffer from the command list");
                    }
                    m_markersArmed.store(false, std::memory_order_release);
                    return false;
                }

                const std::uint32_t slot = id % kMarkerSlots;
                const std::uint32_t word = slot * kValuesPerSlot + (begin ? 0u : 1u);

                // The F-1b binding rule, in Vulkan spelling. TOP_OF_PIPE is
                // the earliest point of the marker command, so a begin marker
                // lands once the GPU has STARTED the scope: "begin written,
                // end missing" means it entered and did not leave.
                // BOTTOM_OF_PIPE is deferred until preceding work has drained
                // through the pipeline, so an end marker's presence is proof
                // the scope finished. There is no third choice that says
                // anything about progress.
                write(commandBuffer,
                      begin ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      static_cast<VkBuffer>(m_markerBuffer),
                      VkDeviceSize{ word } * sizeof(std::uint32_t),
                      id + 1);  // 0 stays reserved for "never reached"
                return true;
            }

            if (m_fenceArmed.load(std::memory_order_acquire))
            {
                // The degrade path records CPU-side only: which scope owns the
                // slot, and where the queue's retirement counter stood when
                // the marker was recorded. CollectFault turns that into
                // completion evidence -- see the derivation there. The end
                // marker deliberately RE-stamps the slot with the (>=) current
                // counter, which makes the "retired" test strictly more
                // conservative than stamping on begin alone.
                FenceSlot& slot = m_fenceSlots[id % kMarkerSlots];
                slot.id.store(id + 1, std::memory_order_relaxed);
                slot.stamp.store(QueueCompletedInstance(), std::memory_order_release);
                return true;
            }

            return false;
        }

        void VulkanCrashBackend::CollectMarkers(Diag::Envelope& envelope, std::uint64_t completedInstance)
        {
            if (m_markerMapped)
            {
                // Host-coherent, so no vkInvalidateMappedMemoryRanges is owed
                // before the replay. The replay itself, the "markers" section,
                // and the breadcrumbs:pass/disarmed keys are backend-agnostic
                // -- Diag::ReplayMarkerBuffer. Guarded rather than called
                // unconditionally (as D3D12 does) because a null mapping here
                // means "try the fence path", not `breadcrumbs:off`.
                Diag::ReplayMarkerBuffer(m_breadcrumbs, m_raw, envelope, m_markerMapped,
                                         m_markersArmed.load(std::memory_order_acquire));
                return;
            }

            if (m_fenceArmed.load(std::memory_order_acquire))
            {
                // The derivation, stated plainly because its granularity is
                // the whole point of the `breadcrumbs:fence` label:
                //
                // A scope stamped when the completed counter read S was
                // recorded into a command list that had NOT been submitted
                // yet, so the submission carrying it has instance M > S.
                // Therefore `completedInstance == S` PROVES it has not
                // retired, and `completedInstance > S` means the submission
                // that could have carried it has. Arcane records and submits
                // inside one function at every seam F-8 lists, so that second
                // implication is exact here -- but it is a SUBMISSION-level
                // statement either way, which is why every live scope reports
                // its begin marker unconditionally: at this granularity the
                // honest claim is "this submission was in flight", not "the
                // GPU reached this pass".
                std::string table = "queueCompletedInstance=" + std::to_string(completedInstance) + "\n";
                table += "granularity=submission (VK_AMD_buffer_marker absent)\n";

                for (std::uint32_t slot = 0; slot < kMarkerSlots; ++slot)
                {
                    const std::uint32_t stored = m_fenceSlots[slot].id.load(std::memory_order_relaxed);
                    if (stored == kMarkerUnwritten) continue;

                    const std::uint32_t id    = stored - 1;
                    const std::uint64_t stamp = m_fenceSlots[slot].stamp.load(std::memory_order_acquire);
                    const bool          retired = completedInstance > stamp;

                    m_breadcrumbs.OnMarkerWritten(id, true);
                    if (retired) m_breadcrumbs.OnMarkerWritten(id, false);

                    table += "slot " + std::to_string(slot) + " id=" + std::to_string(id) +
                             " stamp=" + std::to_string(stamp) +
                             (retired ? "  retired\n" : "  in-flight\n");
                }

                m_raw.Add("markers.fence", table);
                envelope.activeLayers.emplace_back("breadcrumbs:fence");
                return;
            }

            envelope.activeLayers.emplace_back("breadcrumbs:off");
        }

        bool VulkanCrashBackend::CollectDeviceFaultExt(Diag::Envelope& envelope, std::string& out)
        {
            const PFN_vkGetDeviceFaultInfoEXT query = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT;
            if (!query)
            {
                out += "vkGetDeviceFaultInfoEXT=unresolved\n";
                m_humanText += "device fault : entry point unresolved\n";
                return false;
            }

            // F-5d: "vkGetDeviceFaultInfoEXT is the two-call idiom: call once
            // with pFaultInfo == nullptr to fill counts, allocate, call again."
            VkDeviceFaultCountsEXT counts{};
            counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;

            VkResult result = query(static_cast<VkDevice>(m_device), &counts, nullptr);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                out += std::string("vkGetDeviceFaultInfoEXT(counts)=") + VkResultName(result) + "\n";
                m_humanText += std::string("device fault : counts query failed (") +
                               VkResultName(result) + ")\n";
                return false;
            }

            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
            std::vector<std::uint8_t>                vendorBinary(static_cast<std::size_t>(counts.vendorBinarySize));

            VkDeviceFaultInfoEXT info{};
            info.sType             = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
            info.pAddressInfos     = addresses.empty() ? nullptr : addresses.data();
            info.pVendorInfos      = vendors.empty() ? nullptr : vendors.data();
            info.pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data();

            result = query(static_cast<VkDevice>(m_device), &counts, &info);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                out += std::string("vkGetDeviceFaultInfoEXT(info)=") + VkResultName(result) + "\n";
                m_humanText += std::string("device fault : info query failed (") +
                               VkResultName(result) + ")\n";
                return false;
            }

            out += "description=\"" + AsciiField(info.description, VK_MAX_DESCRIPTION_SIZE) + "\"\n";
            out += "addressInfoCount=" + std::to_string(counts.addressInfoCount) + "\n";
            out += "vendorInfoCount=" + std::to_string(counts.vendorInfoCount) + "\n";
            out += "vendorBinarySize=" + std::to_string(vendorBinary.size()) + "\n";

            for (std::uint32_t i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i)
            {
                const VkDeviceFaultAddressInfoEXT& address = addresses[i];
                out += "address " + std::to_string(i) +
                       " type=" + FaultAddressTypeName(address.addressType) +
                       " reported=" + HexU64(address.reportedAddress) +
                       " precision=" + HexU64(address.addressPrecision) + "\n";

                // First address that classifies the fault wins; later ones are
                // still recorded in full above.
                if (envelope.fault.address.empty())
                {
                    if (const char* kind = FaultAddressKind(address.addressType))
                    {
                        envelope.fault.type    = kind;
                        envelope.fault.address = HexU64(address.reportedAddress);
                    }
                }
            }

            for (std::uint32_t i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i)
            {
                const VkDeviceFaultVendorInfoEXT& vendor = vendors[i];
                out += "vendor " + std::to_string(i) +
                       " code=" + HexU64(vendor.vendorFaultCode) +
                       " data=" + HexU64(vendor.vendorFaultData) +
                       " desc=\"" + AsciiField(vendor.description, VK_MAX_DESCRIPTION_SIZE) + "\"\n";
            }

            // The opaque blob is the one section a HUMAN cannot read and a
            // vendor tool can, so it ships verbatim rather than summarized.
            if (!vendorBinary.empty())
                m_raw.Add("vk.fault.vendor", vendorBinary.data(), vendorBinary.size());

            m_humanText += "device fault : \"" + AsciiField(info.description, VK_MAX_DESCRIPTION_SIZE) +
                           "\" (" + std::to_string(counts.addressInfoCount) + " address, " +
                           std::to_string(counts.vendorInfoCount) + " vendor, " +
                           std::to_string(vendorBinary.size()) + "-byte blob)\n";
            return true;
        }

        bool VulkanCrashBackend::CollectDeviceFaultKhr(Diag::Envelope& envelope, std::string& out)
        {
            // The KHR promotion is NOT an alias of the EXT surface: the query
            // is vkGetDeviceFaultReportsKHR (a timeout + an ARRAY of
            // self-contained VkDeviceFaultInfoKHR reports, each carrying flags
            // and one inline address pair), and the vendor blob moved to its
            // own vkGetDeviceFaultDebugInfoKHR call.
            const PFN_vkGetDeviceFaultReportsKHR query =
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultReportsKHR;
            if (!query)
            {
                out += "vkGetDeviceFaultReportsKHR=unresolved\n";
                m_humanText += "device fault : entry point unresolved\n";
                return false;
            }

            std::uint32_t reportCount = 0;
            VkResult result = query(static_cast<VkDevice>(m_device), 0, &reportCount, nullptr);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                out += std::string("vkGetDeviceFaultReportsKHR(count)=") + VkResultName(result) + "\n";
                m_humanText += std::string("device fault : count query failed (") +
                               VkResultName(result) + ")\n";
                return false;
            }

            std::vector<VkDeviceFaultInfoKHR> reports(reportCount);
            for (VkDeviceFaultInfoKHR& report : reports)
                report.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR;

            if (reportCount > 0)
            {
                result = query(static_cast<VkDevice>(m_device), 0, &reportCount, reports.data());
                if (result != VK_SUCCESS && result != VK_INCOMPLETE)
                {
                    out += std::string("vkGetDeviceFaultReportsKHR(reports)=") + VkResultName(result) + "\n";
                    m_humanText += std::string("device fault : report query failed (") +
                                   VkResultName(result) + ")\n";
                    return false;
                }
            }

            out += "reportCount=" + std::to_string(reportCount) + "\n";
            for (std::uint32_t i = 0; i < reportCount && i < reports.size(); ++i)
            {
                const VkDeviceFaultInfoKHR& report = reports[i];
                out += "report " + std::to_string(i) +
                       " flags=" + DescribeFaultFlags(report.flags) +
                       " group=" + std::to_string(report.groupId) +
                       " desc=\"" + AsciiField(report.description, VK_MAX_DESCRIPTION_SIZE) + "\"\n";
                out += "  fault type=" + std::string(FaultAddressTypeName(report.faultAddressInfo.addressType)) +
                       " reported=" + HexU64(report.faultAddressInfo.reportedAddress) +
                       " precision=" + HexU64(report.faultAddressInfo.addressPrecision) + "\n";
                out += "  instr type=" + std::string(FaultAddressTypeName(report.instructionAddressInfo.addressType)) +
                       " reported=" + HexU64(report.instructionAddressInfo.reportedAddress) + "\n";
                out += "  vendor code=" + HexU64(report.vendorInfo.vendorFaultCode) +
                       " data=" + HexU64(report.vendorInfo.vendorFaultData) +
                       " desc=\"" + AsciiField(report.vendorInfo.description, VK_MAX_DESCRIPTION_SIZE) + "\"\n";

                if (envelope.fault.address.empty())
                {
                    if (const char* kind = FaultAddressKind(report.faultAddressInfo.addressType))
                    {
                        envelope.fault.type    = kind;
                        envelope.fault.address = HexU64(report.faultAddressInfo.reportedAddress);
                    }
                    else if (report.flags & VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR)
                    {
                        envelope.fault.type = "device-hung";
                    }
                }
            }

            // Vendor blob, same two-call idiom, separate entry point.
            if (const PFN_vkGetDeviceFaultDebugInfoKHR debugQuery =
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultDebugInfoKHR)
            {
                VkDeviceFaultDebugInfoKHR debug{};
                debug.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_DEBUG_INFO_KHR;
                if (debugQuery(static_cast<VkDevice>(m_device), &debug) == VK_SUCCESS &&
                    debug.vendorBinarySize > 0)
                {
                    std::vector<std::uint8_t> blob(static_cast<std::size_t>(debug.vendorBinarySize));
                    debug.pVendorBinaryData = blob.data();
                    if (debugQuery(static_cast<VkDevice>(m_device), &debug) == VK_SUCCESS)
                        m_raw.Add("vk.fault.vendor", blob.data(), blob.size());
                }
                out += "vendorBinarySize=" + std::to_string(debug.vendorBinarySize) + "\n";
            }

            m_humanText += "device fault : " + std::to_string(reportCount) + " KHR report(s)\n";
            return true;
        }

        void VulkanCrashBackend::CollectFault(Diag::Envelope& envelope)
        {
            m_raw = Diag::GpuDumpWriter{};
            m_humanText.clear();

            m_humanText += "backend      : Vulkan\n";

            // One probe, reused: it is both the fence-correlation input and
            // the liveness test (it throws vk::DeviceLostError on a lost
            // device, which QueueCompletedInstance latches).
            const std::uint64_t completedInstance = QueueCompletedInstance();

            CollectMarkers(envelope, completedInstance);

            Diag::EmitQueueSnapshot(m_breadcrumbs, "graphics", envelope, m_humanText);

            // TWO keys, mirroring the D3D12 backend's dred: / dred-data: pair.
            // This one is ENABLEMENT -- which spelling (if any) the device was
            // actually created with. It says nothing about whether the query
            // answered, which is why the engagement key below exists and why
            // it is emitted on EVERY exit path from here on.
            envelope.activeLayers.emplace_back(DeviceFaultLayer());

            if (!m_device)
            {
                // Enabled-but-unreachable is a real outcome: the extension may
                // have been enabled at device creation and yet there is no
                // native VkDevice to query through. Without this key the
                // report would claim `devicefault:ext` and nothing would
                // contradict it.
                envelope.activeLayers.emplace_back("devicefault-data:none");
                m_humanText += "device       : <no native VkDevice>\n";
                m_raw.Add("vk.device", std::string_view{ "no native VkDevice\n" });
                return;
            }

            // Vulkan has no GetDeviceRemovedReason: the closest equivalent is
            // whether a device-level call still answers. The probe above IS
            // that call, so this costs nothing extra.
            const bool lost = m_deviceLost.load(std::memory_order_acquire);
            envelope.fault.type = lost ? "device-removed" : "device-alive";
            // Vulkan's device_fault reports an ADDRESS, never an object name --
            // there is no analogue of DRED's allocation nodes -- so
            // envelope.fault.resource stays empty on this backend by design.

            std::string adapter;
            if (m_physicalDevice)
            {
                // Bound to a NAMED local: `getProperties()` returns by value,
                // so reading .deviceName off the temporary inline would dangle.
                const vk::PhysicalDeviceProperties properties = m_physicalDevice.getProperties();
                adapter = AsciiField(properties.deviceName.data(), VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
            }

            std::string deviceSection;
            deviceSection += "adapter=" + adapter + "\n";
            deviceSection += std::string("deviceLost=") + (lost ? "1" : "0") + "\n";
            deviceSection += "queueCompletedInstance=" + std::to_string(completedInstance) + "\n";
            deviceSection += std::string("deviceFault=") + DeviceFaultLayer() + "\n";
            deviceSection += "markerSlots=" + std::to_string(kMarkerSlots) + "\n";
            deviceSection += std::string("markersArmed=") +
                             (m_markersArmed.load(std::memory_order_acquire) ? "1" : "0") + "\n";
            deviceSection += std::string("fenceFallback=") +
                             (m_fenceArmed.load(std::memory_order_acquire) ? "1" : "0") + "\n";

            m_humanText += std::string("device state : ") + (lost ? "lost" : "responding") +
                           " (last completed submission " + std::to_string(completedInstance) + ")\n";

            // The ENGAGEMENT key: what actually answered, as opposed to what
            // was enabled. An enabled extension whose entry point did not
            // resolve, or whose query returned an error, engages nothing --
            // and a report that only carried the enablement key would still
            // claim `devicefault:ext` while vk.fault said "unresolved".
            std::string faultSection;
            const char* faultData = "devicefault-data:none";
            switch (m_deviceFault)
            {
            case VulkanCrashDesc::DeviceFault::Ext:
                if (CollectDeviceFaultExt(envelope, faultSection)) faultData = "devicefault-data:ext";
                break;
            case VulkanCrashDesc::DeviceFault::Khr:
                if (CollectDeviceFaultKhr(envelope, faultSection)) faultData = "devicefault-data:khr";
                break;
            default:
                faultSection += "extension=none\n";
                m_humanText += "device fault : unavailable (neither VK_EXT_device_fault nor "
                               "VK_KHR_device_fault was enabled)\n";
                break;
            }
            envelope.activeLayers.emplace_back(faultData);
            deviceSection += std::string("deviceFaultData=") + faultData + "\n";

            // Added even when empty: "queried and had nothing" is a different
            // answer from "never queried", and the section table is the
            // inventory that distinguishes them.
            m_raw.Add("vk.fault", faultSection);
            m_raw.Add("vk.device", deviceSection);

            m_humanText += "fault        : " + envelope.fault.type;
            if (!envelope.fault.address.empty()) m_humanText += " at " + envelope.fault.address;
            m_humanText += "\n";
        }

        void VulkanCrashBackend::FillReport(Diag::Envelope& envelope,
                                            std::string& humanText,
                                            const std::filesystem::path& reportStem)
        {
            CollectFault(envelope);
            // A lost device freezes the ring: the frames the host keeps
            // pumping after removal must not evict the crash-time timeline
            // out from under the LATER reports of the same cascade.
            Diag::FreezeBreadcrumbsOnDeviceLoss(m_breadcrumbs, envelope);
            Diag::EmitGpuDumpSibling(m_raw, envelope, m_humanText, reportStem);
            humanText += m_humanText;
        }
    }

    // -------------------------------------------------------------------------
    // Public entry points
    // -------------------------------------------------------------------------

    std::unique_ptr<IGpuCrashBackend> MakeVulkanCrashBackend(const VulkanCrashDesc& desc)
    {
        if (!desc.device)
            return nullptr;
        return std::make_unique<VulkanCrashBackend>(desc);
    }

    void VulkanGpuSectionProvider(Diag::Envelope& envelope,
                                  std::string& humanText,
                                  const std::filesystem::path& reportStem,
                                  void* user)
    {
        if (!user)
            return;
        // `user` was handed to SetGpuSectionProvider as an IGpuCrashBackend*,
        // so the round trip goes back through that type before down-casting.
        auto* backend = static_cast<VulkanCrashBackend*>(static_cast<IGpuCrashBackend*>(user));
        backend->FillReport(envelope, humanText, reportStem);
    }
}
