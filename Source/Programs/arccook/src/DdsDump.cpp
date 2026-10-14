#include "DdsDump.hpp"

#include <cstdint>
#include <fstream>
#include <span>
#include <vector>

namespace arccook
{
    namespace
    {
        using Arcane::AssetPipeline::ArtifactPixelFormat;
        using Arcane::AssetPipeline::LoadedArtifact;
        using Arcane::AssetPipeline::MipDesc;

        // ---- DDS constants (see MSDN "Programming Guide for DDS") -----------------------
        constexpr std::uint32_t kDdsdCaps        = 0x1;
        constexpr std::uint32_t kDdsdHeight      = 0x2;
        constexpr std::uint32_t kDdsdWidth       = 0x4;
        constexpr std::uint32_t kDdsdPitch       = 0x8;
        constexpr std::uint32_t kDdsdPixelFormat = 0x1000;
        constexpr std::uint32_t kDdsdMipMapCount = 0x20000;
        constexpr std::uint32_t kDdsdLinearSize  = 0x80000;

        constexpr std::uint32_t kDdpfAlphaPixels = 0x1;
        constexpr std::uint32_t kDdpfFourCC      = 0x4;
        constexpr std::uint32_t kDdpfRgb         = 0x40;

        constexpr std::uint32_t kDdsCapsTexture  = 0x1000;
        constexpr std::uint32_t kDdsCapsComplex  = 0x8;
        constexpr std::uint32_t kDdsCapsMipMap   = 0x400000;

        constexpr std::uint32_t kD3d10ResourceDimensionTexture2D = 3;
        constexpr std::uint32_t kDxgiFormatBc7Unorm     = 98;
        constexpr std::uint32_t kDxgiFormatBc7UnormSrgb = 99;

        // Explicit little-endian byte writer -- same discipline every other on-disk format
        // in this codebase uses (ArtifactFormat.cpp's ByteWriter, CookKey.cpp's Fnv1a64):
        // never a struct/scalar memcpy, so the file is byte-identical across toolchains.
        class ByteWriter
        {
        public:
            std::vector<std::byte> buf;

            void U8(std::uint8_t v) { buf.push_back(static_cast<std::byte>(v)); }
            void Char(char c) { U8(static_cast<std::uint8_t>(c)); }

            void U32(std::uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                    buf.push_back(static_cast<std::byte>(static_cast<unsigned char>((v >> (8 * i)) & 0xFFu)));
            }

            void Zeros(std::size_t n) { for (std::size_t i = 0; i < n; ++i) U8(0); }

            void Bytes(std::span<const std::byte> s) { buf.insert(buf.end(), s.begin(), s.end()); }
        };
    }

    bool WriteDds(const std::filesystem::path& path, const LoadedArtifact& artifact)
    {
        const auto& desc = artifact.desc;
        const bool isBc7 = (desc.format == ArtifactPixelFormat::BC7);

        ByteWriter w;

        // "DDS " magic.
        w.Char('D'); w.Char('D'); w.Char('S'); w.Char(' ');

        // ---- DDS_HEADER (124 bytes) ------------------------------------------------------
        std::uint32_t flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat;
        if (desc.mipCount > 1) flags |= kDdsdMipMapCount;

        std::uint32_t pitchOrLinearSize = 0;
        if (isBc7)
        {
            flags |= kDdsdLinearSize;
            // Top mip's own payload size IS its linear size (block-compressed formats
            // report the compressed byte count of mip 0, not a row pitch).
            pitchOrLinearSize = desc.mips.empty() ? 0 : static_cast<std::uint32_t>(desc.mips.front().size);
        }
        else
        {
            flags |= kDdsdPitch;
            pitchOrLinearSize = desc.width * 4u;   // RGBA8: 4 bytes/texel row pitch
        }

        w.U32(124);              // dwSize
        w.U32(flags);
        w.U32(desc.height);
        w.U32(desc.width);
        w.U32(pitchOrLinearSize);
        w.U32(0);                 // dwDepth
        w.U32(desc.mipCount);
        w.Zeros(11 * 4);          // dwReserved1[11]

        // Pixel format sub-struct (32 bytes).
        w.U32(32);   // dwSize
        if (isBc7)
        {
            w.U32(kDdpfFourCC);
            w.Char('D'); w.Char('X'); w.Char('1'); w.Char('0');   // dwFourCC == "DX10"
            w.U32(0);   // dwRGBBitCount
            w.U32(0); w.U32(0); w.U32(0); w.U32(0);                // masks unused for FourCC formats
        }
        else
        {
            w.U32(kDdpfRgb | kDdpfAlphaPixels);
            w.U32(0);   // dwFourCC (unused -- uncompressed)
            w.U32(32);  // dwRGBBitCount
            // Bytes in memory are [R,G,B,A]; read little-endian as one u32 that is
            // A<<24|B<<16|G<<8|R, so the channel masks below are the standard R8G8B8A8 set.
            w.U32(0x000000FFu);   // R
            w.U32(0x0000FF00u);   // G
            w.U32(0x00FF0000u);   // B
            w.U32(0xFF000000u);   // A
        }

        std::uint32_t caps = kDdsCapsTexture;
        if (desc.mipCount > 1) caps |= kDdsCapsComplex | kDdsCapsMipMap;
        w.U32(caps);
        w.U32(0);   // dwCaps2
        w.U32(0);   // dwCaps3
        w.U32(0);   // dwCaps4
        w.U32(0);   // dwReserved2

        // ---- DDS_HEADER_DXT10 (20 bytes), BC7 only ---------------------------------------
        if (isBc7)
        {
            w.U32(desc.srgb ? kDxgiFormatBc7UnormSrgb : kDxgiFormatBc7Unorm);
            w.U32(kD3d10ResourceDimensionTexture2D);
            w.U32(0);   // miscFlag
            w.U32(1);   // arraySize
            w.U32(0);   // miscFlags2 (alpha mode unknown)
        }

        // ---- Mip payload, mip 0 first, exactly as already laid out in the artifact -------
        for (const MipDesc& mip : desc.mips)
        {
            if (mip.offset + mip.size > artifact.payload.size()) return false;   // corrupt artifact
            w.Bytes(std::span<const std::byte>(artifact.payload.data() + mip.offset, mip.size));
        }

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
        ofs.flush();
        return ofs.good();
    }
}
