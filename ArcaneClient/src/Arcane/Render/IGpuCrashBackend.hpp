#pragma once

// GPU crash diagnostics: the seam a GPU backend implements. The ONE live
// implementation is NriGraphCrashBackend, in Nri/NriDiagnostics.cpp -- see
// the block below EnableD3D12Dred/DredTier for what the tree does NOT have.
// Pure interface -- no GPU calls live in THIS header, only shape.
// WriteMarkerNative passes the raw native command-list pointer straight
// through to the backend, nothing more.
//
// The `.gpudump` container is parked here too (Diag::GpuDumpWriter +
// ParseGpuDump/ReadGpuDump): it is the RAW-capture sibling every backend
// writes, so it belongs next to the seam rather than inside either
// backend's TU. Deliberately header-only -- it is pure byte shuffling with
// no state, so a test links it without pulling in a GPU backend.

#include <Arcane/Base/Api.hpp>

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
        // `nativeCommandList` (already open). False on failure (e.g. the
        // required feature/extension isn't available) -- the caller decides
        // whether that's fatal.
        //
        // `nativeCommandList` is the backend's own native type
        // (ID3D12GraphicsCommandList* on D3D12, VkCommandBuffer on Vulkan),
        // as the NRI frame graph gets it from
        // nri::CoreInterface::GetCommandBufferNativeObject.
        //
        // THE ONLY MARKER ENTRY POINT, with exactly one marker layer behind
        // it. Passing the wrong backend's native pointer is undefined (there
        // is nothing to type-check it against), which is what NativeDevice()
        // below exists to let a producer check for first.
        virtual bool WriteMarkerNative(void* nativeCommandList, std::uint32_t id, bool begin) = 0;

        // The native device this backend's marker buffer lives on
        // (ID3D12Device* on D3D12, VkDevice on Vulkan), or null if it never
        // resolved one. Comparable against
        // nri::CoreInterface::GetDeviceNativeObject, which is the ONLY way a
        // producer holding an NRI device can tell whether WriteMarkerNative
        // above is legal for it.
        //
        // WHY THIS IS ON THE SEAM AT ALL: a GPU marker is a WRITE, from a
        // command buffer, into the backend's marker buffer -- and both APIs
        // require the two to belong to ONE device (Vulkan:
        // VUID-vkCmdWriteBufferMarkerAMD-commonparent; D3D12:
        // WriteBufferImmediate takes a GPU virtual address, which is
        // meaningless on another device's address space). A producer holding
        // a device this backend was NOT built over MUST check before it
        // writes: doing that write across a device boundary fired 20
        // validation errors on the run that found it, and D3D12 had the same
        // bug and was merely mute.
        //
        // A null answer, or two devices that differ, means "no native markers
        // for you" -- never an error. The CPU-side breadcrumb ring
        // (Breadcrumbs()) is device-agnostic and keeps recording either way,
        // and it is the half that feeds today's hang/crash reports.
        [[nodiscard]] virtual void* NativeDevice() const = 0;

        // Fills `envelope`'s fault-classification fields (and anything
        // else this backend can determine -- DRED breadcrumbs, device-fault
        // page/address, ...) from whatever GPU-side crash state it can
        // retrieve at the point a device-removed/hang is observed.
        virtual void CollectFault(Diag::Envelope&) = 0;

        // The CPU-side scope ring this backend's marker ids index into --
        // owned by the backend, per GpuBreadcrumbs.hpp ("a backend calls
        // BeginScope/EndScope around each render pass"). A pass-scope
        // helper takes its token from BeginScope here and hands the same
        // token to WriteMarkerNative; CollectFault turns observed marker
        // values back into NAMED queue timelines through it.
        virtual GpuBreadcrumbs& Breadcrumbs() = 0;

        // Short identifying label ("D3D12", "Vulkan") for logs and the
        // envelope's activeLayers.
        virtual const char* Name() const = 0;
    };

    // ---------------------------------------------------------------------
    // The D3D12 DRED tier -- the whole of GpuCrashD3D12.cpp
    // ---------------------------------------------------------------------
    // Two free functions, and neither goes through a backend object: DRED
    // settings are process-global and are armed before any device exists.

    // Applies the build-config DRED policy tier down the F-2d QueryInterface
    // ladder. MUST be called BEFORE D3D12CreateDevice -- DRED settings are
    // process-global and only affect devices created afterwards. Idempotent
    // per process; every failure is one WARN plus a degraded tier, never
    // fatal.
    //
    // THE TIER: full auto-breadcrumbs in ALL THREE configs. F-2's original
    // split -- full in Debug/Release, markers-only in Dist -- is SUSPENDED.
    // Markers-only auto-breadcrumbs are worth something only if pass scopes
    // also emit GPU markers, and no marker producer exists while
    // WriteMarkerNative is a stub on the graph backend; selecting
    // markers-only would therefore yield an EMPTY breadcrumb list, silently,
    // in the one config nobody runs interactively. DiagnosticsTest's "dred
    // tier never selects markers-only while WriteMarkerNative is a stub" case
    // FORBIDS it in every config. The lighter tier is re-earnable when the
    // native NRI marker layer lands (F-2c-bis) -- in that order, and only in
    // that order. The Dist arm still differs in one respect:
    // SetPageFaultEnablement(FORCED_OFF).
    ARCANE_API void EnableD3D12Dred();

    // The DRED tier EnableD3D12Dred() actually selected (e.g. "dred:full",
    // "dred:markers-only" -- reserved, unreachable in all three configs;
    // see EnableD3D12Dred()'s comment above --,
    // "dred:off", or a "-nocontext" variant of any of those) -- otherwise
    // only observable in a log line. Exposed so tests can pin the
    // build-config policy tier (F-2c-bis) rather than trusting it by
    // inspection. "dred:off" before EnableD3D12Dred() has run.
    ARCANE_API const char* DredTier();

    // THERE IS NO GPU-API CRASH BACKEND IN THE TREE, and the gap is named
    // rather than left to be discovered: nothing reads DRED breadcrumbs or
    // page-fault state back on D3D12, and nothing reads
    // VK_EXT/KHR_device_fault or writes VK_AMD_buffer_marker on Vulkan.
    // GpuCrashD3D12.cpp survives only for EnableD3D12Dred/DredTier above,
    // which ARE live (DeviceCreationD3D12.cpp arms the tier before
    // D3D12CreateDevice; DiagnosticsTest pins it).
    //
    // The live crash path is NriGraphCrashBackend (Nri/NriDiagnostics.cpp),
    // whose payload is the CPU-side GpuBreadcrumbs ring plus the
    // marker-buffer replay. Restoring the readback belongs to the native NRI
    // marker layer, beside the F-2c-bis obligation that same layer carries.

    // ---------------------------------------------------------------------
    // Vulkan device-creation record -- the enum outlives its backend
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

        // ALL THIS STRUCT CARRIES is the DeviceFault spelling above and the
        // field below, because DeviceCreationVulkan RECORDS which
        // device-fault surface it managed to enable (DeviceCreationVulkan.cpp
        // :593-602, .hpp:98) independently of whether anything consumes it
        // yet. It is kept as a struct, rather than the enum being hoisted to
        // namespace scope, so those call sites keep spelling it
        // VulkanCrashDesc::DeviceFault.
        DeviceFault deviceFault = DeviceFault::None;
    };
}
