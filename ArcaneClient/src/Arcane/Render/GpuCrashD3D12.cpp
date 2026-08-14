// GPU crash diagnostics arc (Task 5): the D3D12 crash backend.
//
// Three independent layers, each of which degrades to "off" with exactly one
// WARN rather than failing device creation:
//
//   1. MARKERS (F-1). A per-scope begin/end value written by the GPU into
//      process-owned system memory -- VirtualAlloc ->
//      ID3D12Device3::OpenExistingHeapFromAddress -> CreatePlacedResource in
//      COPY_DEST -- so the values survive device removal and are read back
//      through the ORIGINAL CPU pointer, never a Map() on a device-owned
//      heap. Written with ID3D12GraphicsCommandList2::WriteBufferImmediate,
//      MARKER_IN for begin and MARKER_OUT for end (F-1b).
//   2. DRED (F-2). Enabled process-wide BEFORE D3D12CreateDevice, at the
//      build-config policy tier, down the F-2d QueryInterface ladder;
//      retrieved after removal by QI on the DEVICE (F-2b).
//   3. The `.gpudump` container (Diag::GpuDumpWriter, IGpuCrashBackend.hpp).
//      Raw marker bytes plus this backend's flattening of DRED's
//      pointer-linked breadcrumb/page-fault output -- written for every gpu
//      kind, even on partial collection.
//
// NVRHI boundary rules: no ICommandList is ever wrapped and no barrier is
// ever issued here. The only native access is getNativeObject at the two
// seams F-4 names (device, graphics command list). WriteBufferImmediate
// appears in this file and nowhere else in the engine.

#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/GpuCrashReport.hpp>

#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Arcane
{
    namespace
    {
        // ------------------------------------------------------------------
        // DRED policy tier (F-2) -- decided once, before any device exists
        // ------------------------------------------------------------------

        std::once_flag              g_dredOnce;
        std::atomic<const char*>    g_dredTier{ "dred:off" };

        // ------------------------------------------------------------------
        // Marker buffer geometry (F-1a / F-8e)
        // ------------------------------------------------------------------
        // The geometry itself is SHARED (Diag::kGpuMarker*, GpuCrashReport.hpp)
        // because Diag::ReplayMarkerBuffer reads this region back with those
        // same constants -- a writer and reader that could disagree would
        // silently replay garbage. These aliases keep the local code readable.
        constexpr std::uint32_t kMarkerSlots     = Diag::kGpuMarkerSlots;
        constexpr std::uint32_t kValuesPerSlot   = Diag::kGpuMarkerValuesPerSlot;
        constexpr std::size_t   kMarkerBytes     = Diag::kGpuMarkerBytes;

        // F-1a: Microsoft documents exactly one constraint on the region handed
        // to OpenExistingHeapFromAddress -- "The heap is created in system
        // memory and permits CPU access. It wraps the entire VirtualAlloc
        // region."
        // (learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress)
        // -- no alignment or size rule of its own. The binding constraint comes
        // from the CONSUMER: CreatePlacedResource places a buffer at
        // D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT (65536; vendored
        // ThirdParty/DirectX-Headers/include/directx/d3d12.h:928) and the placed
        // resource must fit inside the heap, so the region has to be at least
        // 64 KiB AND 64 KiB-aligned. VirtualAlloc supplies both for free: with
        // lpAddress == NULL "the specified address is rounded down to the
        // nearest multiple of the allocation granularity" (64 KiB on Windows
        // x64) and dwSize "is rounded up to the next page boundary"
        // (learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc),
        // so a request that is already a multiple of both is taken verbatim.
        // dwAllocationGranularity is read rather than assumed.
        [[nodiscard]] std::size_t MarkerRegionBytes()
        {
            SYSTEM_INFO info{};
            ::GetSystemInfo(&info);
            std::size_t alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            if (info.dwAllocationGranularity > alignment)
                alignment = info.dwAllocationGranularity;
            return ((kMarkerBytes + alignment - 1) / alignment) * alignment;
        }

        // Hex formatting is shared with the Vulkan backend -- Diag::HexU32 /
        // Diag::HexU64 (GpuCrashReport.hpp).
        using Diag::HexU32;
        using Diag::HexU64;

        [[nodiscard]] const char* RemovedReasonName(HRESULT hr)
        {
            switch (hr)
            {
            case S_OK:                              return "none";
            case DXGI_ERROR_DEVICE_HUNG:            return "DXGI_ERROR_DEVICE_HUNG";
            case DXGI_ERROR_DEVICE_REMOVED:         return "DXGI_ERROR_DEVICE_REMOVED";
            case DXGI_ERROR_DEVICE_RESET:           return "DXGI_ERROR_DEVICE_RESET";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:  return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
            case DXGI_ERROR_INVALID_CALL:           return "DXGI_ERROR_INVALID_CALL";
            case DXGI_ERROR_ACCESS_DENIED:          return "DXGI_ERROR_ACCESS_DENIED";
            default:                                return "unrecognized";
            }
        }

        // Envelope Fault::type vocabulary, HRESULT flavour (the DRED device
        // state and a non-zero page-fault VA both override this below).
        [[nodiscard]] const char* RemovedReasonKind(HRESULT hr)
        {
            switch (hr)
            {
            case S_OK:                              return "device-alive";
            case DXGI_ERROR_DEVICE_HUNG:            return "device-hung";
            case DXGI_ERROR_DEVICE_RESET:           return "device-reset";
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR:  return "driver-internal-error";
            default:                                return "device-removed";
            }
        }

        [[nodiscard]] const char* DeviceStateName(D3D12_DRED_DEVICE_STATE state)
        {
            switch (state)
            {
            case D3D12_DRED_DEVICE_STATE_HUNG:      return "hung";
            case D3D12_DRED_DEVICE_STATE_FAULT:     return "fault";
            case D3D12_DRED_DEVICE_STATE_PAGEFAULT: return "page-fault";
            default:                                return "unknown";
            }
        }

        // DRED names objects as either ANSI or wide, never reliably both.
        [[nodiscard]] std::string ObjectName(const char* ansi, const wchar_t* wide)
        {
            if (ansi && *ansi) return ansi;
            if (wide && *wide)
            {
                std::string out;
                for (const wchar_t* p = wide; *p; ++p)
                    out.push_back(*p < 128 ? static_cast<char>(*p) : '?');
                return out;
            }
            return {};
        }

        // ------------------------------------------------------------------
        // DRED output -> flat text, the `.gpudump` sections
        // ------------------------------------------------------------------
        //
        // DRED hands back pointer-linked lists into runtime-owned memory, so a
        // byte-for-byte "raw" dump would be meaningless. These serializations
        // ARE the raw capture for this backend: every field the API exposes,
        // flattened, nothing interpreted away. Auto-breadcrumb op codes stay
        // NUMERIC (D3D12_AUTO_BREADCRUMB_OP values) -- the container is the
        // unparsed sibling; the `.arcdiag` is the parsed summary.

        // Breadcrumb histories can be very long on a busy list; a crash dump
        // that is mostly one node's op stream helps nobody.
        constexpr std::uint32_t kMaxOpsPerNode = 8192;

        template <typename NodeT>
        [[nodiscard]] std::string SerializeBreadcrumbNodes(const NodeT* head)
        {
            std::string out;
            std::uint32_t index = 0;
            for (const NodeT* node = head; node != nullptr; node = node->pNext, ++index)
            {
                const std::uint32_t count = node->BreadcrumbCount;
                const std::uint32_t last  = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;

                out += "node " + std::to_string(index);
                out += " queue=\"" + ObjectName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW) + "\"";
                out += " list=\"" + ObjectName(node->pCommandListDebugNameA, node->pCommandListDebugNameW) + "\"";
                out += " lastCompleted=" + std::to_string(last);
                out += " of " + std::to_string(count) + "\n";

                if (node->pCommandHistory)
                {
                    const std::uint32_t emit = count < kMaxOpsPerNode ? count : kMaxOpsPerNode;
                    for (std::uint32_t op = 0; op < emit; ++op)
                    {
                        out += "  op " + std::to_string(op) + " = " +
                               std::to_string(static_cast<std::uint32_t>(node->pCommandHistory[op]));
                        if (op + 1 == last) out += "  <-- last completed";
                        out += "\n";
                    }
                    if (emit < count)
                        out += "  ... " + std::to_string(count - emit) + " more ops truncated\n";
                }

                if constexpr (requires { node->BreadcrumbContextsCount; })
                {
                    for (std::uint32_t c = 0; c < node->BreadcrumbContextsCount; ++c)
                    {
                        const auto& context = node->pBreadcrumbContexts[c];
                        out += "  context " + std::to_string(context.BreadcrumbIndex) + " = \"" +
                               ObjectName(nullptr, context.pContextString) + "\"\n";
                    }
                }
            }
            return out;
        }

        template <typename NodeT>
        void SerializeAllocationNodes(const char* label, const NodeT* head, std::string& out)
        {
            std::uint32_t index = 0;
            for (const NodeT* node = head; node != nullptr; node = node->pNext, ++index)
            {
                out += label;
                out += " " + std::to_string(index) + " type=" +
                       std::to_string(static_cast<std::uint32_t>(node->AllocationType)) +
                       " name=\"" + ObjectName(node->ObjectNameA, node->ObjectNameW) + "\"\n";
            }
        }

        template <typename NodeT>
        [[nodiscard]] std::string FirstAllocationName(const NodeT* head)
        {
            for (const NodeT* node = head; node != nullptr; node = node->pNext)
            {
                if (std::string name = ObjectName(node->ObjectNameA, node->ObjectNameW); !name.empty())
                    return name;
            }
            return {};
        }

        // ------------------------------------------------------------------
        // The backend
        // ------------------------------------------------------------------

        class D3D12CrashBackend final : public IGpuCrashBackend
        {
        public:
            explicit D3D12CrashBackend(nvrhi::IDevice* device);
            ~D3D12CrashBackend() override;

            bool WriteMarker(nvrhi::ICommandList* commandList, std::uint32_t id, bool begin) override;
            bool WriteMarkerNative(void* nativeCommandList, std::uint32_t id, bool begin) override;
            void CollectFault(Diag::Envelope& envelope) override;
            GpuBreadcrumbs& Breadcrumbs() override { return m_breadcrumbs; }
            const char* Name() const override { return "D3D12"; }
            // The device the marker heap was placed on (F-1a's
            // CreatePlacedResource) -- what a WriteBufferImmediate GPU virtual
            // address is only meaningful on. Null when no native device
            // resolved, which already means markers are off.
            [[nodiscard]] void* NativeDevice() const override { return m_device; }

            // The Diagnostics GPU-section body: CollectFault plus the human
            // block plus the `.gpudump` sibling. Not on the interface -- the
            // seam's CollectFault deliberately takes only an Envelope.
            void FillReport(Diag::Envelope& envelope,
                            std::string& humanText,
                            const std::filesystem::path& reportStem);

        private:
            bool ArmMarkerBuffer();
            void ReleaseMarkerBuffer();

            nvrhi::IDevice*             m_nvrhi = nullptr;
            ID3D12Device*               m_device = nullptr;   // non-owning: nvrhi owns it (F-4c)

            void*                       m_markerMemory = nullptr;   // the VirtualAlloc region
            std::size_t                 m_regionBytes  = 0;
            ComPtr<ID3D12Heap>          m_markerHeap;
            ComPtr<ID3D12Resource>      m_markerResource;
            D3D12_GPU_VIRTUAL_ADDRESS   m_markerGpuVa = 0;
            std::atomic<bool>           m_markersArmed{ false };
            std::atomic<bool>           m_markerFailureLogged{ false };

            GpuBreadcrumbs              m_breadcrumbs;
            Diag::GpuDumpWriter         m_raw;
            std::string                 m_humanText;
        };

        D3D12CrashBackend::D3D12CrashBackend(nvrhi::IDevice* device)
            : m_nvrhi(device)
        {
            // F-4a/F-4b: the native ID3D12Device through NVRHI's accessor.
            // DeviceD3D12's own D3D12Device() is TU-local (F-4c), and the
            // validation layer forwards getNativeObject, so this reaches the
            // real device either way.
            m_device = static_cast<ID3D12Device*>(
                m_nvrhi ? m_nvrhi->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer : nullptr);

            if (!m_device)
            {
                ARC_WARN("GPU crash backend: no native ID3D12Device from the nvrhi device; "
                         "markers and DRED retrieval are both disabled");
                return;
            }

            if (ArmMarkerBuffer())
            {
                m_markersArmed.store(true, std::memory_order_release);
            }
            else
            {
                ReleaseMarkerBuffer();
            }
        }

        D3D12CrashBackend::~D3D12CrashBackend()
        {
            ReleaseMarkerBuffer();
        }

        bool D3D12CrashBackend::ArmMarkerBuffer()
        {
            // F-1a: OpenExistingHeapFromAddress needs driver support for
            // existing heaps -- the same dependency DRED's own documentation
            // names, and for the same reason (the bytes must outlive the
            // device). The documented L0-custom-heap fallback carries NO
            // persistence-after-removal guarantee, so refusing markers beats
            // shipping a recipe that reads freed memory.
            D3D12_FEATURE_DATA_EXISTING_HEAPS existingHeaps{};
            if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS,
                                                     &existingHeaps, sizeof(existingHeaps))) ||
                existingHeaps.Supported == FALSE)
            {
                ARC_WARN("GPU markers disabled: the display driver does not support "
                         "D3D12_FEATURE_EXISTING_HEAPS, so no marker buffer can survive "
                         "device removal (F-1a)");
                return false;
            }

            ComPtr<ID3D12Device3> device3;
            if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&device3))))
            {
                ARC_WARN("GPU markers disabled: ID3D12Device3 unavailable "
                         "(no OpenExistingHeapFromAddress)");
                return false;
            }

            m_regionBytes = MarkerRegionBytes();

            // F-1a step 1: process-owned system memory. The D3D12 device never
            // owns this, which is exactly why it stays readable after removal.
            m_markerMemory = ::VirtualAlloc(nullptr, m_regionBytes,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!m_markerMemory)
            {
                ARC_WARN("GPU markers disabled: VirtualAlloc({} bytes) failed (error {})",
                         m_regionBytes, ::GetLastError());
                return false;
            }

            // F-1a step 2.
            if (FAILED(device3->OpenExistingHeapFromAddress(m_markerMemory, IID_PPV_ARGS(&m_markerHeap))))
            {
                ARC_WARN("GPU markers disabled: OpenExistingHeapFromAddress failed");
                return false;
            }

            // F-1a step 3 + F-1b: COPY_DEST is a hard requirement of
            // WriteBufferImmediate ("The receiving buffer (resource) must be in
            // the D3D12_RESOURCE_STATE_COPY_DEST state to be a valid
            // destination"). It is the initial state and never transitions --
            // there is no other consumer, and issuing a barrier here would
            // violate the NVRHI boundary rule anyway.
            D3D12_RESOURCE_DESC bufferDesc{};
            bufferDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Alignment          = 0;
            bufferDesc.Width              = m_regionBytes;
            bufferDesc.Height             = 1;
            bufferDesc.DepthOrArraySize   = 1;
            bufferDesc.MipLevels          = 1;
            bufferDesc.Format             = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count   = 1;
            bufferDesc.SampleDesc.Quality = 0;
            bufferDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            // F-1a (desk-verified 2026-08-12): heaps minted by
            // OpenExistingHeapFromAddress carry SHARED | SHARED_CROSS_ADAPTER |
            // ALLOW_ONLY_BUFFERS (0x421 observed, RTX 3070), and D3D12 requires
            // ALLOW_CROSS_ADAPTER on any resource placed in a
            // SHARED_CROSS_ADAPTER heap -- FLAG_NONE fails with E_INVALIDARG.
            bufferDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            const HRESULT placedHr = m_device->CreatePlacedResource(m_markerHeap.Get(), 0, &bufferDesc,
                                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                    IID_PPV_ARGS(&m_markerResource));
            if (FAILED(placedHr))
            {
                ARC_WARN("GPU markers disabled: CreatePlacedResource on the diagnostic heap failed (hr=0x{:08x})",
                         static_cast<std::uint32_t>(placedHr));
                return false;
            }

            m_markerGpuVa = m_markerResource->GetGPUVirtualAddress();
            std::memset(m_markerMemory, 0, m_regionBytes);  // 0 == Diag::kGpuMarkerUnwritten
            ARC_INFO("GPU markers armed: {} slots in a {}-byte diagnostic heap",
                     kMarkerSlots, m_regionBytes);
            return true;
        }

        void D3D12CrashBackend::ReleaseMarkerBuffer()
        {
            m_markersArmed.store(false, std::memory_order_release);
            m_markerResource.Reset();
            m_markerHeap.Reset();
            if (m_markerMemory)
            {
                ::VirtualFree(m_markerMemory, 0, MEM_RELEASE);
                m_markerMemory = nullptr;
            }
            m_markerGpuVa = 0;
            m_regionBytes = 0;
        }

        bool D3D12CrashBackend::WriteMarker(nvrhi::ICommandList* commandList, std::uint32_t id, bool begin)
        {
            if (!commandList)
                return false;

            // F-4b: NVRHI exposes only the BASE ID3D12GraphicsCommandList.
            // Resolving it is the ONLY thing this overload does that the
            // native one cannot -- everything past here is shared, so the two
            // producers (NVRHI passes and the NRI frame graph) write into one
            // marker buffer through one code path.
            return WriteMarkerNative(
                commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer,
                id, begin);
        }

        bool D3D12CrashBackend::WriteMarkerNative(void* nativeCommandList, std::uint32_t id, bool begin)
        {
            if (!m_markersArmed.load(std::memory_order_acquire))
                return false;

            // WriteBufferImmediate needs a QueryInterface up to ...List2.
            auto* baseList = static_cast<ID3D12GraphicsCommandList*>(nativeCommandList);
            ComPtr<ID3D12GraphicsCommandList2> list2;
            if (!baseList || FAILED(baseList->QueryInterface(IID_PPV_ARGS(&list2))))
            {
                // One WARN, then the layer is off for good: a per-draw log from
                // a failing marker path would bury the crash it exists to find.
                if (!m_markerFailureLogged.exchange(true, std::memory_order_acq_rel))
                {
                    ARC_WARN("GPU markers disabled: ID3D12GraphicsCommandList2 unavailable "
                             "(no WriteBufferImmediate) -- F-4b");
                }
                m_markersArmed.store(false, std::memory_order_release);
                return false;
            }

            const std::uint32_t slot = id % kMarkerSlots;
            const std::uint32_t word = slot * kValuesPerSlot + (begin ? 0u : 1u);

            D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter{};
            parameter.Dest  = m_markerGpuVa + std::uint64_t{ word } * sizeof(std::uint32_t);
            parameter.Value = id + 1;   // 0 stays reserved for "never reached"

            // F-1b: begin lands once preceding work has STARTED (MARKER_IN), so
            // "begin written, end missing" means the GPU entered the scope and
            // did not leave it. End is deferred until prior work has COMPLETED
            // through the pipeline (MARKER_OUT), so its presence is proof the
            // scope finished -- and MARKER_OUT does not stall what follows.
            // DEFAULT would carry no ordering guarantee and say nothing about
            // progress. pModes is always explicit; a null pModes would silently
            // mean DEFAULT for every write.
            const D3D12_WRITEBUFFERIMMEDIATE_MODE mode =
                begin ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN
                      : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;

            list2->WriteBufferImmediate(1, &parameter, &mode);
            return true;
        }

        void D3D12CrashBackend::CollectFault(Diag::Envelope& envelope)
        {
            m_raw       = Diag::GpuDumpWriter{};
            m_humanText.clear();

            m_humanText += "backend      : D3D12\n";

            // ---- markers -------------------------------------------------
            // F-1a step 4: replay through the ORIGINAL VirtualAlloc pointer --
            // never a Map() on a device-owned heap. The replay itself, the
            // "markers" section, and the breadcrumbs:pass/disarmed/off keys are
            // backend-agnostic and live in Diag::ReplayMarkerBuffer; a null
            // region there means `breadcrumbs:off`, which is exactly this
            // backend's no-marker-layer answer.
            Diag::ReplayMarkerBuffer(m_breadcrumbs, m_raw, envelope, m_markerMemory,
                                     m_markersArmed.load(std::memory_order_acquire));

            Diag::EmitQueueSnapshot(m_breadcrumbs, "graphics", envelope, m_humanText);

            // ---- device state --------------------------------------------
            std::string deviceSection;
            envelope.activeLayers.emplace_back(g_dredTier.load(std::memory_order_acquire));

            if (!m_device)
            {
                envelope.activeLayers.emplace_back("dred-data:none");
                m_humanText += "device       : <no native ID3D12Device>\n";
                m_raw.Add("d3d12.device", std::string_view{ "no native ID3D12Device\n" });
                return;
            }

            const HRESULT removedReason = m_device->GetDeviceRemovedReason();
            envelope.fault.type = RemovedReasonKind(removedReason);
            m_humanText += std::string("device state : ") + RemovedReasonName(removedReason) +
                           " (" + HexU32(static_cast<std::uint32_t>(removedReason)) + ")\n";
            deviceSection += std::string("removedReason=") +
                             HexU32(static_cast<std::uint32_t>(removedReason)) +
                             " (" + RemovedReasonName(removedReason) + ")\n";
            deviceSection += std::string("dredTier=") + g_dredTier.load(std::memory_order_acquire) + "\n";
            deviceSection += "markerSlots=" + std::to_string(kMarkerSlots) + "\n";
            deviceSection += std::string("markersArmed=") +
                             (m_markersArmed.load(std::memory_order_acquire) ? "1" : "0") + "\n";

            // ---- DRED retrieval ------------------------------------------
            // F-2b: DRED data comes from a QueryInterface on the DEVICE, not on
            // the debug interface. F-2d: descending ladder Data2 -> Data1 ->
            // Data, taking the first that succeeds and recording which rung.
            ComPtr<ID3D12DeviceRemovedExtendedData> dred;
            if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dred))))
            {
                envelope.activeLayers.emplace_back("dred-data:none");
                deviceSection += "dredData=none\n";
                m_humanText += "dred         : unavailable (no ID3D12DeviceRemovedExtendedData)\n";
                m_raw.Add("d3d12.device", deviceSection);
                return;
            }

            ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
            const bool hasData1 = SUCCEEDED(dred.As(&dred1));
            ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
            const bool hasData2 = SUCCEEDED(dred.As(&dred2));

            const char* dataTier = hasData2 ? "dred-data:2" : (hasData1 ? "dred-data:1" : "dred-data:0");
            envelope.activeLayers.emplace_back(dataTier);
            deviceSection += std::string("dredData=") + dataTier + "\n";
            m_humanText += std::string("dred         : ") + g_dredTier.load(std::memory_order_acquire) +
                           " / " + dataTier + "\n";

            // Device state is a Data2-only refinement (F-2d).
            if (hasData2)
            {
                const D3D12_DRED_DEVICE_STATE state = dred2->GetDeviceState();
                deviceSection += std::string("dredDeviceState=") +
                                 std::to_string(static_cast<std::uint32_t>(state)) +
                                 " (" + DeviceStateName(state) + ")\n";
                m_humanText += std::string("dred state   : ") + DeviceStateName(state) + "\n";
                if (state != D3D12_DRED_DEVICE_STATE_UNKNOWN)
                    envelope.fault.type = DeviceStateName(state);
            }

            // Auto-breadcrumbs: Output1 (with PIX contexts) when Data1+ exists.
            {
                std::string crumbs;
                if (hasData1)
                {
                    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output{};
                    if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&output)))
                        crumbs = SerializeBreadcrumbNodes(output.pHeadAutoBreadcrumbNode);
                }
                else
                {
                    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT output{};
                    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&output)))
                        crumbs = SerializeBreadcrumbNodes(output.pHeadAutoBreadcrumbNode);
                }
                // Added even when empty: "DRED was queried and had nothing" is
                // a different answer from "DRED was never queried", and the
                // section table is the inventory that distinguishes them.
                m_raw.Add("dred.breadcrumb", crumbs);
            }

            // Page fault: Output2 -> Output1 -> Output, same ladder.
            {
                std::string          pageFault;
                D3D12_GPU_VIRTUAL_ADDRESS faultVa = 0;
                std::string          faultingResource;

                if (hasData2)
                {
                    D3D12_DRED_PAGE_FAULT_OUTPUT2 output{};
                    if (SUCCEEDED(dred2->GetPageFaultAllocationOutput2(&output)))
                    {
                        faultVa = output.PageFaultVA;
                        pageFault += "pageFaultFlags=" +
                                     std::to_string(static_cast<std::uint32_t>(output.PageFaultFlags)) + "\n";
                        SerializeAllocationNodes("existing", output.pHeadExistingAllocationNode, pageFault);
                        SerializeAllocationNodes("freed", output.pHeadRecentFreedAllocationNode, pageFault);
                        faultingResource = FirstAllocationName(output.pHeadExistingAllocationNode);
                        if (faultingResource.empty())
                            faultingResource = FirstAllocationName(output.pHeadRecentFreedAllocationNode);
                    }
                }
                else if (hasData1)
                {
                    D3D12_DRED_PAGE_FAULT_OUTPUT1 output{};
                    if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&output)))
                    {
                        faultVa = output.PageFaultVA;
                        SerializeAllocationNodes("existing", output.pHeadExistingAllocationNode, pageFault);
                        SerializeAllocationNodes("freed", output.pHeadRecentFreedAllocationNode, pageFault);
                        faultingResource = FirstAllocationName(output.pHeadExistingAllocationNode);
                        if (faultingResource.empty())
                            faultingResource = FirstAllocationName(output.pHeadRecentFreedAllocationNode);
                    }
                }
                else
                {
                    D3D12_DRED_PAGE_FAULT_OUTPUT output{};
                    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&output)))
                    {
                        faultVa = output.PageFaultVA;
                        SerializeAllocationNodes("existing", output.pHeadExistingAllocationNode, pageFault);
                        SerializeAllocationNodes("freed", output.pHeadRecentFreedAllocationNode, pageFault);
                        faultingResource = FirstAllocationName(output.pHeadExistingAllocationNode);
                        if (faultingResource.empty())
                            faultingResource = FirstAllocationName(output.pHeadRecentFreedAllocationNode);
                    }
                }

                if (faultVa != 0)
                {
                    envelope.fault.type    = "page-fault";
                    envelope.fault.address = HexU64(faultVa);
                    pageFault.insert(0, "pageFaultVA=" + HexU64(faultVa) + "\n");
                }
                envelope.fault.resource = faultingResource;

                m_raw.Add("dred.pagefault", pageFault);
                m_humanText += "fault        : " + envelope.fault.type;
                if (!envelope.fault.address.empty()) m_humanText += " at " + envelope.fault.address;
                if (!envelope.fault.resource.empty()) m_humanText += " -> \"" + envelope.fault.resource + "\"";
                m_humanText += "\n";
            }

            m_raw.Add("d3d12.device", deviceSection);
        }

        void D3D12CrashBackend::FillReport(Diag::Envelope& envelope,
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

    void EnableD3D12Dred()
    {
        std::call_once(g_dredOnce, []() {
            // F-2b: "DRED settings are global to the process, and you must
            // configure them prior to creating a Direct3D 12 Device."
            // D3D12GetDebugInterface is the retrieval path for the SETTINGS
            // object; it does not enable the debug layer, so this is
            // independent of RenderDeviceDesc::enableD3D12DebugLayer.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
            if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
            {
                ARC_WARN("DRED unavailable: D3D12GetDebugInterface for "
                         "ID3D12DeviceRemovedExtendedDataSettings failed; continuing without "
                         "GPU breadcrumbs");
                g_dredTier.store("dred:off", std::memory_order_release);
                return;
            }

            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

            // F-2d: the Settings2 -> Settings1 -> Settings ladder. Only the
            // Settings1 rung may call SetBreadcrumbContextEnablement, only the
            // Settings2 rung may call UseMarkersOnlyAutoBreadcrumbs; the
            // headers on this machine carry both, the RUNTIME may not.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings1;
            const bool hasSettings1 = SUCCEEDED(settings.As(&settings1));
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings2> settings2;
            const bool hasSettings2 = SUCCEEDED(settings.As(&settings2));

            // PIX marker/event strings inside the breadcrumb list (DRED 1.2).
            // Wanted in every tier: it is what turns a breadcrumb op stream
            // into named scopes, and it is the ONLY readable content the
            // markers-only tier produces at all.
            //
            // The WARN sits ABOVE the tier split on purpose: losing Settings1
            // degrades BOTH arms, and it hurts the Dist arm hardest (numeric-
            // only ops from an already-sparse breadcrumb list). Every tier
            // label below therefore carries a `-nocontext` variant, so
            // activeLayers distinguishes the degrade in either build.
            if (hasSettings1)
            {
                settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
            else
            {
                ARC_WARN("DRED Settings1 unavailable (no SetBreadcrumbContextEnablement); "
                         "breadcrumbs will carry no PIX marker strings (F-2d)");
            }

#if defined(ARCANE_DIST)
            // Dist = the lightweight tier (F-2c): breadcrumbs only at
            // SetMarker/BeginEvent/EndEvent, page-fault reporting off (it is
            // separately documented as costing system memory plus object
            // create/destroy time).
            //
            // F-2c-bis: this tier is only worth anything if pass-scope
            // instrumentation ALSO emits nvrhi::ICommandList::beginMarker /
            // endMarker -- markers-only DRED with no markers yields an EMPTY
            // breadcrumb list, which is strictly worse than no DRED. That
            // obligation is Task 7's; this backend's own WriteBufferImmediate
            // markers do not satisfy it.
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);
            if (hasSettings2)
            {
                settings2->UseMarkersOnlyAutoBreadcrumbs(TRUE);
                g_dredTier.store(hasSettings1 ? "dred:markers-only" : "dred:markers-only-nocontext",
                                 std::memory_order_release);
            }
            else
            {
                ARC_WARN("DRED Settings2 unavailable (no UseMarkersOnlyAutoBreadcrumbs); "
                         "Dist falls back to full auto-breadcrumbs (F-2d)");
                g_dredTier.store(hasSettings1 ? "dred:breadcrumbs" : "dred:breadcrumbs-nocontext",
                                 std::memory_order_release);
            }
#else
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            g_dredTier.store(hasSettings1 ? "dred:full" : "dred:full-nocontext",
                             std::memory_order_release);
            (void)hasSettings2;
#endif
            ARC_INFO("DRED enabled: {}", g_dredTier.load(std::memory_order_acquire));
        });
    }

    std::unique_ptr<IGpuCrashBackend> MakeD3D12CrashBackend(nvrhi::IDevice* device)
    {
        if (!device)
            return nullptr;
        return std::make_unique<D3D12CrashBackend>(device);
    }

    void D3D12GpuSectionProvider(Diag::Envelope& envelope,
                                 std::string& humanText,
                                 const std::filesystem::path& reportStem,
                                 void* user)
    {
        if (!user)
            return;
        // `user` was handed to SetGpuSectionProvider as an IGpuCrashBackend*,
        // so the round trip goes back through that type before down-casting.
        auto* backend = static_cast<D3D12CrashBackend*>(static_cast<IGpuCrashBackend*>(user));
        backend->FillReport(envelope, humanText, reportStem);
    }
}
