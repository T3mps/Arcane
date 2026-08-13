#pragma once

// GPU crash diagnostics arc (Task 3): the seam a GPU backend implements
// (Task 5 = D3D12 via WriteBufferImmediate + DRED, Task 6 = Vulkan via the
// AMD buffer-marker / device-fault extensions). Pure interface -- no GPU
// calls live in THIS header, only shape. NEVER wrap nvrhi::ICommandList
// (NVRHI boundary rule) -- WriteMarker passes the raw pointer straight
// through to the backend, nothing more.
//
// Task 5 also parks the `.gpudump` container here (Diag::GpuDumpWriter +
// ParseGpuDump/ReadGpuDump): it is the RAW-capture sibling every backend
// writes, so it belongs next to the seam rather than inside either
// backend's TU. Deliberately header-only -- it is pure byte shuffling with
// no state, so a test links it without pulling in a GPU backend.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration only -- MakeVulkanCrashBackend needs the UNWRAPPED
// nvrhi Vulkan device (queueGetCompletedInstance is declared there, not on
// nvrhi::IDevice), but this header is also included by the D3D12 device TU
// and must not drag <nvrhi/vulkan.h> / <vulkan/vulkan.h> in behind it.
namespace nvrhi::vulkan
{
    class IDevice;
}

namespace Arcane::Diag
{
    struct Envelope; // <Arcane/Base/DiagEnvelope.hpp> -- forward-declared to keep this header light

    // -----------------------------------------------------------------
    // `.gpudump` -- the raw-capture container (the GPU analog of `.dmp`)
    // -----------------------------------------------------------------
    // Written beside the report's `.txt`/`.dmp`/`.arcdiag` for every gpu
    // kind, ALWAYS -- even when collection was partial, because the
    // section table doubles as the capture inventory ("markers present,
    // DRED absent" is itself the answer). The `.arcdiag` stays the PARSED
    // summary; this file is what the backend actually read out.
    //
    // Layout, little-endian throughout:
    //
    //   'A' 'G' 'P' 'U'                       magic
    //   u32 version                           kGpuDumpVersion
    //   u32 sectionCount
    //   sectionCount x {
    //       char     tag[16]                  NUL-padded, never NUL-terminated-required
    //       u64      offset                   absolute, from the start of the file
    //       u64      size
    //   }
    //   ... payloads, in table order, at the recorded offsets
    //
    // Section payloads are opaque here on purpose: a backend decides
    // whether a section is raw device memory (D3D12 marker bytes) or its
    // own flattening of a pointer-linked API structure (DRED breadcrumbs).
    inline constexpr char          kGpuDumpMagic[4]  = { 'A', 'G', 'P', 'U' };
    inline constexpr std::uint32_t kGpuDumpVersion   = 1;
    inline constexpr std::size_t   kGpuDumpTagBytes  = 16;
    inline constexpr std::size_t   kGpuDumpHeaderBytes = 12;                      // magic + version + count
    inline constexpr std::size_t   kGpuDumpEntryBytes  = kGpuDumpTagBytes + 8 + 8; // tag + offset + size

    struct GpuDumpSection
    {
        std::string               tag;   // <= kGpuDumpTagBytes bytes
        std::vector<std::uint8_t> bytes;
    };

    struct GpuDump
    {
        std::uint32_t               version = kGpuDumpVersion;
        std::vector<GpuDumpSection> sections;
    };

    class GpuDumpWriter
    {
    public:
        // Appends one section. `tag` is truncated to kGpuDumpTagBytes; an
        // EMPTY section is legal and meaningful (it records that a layer
        // was armed but yielded nothing).
        void Add(std::string_view tag, const void* data, std::size_t size)
        {
            GpuDumpSection section;
            section.tag.assign(tag.substr(0, kGpuDumpTagBytes));
            if (const auto* first = static_cast<const std::uint8_t*>(data); first && size > 0)
                section.bytes.assign(first, first + size);
            m_sections.push_back(std::move(section));
        }

        void Add(std::string_view tag, std::string_view text) { Add(tag, text.data(), text.size()); }

        [[nodiscard]] std::size_t SectionCount() const noexcept { return m_sections.size(); }

        // The container's section names in table order -- the inventory a
        // report's human text quotes without re-reading the file.
        [[nodiscard]] std::string Inventory() const
        {
            std::string out;
            for (const GpuDumpSection& section : m_sections)
            {
                if (!out.empty()) out += ", ";
                out += section.tag;
            }
            return out;
        }

        [[nodiscard]] std::string Build() const
        {
            const std::uint32_t count = static_cast<std::uint32_t>(m_sections.size());
            std::size_t payloadBytes = 0;
            for (const GpuDumpSection& section : m_sections) payloadBytes += section.bytes.size();

            std::string out;
            out.reserve(kGpuDumpHeaderBytes + count * kGpuDumpEntryBytes + payloadBytes);

            auto put32 = [&out](std::uint32_t v) {
                for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFFu));
            };
            auto put64 = [&out](std::uint64_t v) {
                for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFFull));
            };

            out.append(kGpuDumpMagic, 4);
            put32(kGpuDumpVersion);
            put32(count);

            std::uint64_t offset = kGpuDumpHeaderBytes + static_cast<std::uint64_t>(count) * kGpuDumpEntryBytes;
            for (const GpuDumpSection& section : m_sections)
            {
                char tag[kGpuDumpTagBytes]{};
                const std::size_t tagBytes =
                    section.tag.size() < kGpuDumpTagBytes ? section.tag.size() : kGpuDumpTagBytes;
                std::memcpy(tag, section.tag.data(), tagBytes);
                out.append(tag, kGpuDumpTagBytes);
                put64(offset);
                put64(static_cast<std::uint64_t>(section.bytes.size()));
                offset += section.bytes.size();
            }
            for (const GpuDumpSection& section : m_sections)
            {
                if (!section.bytes.empty())
                    out.append(reinterpret_cast<const char*>(section.bytes.data()), section.bytes.size());
            }
            return out;
        }

        // Build() written to `path` in binary mode. False on IO failure --
        // the caller must NOT record a siblingGpuDump path it didn't write.
        [[nodiscard]] bool Write(const std::filesystem::path& path) const
        {
            const std::string bytes = Build();
            std::FILE* file = nullptr;
#if defined(_WIN32)
            if (::fopen_s(&file, path.string().c_str(), "wb") != 0 || !file) return false;
#else
            file = std::fopen(path.string().c_str(), "wb");
            if (!file) return false;
#endif
            const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
            return written == bytes.size();
        }

    private:
        std::vector<GpuDumpSection> m_sections;
    };

    // Bytes -> GpuDump. nullopt on a short buffer, a bad magic, an
    // unsupported version, or a section table entry whose offset/size runs
    // past the end -- a truncated capture must fail loudly, not hand back
    // half a section.
    [[nodiscard]] inline std::optional<GpuDump> ParseGpuDump(std::string_view bytes)
    {
        if (bytes.size() < kGpuDumpHeaderBytes) return std::nullopt;
        if (std::memcmp(bytes.data(), kGpuDumpMagic, 4) != 0) return std::nullopt;

        auto get32 = [&bytes](std::size_t at) {
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (i * 8);
            return v;
        };
        auto get64 = [&bytes](std::size_t at) {
            std::uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at + i])) << (i * 8);
            return v;
        };

        GpuDump dump;
        dump.version = get32(4);
        if (dump.version != kGpuDumpVersion) return std::nullopt;

        const std::uint32_t count = get32(8);
        if (bytes.size() < kGpuDumpHeaderBytes + static_cast<std::size_t>(count) * kGpuDumpEntryBytes)
            return std::nullopt;

        dump.sections.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::size_t at = kGpuDumpHeaderBytes + static_cast<std::size_t>(i) * kGpuDumpEntryBytes;

            std::string tag(bytes.substr(at, kGpuDumpTagBytes));
            if (const std::size_t nul = tag.find('\0'); nul != std::string::npos) tag.resize(nul);

            const std::uint64_t offset = get64(at + kGpuDumpTagBytes);
            const std::uint64_t size   = get64(at + kGpuDumpTagBytes + 8);
            if (offset > bytes.size() || size > bytes.size() - offset) return std::nullopt;

            GpuDumpSection section;
            section.tag = std::move(tag);
            const auto* first = reinterpret_cast<const std::uint8_t*>(bytes.data()) + offset;
            section.bytes.assign(first, first + size);
            dump.sections.push_back(std::move(section));
        }
        return dump;
    }

    // `path` read in full, then ParseGpuDump. nullopt on IO failure too.
    [[nodiscard]] inline std::optional<GpuDump> ReadGpuDump(const std::filesystem::path& path)
    {
        std::FILE* file = nullptr;
#if defined(_WIN32)
        if (::fopen_s(&file, path.string().c_str(), "rb") != 0 || !file) return std::nullopt;
#else
        file = std::fopen(path.string().c_str(), "rb");
        if (!file) return std::nullopt;
#endif
        std::string bytes;
        char buffer[4096];
        while (const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file))
            bytes.append(buffer, read);
        std::fclose(file);
        return ParseGpuDump(bytes);
    }
}

namespace Arcane
{
    class GpuBreadcrumbs; // <Arcane/Render/GpuBreadcrumbs.hpp>

    class ARCANE_API IGpuCrashBackend
    {
    public:
        virtual ~IGpuCrashBackend() = default;

        // Emits a begin (true) or end (false) marker for scope `id` on
        // `commandList` (already open). False on failure (e.g. the
        // required feature/extension isn't available) -- the caller
        // decides whether that's fatal.
        virtual bool WriteMarker(nvrhi::ICommandList*, std::uint32_t id, bool begin) = 0;

        // The same marker, for a producer that holds no nvrhi::ICommandList:
        // Phase 2's NRI frame graph, whose native command list comes from
        // nri::CoreInterface::GetCommandBufferNativeObject instead of
        // nvrhi::ICommandList::getNativeObject. `nativeCommandList` is the
        // backend's own native type (ID3D12GraphicsCommandList* on D3D12,
        // VkCommandBuffer on Vulkan) and must already be open, exactly as
        // above.
        //
        // This is the SAME marker layer, not a parallel one: WriteMarker
        // above resolves its native pointer and then calls straight into
        // this, so both producers write into one marker buffer and one crash
        // report. Passing the wrong backend's native pointer is undefined --
        // there is nothing to type-check it against, which is why the two
        // entry points stay separate rather than one void* overload.
        virtual bool WriteMarkerNative(void* nativeCommandList, std::uint32_t id, bool begin) = 0;

        // Fills `envelope`'s fault-classification fields (and anything
        // else this backend can determine -- DRED breadcrumbs, device-fault
        // page/address, ...) from whatever GPU-side crash state it can
        // retrieve at the point a device-removed/hang is observed.
        virtual void CollectFault(Diag::Envelope&) = 0;

        // The CPU-side scope ring this backend's marker ids index into --
        // owned by the backend, per GpuBreadcrumbs.hpp ("a backend calls
        // BeginScope/EndScope around each render pass"). A pass-scope
        // helper takes its token from BeginScope here and hands the same
        // token to WriteMarker; CollectFault turns observed marker values
        // back into NAMED queue timelines through it.
        virtual GpuBreadcrumbs& Breadcrumbs() = 0;

        // Short identifying label ("D3D12", "Vulkan") for logs and the
        // envelope's activeLayers.
        virtual const char* Name() const = 0;
    };

    // ---------------------------------------------------------------------
    // D3D12 backend (Task 5) -- implemented in GpuCrashD3D12.cpp
    // ---------------------------------------------------------------------

    // Applies the build-config DRED policy tier (F-2: full in Debug/Release,
    // markers-only in Dist) down the F-2d QueryInterface ladder. MUST be
    // called BEFORE D3D12CreateDevice -- DRED settings are process-global
    // and only affect devices created afterwards. Idempotent per process;
    // every failure is one WARN plus a degraded tier, never fatal.
    ARCANE_API void EnableD3D12Dred();

    // The D3D12 crash backend over an nvrhi D3D12 device (the native
    // ID3D12Device comes from getNativeObject, F-4). Null only if `device`
    // is null: a backend whose marker buffer or DRED tier failed to arm is
    // still returned, still collects whatever remains, and says so in the
    // envelope's activeLayers.
    ARCANE_API std::unique_ptr<IGpuCrashBackend> MakeD3D12CrashBackend(nvrhi::IDevice* device);

    // The Diagnostics::GpuSectionProvider for a backend made above -- pass
    // the IGpuCrashBackend* as `user`. Runs CollectFault, appends the
    // human-readable GPU block, and writes <reportStem>.gpudump for gpu
    // kinds, recording siblingGpuDump ONLY when the file actually landed.
    // Installed by the device layer, which owns the one
    // SetGpuSectionProvider call per host lifetime.
    ARCANE_API void D3D12GpuSectionProvider(Diag::Envelope& envelope,
                                            std::string& humanText,
                                            const std::filesystem::path& reportStem,
                                            void* user);

    // ---------------------------------------------------------------------
    // Vulkan backend (Task 6) -- implemented in GpuCrashVulkan.cpp
    // ---------------------------------------------------------------------

    // What DeviceVulkan actually managed to enable at device creation (F-5).
    // Both diagnostics extensions are request-if-available: an absent one
    // degrades exactly one layer and NEVER fails device creation, so the
    // backend is TOLD which rungs it got rather than re-deriving them (it
    // cannot -- a VkDevice does not report the extension list it was created
    // with, and re-enumerating the physical device would only say what was
    // AVAILABLE, not what was enabled).
    struct VulkanCrashDesc
    {
        // `VK_EXT_device_fault` and its `VK_KHR_device_fault` promotion are
        // DISTINCT surfaces, not aliases: the feature structs differ
        // (VkPhysicalDeviceFaultFeaturesEXT vs ...KHR) and so do the queries
        // (vkGetDeviceFaultInfoEXT vs vkGetDeviceFaultReportsKHR). Which
        // spelling was enabled therefore has to travel with the flag.
        enum class DeviceFault : std::uint8_t { None, Ext, Khr };

        // Possibly validation-wrapped -- getNativeObject forwards verbatim
        // (validation-device.cpp:85, validation-commandlist.cpp:131), so the
        // native VkDevice/VkPhysicalDevice and every native command buffer
        // resolve the same either way.
        nvrhi::IDevice* device = nullptr;

        // The UNWRAPPED backend device. `queueGetCompletedInstance`
        // (nvrhi/vulkan.h:45) is declared on nvrhi::vulkan::IDevice only, and
        // it is the fence-progress source the `breadcrumbs:fence` degrade path
        // derives scope completion from when VK_AMD_buffer_marker is absent.
        nvrhi::vulkan::IDevice* backendDevice = nullptr;

        DeviceFault deviceFault  = DeviceFault::None;
        bool        bufferMarker = false;
    };

    // The Vulkan crash backend. Null only if `desc.device` is null: a backend
    // whose marker buffer failed to arm, or that got neither optional
    // extension, is still returned, still collects whatever remains, and says
    // exactly which layers engaged in the envelope's activeLayers.
    ARCANE_API std::unique_ptr<IGpuCrashBackend> MakeVulkanCrashBackend(const VulkanCrashDesc& desc);

    // The Diagnostics::GpuSectionProvider for a backend made above -- pass the
    // IGpuCrashBackend* as `user`. Same contract as D3D12GpuSectionProvider:
    // runs CollectFault, appends the human-readable GPU block, and writes
    // <reportStem>.gpudump for gpu kinds, recording siblingGpuDump ONLY when
    // the file actually landed. Installed by the device layer, which owns the
    // one SetGpuSectionProvider call per host lifetime.
    ARCANE_API void VulkanGpuSectionProvider(Diag::Envelope& envelope,
                                             std::string& humanText,
                                             const std::filesystem::path& reportStem,
                                             void* user);
}
