#include "Arcane/AssetPipeline/ArtifactFormat.hpp"

#include <array>
#include <fstream>

namespace Arcane::AssetPipeline
{
    namespace
    {
        constexpr std::uint32_t kArtifactVersion = 1;
        constexpr std::array<std::uint8_t, 4> kMagic{
            static_cast<std::uint8_t>('A'), static_cast<std::uint8_t>('R'),
            static_cast<std::uint8_t>('C'), static_cast<std::uint8_t>('A'),
        };
        // On-disk width of one section table entry: tag(u32) + offset(u64) + size(u64).
        constexpr std::uint64_t kSectionEntrySize = 4 + 8 + 8;
        // On-disk width of one MipTable entry: offset(u64) + size(u64) + width(u32) + height(u32).
        constexpr std::uint64_t kMipEntrySize = 8 + 8 + 4 + 4;

        [[nodiscard]] constexpr std::byte ByteOf(std::uint64_t v, int shift) noexcept
        {
            return static_cast<std::byte>(static_cast<unsigned char>((v >> shift) & 0xFFu));
        }

        // Appends multi-byte integers little-endian, one explicit byte at a time -- never a
        // struct/scalar memcpy. Struct padding + host endianness would make the artifact
        // non-deterministic across toolchains, which breaks content-hash-based caching.
        class ByteWriter
        {
        public:
            std::vector<std::byte> buf;

            void U8(std::uint8_t v) { buf.push_back(static_cast<std::byte>(v)); }

            void U32(std::uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                    buf.push_back(ByteOf(v, 8 * i));
            }

            void U64(std::uint64_t v)
            {
                for (int i = 0; i < 8; ++i)
                    buf.push_back(ByteOf(v, 8 * i));
            }

            void Bytes(std::span<const std::byte> s) { buf.insert(buf.end(), s.begin(), s.end()); }

            [[nodiscard]] std::size_t Size() const noexcept { return buf.size(); }
        };

        // Bounds-checked little-endian reader over an in-memory buffer. Every accessor returns
        // false (never throws/UB) on a short read so callers can turn ANY truncation, anywhere
        // in the file, into a single ReadTextureArtifact() nullopt.
        class ByteReader
        {
        public:
            ByteReader(const std::byte* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

            [[nodiscard]] bool U8(std::uint8_t& out) noexcept
            {
                if (m_pos + 1 > m_size) return false;
                out = static_cast<std::uint8_t>(m_data[m_pos]);
                m_pos += 1;
                return true;
            }

            [[nodiscard]] bool U32(std::uint32_t& out) noexcept
            {
                if (m_pos + 4 > m_size) return false;
                out = 0;
                for (int i = 0; i < 4; ++i)
                    out |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(m_data[m_pos + i])) << (8 * i);
                m_pos += 4;
                return true;
            }

            [[nodiscard]] bool U64(std::uint64_t& out) noexcept
            {
                if (m_pos + 8 > m_size) return false;
                out = 0;
                for (int i = 0; i < 8; ++i)
                    out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(m_data[m_pos + i])) << (8 * i);
                m_pos += 8;
                return true;
            }

            // Copies `count` bytes starting at ABSOLUTE offset `at` against the whole backing
            // buffer (not the sequential cursor) -- section bodies are addressed by the
            // section table's own offset/size, not read in sequence.
            [[nodiscard]] bool Slice(std::uint64_t at, std::uint64_t count, std::vector<std::byte>& out) const
            {
                if (at > m_size || count > m_size - at) return false;
                const auto* begin = m_data + static_cast<std::size_t>(at);
                out.assign(begin, begin + static_cast<std::size_t>(count));
                return true;
            }

        private:
            const std::byte* m_data;
            std::size_t m_pos = 0;
            std::size_t m_size;
        };

        struct SectionTableEntry
        {
            std::uint32_t tag;
            std::uint64_t offset;
            std::uint64_t size;
        };
    }

    bool WriteTextureArtifact(const std::filesystem::path& path, const TextureArtifactDesc& desc,
                               std::span<const std::byte> payload, std::span<const std::byte> thumbRgba)
    {
        // MipTable section body: count + {offset, size, width, height} per mip, IN ORDER --
        // slice-major, mip-minor when arrayOrDepth > 1; the vector's own order IS that order,
        // never resorted.
        ByteWriter mipTable;
        mipTable.U32(static_cast<std::uint32_t>(desc.mips.size()));
        for (const MipDesc& mip : desc.mips)
        {
            mipTable.U64(mip.offset);
            mipTable.U64(mip.size);
            mipTable.U32(mip.width);
            mipTable.U32(mip.height);
        }

        // Fixed header: magic, artifactVersion, then the desc fields in declaration order.
        ByteWriter header;
        for (std::uint8_t b : kMagic) header.U8(b);
        header.U32(kArtifactVersion);
        header.U8(static_cast<std::uint8_t>(desc.contentKind));
        header.U64(desc.sourceGuid.hi);
        header.U64(desc.sourceGuid.lo);
        header.U64(desc.sourceHash);
        header.U32(desc.importerVersion);
        header.U8(static_cast<std::uint8_t>(desc.format));
        header.U8(static_cast<std::uint8_t>(desc.dimension));
        header.U32(desc.arrayOrDepth);
        header.U32(desc.width);
        header.U32(desc.height);
        header.U32(desc.mipCount);
        header.U8(desc.srgb ? 1 : 0);
        header.U32(desc.thumbWidth);
        header.U32(desc.thumbHeight);

        struct Section { SectionTag tag; std::span<const std::byte> data; };
        const std::array<Section, 3> sections{ {
            { SectionTag::MipTable,  std::span<const std::byte>(mipTable.buf) },
            { SectionTag::Payload,   payload },
            { SectionTag::Thumbnail, thumbRgba },
        } };

        const std::uint64_t sectionTableSize = 4 + sections.size() * kSectionEntrySize;   // + sectionCount

        ByteWriter sectionTable;
        sectionTable.U32(static_cast<std::uint32_t>(sections.size()));
        std::uint64_t runningOffset = header.Size() + sectionTableSize;
        for (const Section& s : sections)
        {
            sectionTable.U32(static_cast<std::uint32_t>(s.tag));
            sectionTable.U64(runningOffset);
            sectionTable.U64(static_cast<std::uint64_t>(s.data.size()));
            runningOffset += s.data.size();
        }

        ByteWriter file;
        file.buf.reserve(static_cast<std::size_t>(runningOffset));
        file.Bytes(header.buf);
        file.Bytes(sectionTable.buf);
        for (const Section& s : sections)
            file.Bytes(s.data);

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(file.buf.data()), static_cast<std::streamsize>(file.buf.size()));
        ofs.flush();
        return ofs.good();
    }

    std::optional<LoadedArtifact> ReadTextureArtifact(const std::filesystem::path& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return std::nullopt;

        std::vector<std::byte> raw;
        {
            ifs.seekg(0, std::ios::end);
            const std::streamoff len = ifs.tellg();
            if (len < 0) return std::nullopt;
            raw.resize(static_cast<std::size_t>(len));
            ifs.seekg(0, std::ios::beg);
            if (!raw.empty())
            {
                ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                if (!ifs) return std::nullopt;
            }
        }

        ByteReader r(raw.data(), raw.size());

        std::uint8_t magic[4]{};
        for (std::uint8_t& b : magic)
            if (!r.U8(b)) return std::nullopt;
        if (magic[0] != kMagic[0] || magic[1] != kMagic[1] || magic[2] != kMagic[2] || magic[3] != kMagic[3])
            return std::nullopt;

        std::uint32_t version = 0;
        if (!r.U32(version)) return std::nullopt;
        if (version != kArtifactVersion) return std::nullopt;

        LoadedArtifact loaded;
        TextureArtifactDesc& desc = loaded.desc;

        std::uint8_t contentKind = 0;
        if (!r.U8(contentKind)) return std::nullopt;
        // Minor fix (final-review wave): fail closed on an unexpected contentKind rather
        // than silently accepting it as this reader's own Texture-shaped body -- spares
        // F2c's mesh-artifact author a texture reader that happily "parses" (misreads) a
        // future artifact kind's bytes just because the fixed header widths still fit.
        if (contentKind != static_cast<std::uint8_t>(ContentKind::Texture))
            return std::nullopt;
        desc.contentKind = static_cast<ContentKind>(contentKind);

        if (!r.U64(desc.sourceGuid.hi)) return std::nullopt;
        if (!r.U64(desc.sourceGuid.lo)) return std::nullopt;
        if (!r.U64(desc.sourceHash)) return std::nullopt;
        if (!r.U32(desc.importerVersion)) return std::nullopt;

        std::uint8_t format = 0, dimension = 0;
        if (!r.U8(format)) return std::nullopt;
        desc.format = static_cast<ArtifactPixelFormat>(format);
        if (!r.U8(dimension)) return std::nullopt;
        desc.dimension = static_cast<ArtifactDimension>(dimension);

        if (!r.U32(desc.arrayOrDepth)) return std::nullopt;
        if (!r.U32(desc.width)) return std::nullopt;
        if (!r.U32(desc.height)) return std::nullopt;
        if (!r.U32(desc.mipCount)) return std::nullopt;

        std::uint8_t srgb = 0;
        if (!r.U8(srgb)) return std::nullopt;
        desc.srgb = (srgb != 0);

        if (!r.U32(desc.thumbWidth)) return std::nullopt;
        if (!r.U32(desc.thumbHeight)) return std::nullopt;

        std::uint32_t sectionCount = 0;
        if (!r.U32(sectionCount)) return std::nullopt;

        // Cheap sanity bound before reserving: each entry costs at least 20 bytes on disk, so
        // a section count that couldn't possibly fit in the remaining file is corrupt. Without
        // this a crafted sectionCount (e.g. 0xFFFFFFFF) would ask to reserve tens of GB before
        // the per-entry bounds checks below ever get a chance to fail closed.
        if (static_cast<std::uint64_t>(sectionCount) * kSectionEntrySize > raw.size())
            return std::nullopt;

        std::vector<SectionTableEntry> entries;
        entries.reserve(sectionCount);
        for (std::uint32_t i = 0; i < sectionCount; ++i)
        {
            SectionTableEntry e{};
            if (!r.U32(e.tag) || !r.U64(e.offset) || !r.U64(e.size)) return std::nullopt;
            entries.push_back(e);
        }

        for (const SectionTableEntry& e : entries)
        {
            std::vector<std::byte> body;
            if (!r.Slice(e.offset, e.size, body))
                return std::nullopt;   // section runs past EOF -- truncated/corrupt file

            switch (static_cast<SectionTag>(e.tag))
            {
            case SectionTag::MipTable:
            {
                ByteReader mr(body.data(), body.size());
                std::uint32_t mipCount = 0;
                if (!mr.U32(mipCount)) return std::nullopt;

                // I3 fix (final-review wave): same cheap sanity bound as the section
                // table's own check above, same rationale -- each mip entry costs at
                // least kMipEntrySize bytes on disk, so a mip count that could not
                // possibly fit in THIS section's own body is corrupt. Without this, a
                // crafted mipCount (e.g. 0xFFFFFFFF) would ask std::vector::reserve for
                // ~64 GB and THROW std::bad_alloc out of this read instead of refusing
                // it cleanly (nullopt).
                if (static_cast<std::uint64_t>(mipCount) * kMipEntrySize > body.size())
                    return std::nullopt;

                desc.mips.clear();
                desc.mips.reserve(mipCount);
                for (std::uint32_t i = 0; i < mipCount; ++i)
                {
                    MipDesc mip{};
                    if (!mr.U64(mip.offset) || !mr.U64(mip.size) || !mr.U32(mip.width) || !mr.U32(mip.height))
                        return std::nullopt;
                    desc.mips.push_back(mip);
                }
                break;
            }
            case SectionTag::Payload:
                loaded.payload = std::move(body);
                break;
            case SectionTag::Thumbnail:
                loaded.thumbRgba = std::move(body);
                break;
            default:
                // Forward-compat: a section tag this reader doesn't recognise (e.g. a future
                // artifact kind's section) is skipped, not an error.
                break;
            }
        }

        return loaded;
    }
}
