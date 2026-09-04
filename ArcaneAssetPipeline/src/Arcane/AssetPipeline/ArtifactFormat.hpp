#pragma once

// Arcane::AssetPipeline::ArtifactFormat -- the ".arcart" texture artifact container: a small,
// stable-on-disk binary format the offline cook (arccook, later tasks) writes and the runtime
// content loader reads. Fixed little-endian header + a tagged section table (MipTable/Payload/
// Thumbnail) so future artifact kinds (F2c's mesh artifacts, discriminated via ContentKind) can
// add sections without breaking old readers -- the reader SKIPS an unrecognised section tag
// rather than failing on it (forward-compat).
//
// Every multi-byte field is written EXPLICITLY, one byte at a time, little-endian -- never a
// raw struct memcpy. Struct padding is compiler/ABI dependent; a memcpy'd struct would make the
// artifact non-deterministic across toolchains/configs, which breaks content-hash-based caching
// in later tasks (spec s4).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "Arcane/Guid.hpp"

namespace Arcane::AssetPipeline
{
    enum class ArtifactPixelFormat : std::uint8_t
    {
        RGBA8,
        BC7,
        BC5_Reserved,
        BC6H_Reserved,
    };

    enum class ArtifactDimension : std::uint8_t
    {
        Tex2D,
        Tex2DArray_Reserved,
        TexCube_Reserved,
        TexCubeArray_Reserved,
        Tex3D_Reserved,
    };

    // Tagged-section table entries. Values are stable on disk -- never renumber an existing
    // tag; a future section kind adds a NEW value. ReadTextureArtifact skips any tag it does
    // not recognise instead of failing (forward-compat pin).
    enum class SectionTag : std::uint32_t
    {
        MipTable  = 1,
        Payload   = 2,
        Thumbnail = 3,
    };

    // F2c's mesh artifacts discriminate on this same header field.
    enum class ContentKind : std::uint8_t
    {
        Texture = 1,
    };

    struct MipDesc
    {
        std::uint64_t offset;
        std::uint64_t size;
        std::uint32_t width, height;
    };

    // Header, serialized little-endian, explicit field order (see WriteTextureArtifact's body
    // for the exact on-disk order -- it mirrors this declaration order field-for-field).
    struct TextureArtifactDesc
    {
        ContentKind contentKind = ContentKind::Texture;   // spec s4 pins it; do not drop

        Guid sourceGuid;
        std::uint64_t sourceHash;
        std::uint32_t importerVersion;

        ArtifactPixelFormat format;
        ArtifactDimension dimension;   // Tex2D this slice
        std::uint32_t arrayOrDepth;    // 1 this slice

        std::uint32_t width, height, mipCount;
        bool srgb;

        std::vector<MipDesc> mips;     // slice-major, mip-minor when arrayOrDepth > 1

        std::uint32_t thumbWidth, thumbHeight;
    };

    // Writes a complete .arcart file at `path`: fixed header, then the section table, then the
    // MipTable/Payload/Thumbnail sections themselves. `payload` is the (already-encoded, e.g.
    // BC7-compressed) texel data for every mip back to back, addressed by each MipDesc's own
    // offset/size; `thumbRgba` is a small uncompressed RGBA8 preview. Returns false on any IO
    // failure (bad path, disk full, etc.) -- the file is not guaranteed to exist on false.
    [[nodiscard]] bool WriteTextureArtifact(const std::filesystem::path& path,
                                             const TextureArtifactDesc& desc,
                                             std::span<const std::byte> payload,
                                             std::span<const std::byte> thumbRgba);

    struct LoadedArtifact
    {
        TextureArtifactDesc desc;
        std::vector<std::byte> payload;
        std::vector<std::byte> thumbRgba;
    };

    // Reads and validates a .arcart file. Returns nullopt on bad magic, an unrecognised
    // artifactVersion, or a truncated/corrupt section table (a section's offset+size runs past
    // EOF). A SectionTag this reader does not recognise is skipped, not treated as an error --
    // that is what keeps future artifact sections forward-compatible with this reader.
    [[nodiscard]] std::optional<LoadedArtifact> ReadTextureArtifact(const std::filesystem::path& path);
}
