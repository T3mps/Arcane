#include "Arcane/AssetPipeline/ArtifactFormat.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <string>

namespace Arcane::AssetPipeline
{
    namespace
    {
        constexpr std::uint32_t kArtifactVersion = 1;
        constexpr std::array<std::uint8_t, 4> kMagic{
            static_cast<std::uint8_t>('A'), static_cast<std::uint8_t>('R'),
            static_cast<std::uint8_t>('C'), static_cast<std::uint8_t>('A'),
        };
        // On-disk width of one section table entry: tag(u32) + offset(u64) + size(u64). Shared
        // by both artifact kinds -- this is the CONTAINER's own top-level index, not either
        // kind's per-content section table.
        constexpr std::uint64_t kSectionEntrySize = 4 + 8 + 8;
        // On-disk width of one MipTable entry: offset(u64) + size(u64) + width(u32) + height(u32).
        constexpr std::uint64_t kMipEntrySize = 8 + 8 + 4 + 4;
        // Minimum on-disk width of one MESH SectionTable entry (SectionTag::SectionTable's own
        // body): nameLen(u16) + indexOffset(u32) + indexCount(u32) + slotIndex(u32), i.e. the
        // width with a zero-length name -- a real entry is this wide plus its name's bytes.
        constexpr std::uint64_t kMeshSectionEntrySize = 2 + 4 + 4 + 4;

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

            void U16(std::uint16_t v)
            {
                for (int i = 0; i < 2; ++i)
                    buf.push_back(ByteOf(v, 8 * i));
            }

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

            // Floats go through their EXACT bit pattern, never a numeric conversion:
            // std::bit_cast preserves every bit including a signed zero or a NaN payload,
            // which a float -> integer conversion would not. Same byte-explicit,
            // endianness-explicit discipline as U32 above -- an AABB that round-trips
            // approximately is an AABB that fails the mesh's own framing math on the second
            // load.
            void F32(float v) { U32(std::bit_cast<std::uint32_t>(v)); }

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

            [[nodiscard]] bool U16(std::uint16_t& out) noexcept
            {
                if (m_pos + 2 > m_size) return false;
                out = 0;
                for (int i = 0; i < 2; ++i)
                    out |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(m_data[m_pos + i])) << (8 * i);
                m_pos += 2;
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

            [[nodiscard]] bool F32(float& out) noexcept
            {
                std::uint32_t bits = 0;
                if (!U32(bits)) return false;
                out = std::bit_cast<float>(bits);
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

        // Mirrors ArtifactReader.cpp's own kHeaderProbeBytes reasoning: generous headroom
        // over the fixed 37-byte common prefix (magic 4 + version 4 + contentKind 1 +
        // sourceGuid 16 + sourceHash 8 + importerVersion 4) ReadArtifactPrefix actually
        // parses, so a future prefix field added to either side does not silently start
        // under-reading -- still minuscule next to a real artifact's payload, which is the
        // whole point: RebuildIndexFromScan must never pay for anything past the header.
        constexpr std::size_t kPrefixProbeBytes = 256;

        // Reads at most `maxBytes` from the START of `path` -- NEVER the whole file. A file
        // shorter than `maxBytes` on disk reads however many bytes actually exist; the
        // bounds-checked ByteReader accessors then correctly refuse anything truncated
        // inside the prefix, exactly as they would reading the same short buffer out of a
        // full-file read. `ifs.bad()` (a genuine IO error) is distinguished from merely
        // hitting EOF before `maxBytes` (failbit/eofbit, not badbit -- the ordinary "short
        // file" case, not a failure).
        [[nodiscard]] std::optional<std::vector<std::byte>> ReadFilePrefix(
            const std::filesystem::path& path, std::size_t maxBytes)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) return std::nullopt;

            std::vector<std::byte> raw(maxBytes);
            ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(maxBytes));
            if (ifs.bad()) return std::nullopt;

            const std::streamsize got = ifs.gcount();
            if (got < 0) return std::nullopt;
            raw.resize(static_cast<std::size_t>(got));
            return raw;
        }
    }

    std::optional<ArtifactPrefix> ReadArtifactPrefix(const std::filesystem::path& path)
    {
        const std::optional<std::vector<std::byte>> raw = ReadFilePrefix(path, kPrefixProbeBytes);
        if (!raw) return std::nullopt;

        ByteReader r(raw->data(), raw->size());

        std::uint8_t magic[4]{};
        for (std::uint8_t& b : magic)
            if (!r.U8(b)) return std::nullopt;
        if (magic[0] != kMagic[0] || magic[1] != kMagic[1] || magic[2] != kMagic[2] || magic[3] != kMagic[3])
            return std::nullopt;

        std::uint32_t version = 0;
        if (!r.U32(version)) return std::nullopt;
        if (version != kArtifactVersion) return std::nullopt;

        ArtifactPrefix prefix;

        std::uint8_t contentKind = 0;
        if (!r.U8(contentKind)) return std::nullopt;
        // NO contentKind GATE HERE, deliberately -- see this function's own doc comment
        // (ArtifactFormat.hpp): the whole point is to answer "whose guid is this" for ANY
        // kind. Each kind's own full reader (ReadTextureArtifact/ReadMeshArtifact) keeps
        // its own fail-closed check unchanged.
        prefix.contentKind = static_cast<ContentKind>(contentKind);

        if (!r.U64(prefix.sourceGuid.hi)) return std::nullopt;
        if (!r.U64(prefix.sourceGuid.lo)) return std::nullopt;
        if (!r.U64(prefix.sourceHash)) return std::nullopt;
        if (!r.U32(prefix.importerVersion)) return std::nullopt;

        return prefix;
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

    // ---- Mesh artifacts (F2c) --------------------------------------------------------------
    // Independent writer/reader pair -- see ArtifactFormat.hpp's file banner for why the two
    // kinds share ONE container format and ONE SectionTag space, and ArtifactFormat.hpp's own
    // struct comments for the A1/A5 design comparisons cited below.

    bool WriteMeshArtifact(const std::filesystem::path& path, const MeshArtifactDesc& desc,
                            std::span<const MeshArtifactVertex> vertices, std::span<const std::uint32_t> indices)
    {
        // The MESH's own SectionTable body (tag SectionTable): count + {nameLen(u16), name
        // bytes, indexOffset(u32), indexCount(u32), slotIndex(u32)} per section, IN ORDER.
        // This is NOT the container's own section table below (that one has no tag and is a
        // fixed {tag,offset,size} index over the three top-level sections).
        ByteWriter sectionRecords;
        sectionRecords.U32(static_cast<std::uint32_t>(desc.sections.size()));
        for (const MeshArtifactSection& s : desc.sections)
        {
            // A name longer than 65535 bytes is clamped with one comment saying so -- glTF
            // material names are identifiers, not documents.
            const std::size_t clampedLen = s.name.size() > 0xFFFFu ? 0xFFFFu : s.name.size();
            sectionRecords.U16(static_cast<std::uint16_t>(clampedLen));
            for (std::size_t k = 0; k < clampedLen; ++k)
                sectionRecords.U8(static_cast<std::uint8_t>(s.name[k]));
            sectionRecords.U32(s.indexOffset);
            sectionRecords.U32(s.indexCount);
            sectionRecords.U32(s.slotIndex);
        }

        // Vertex/index data are emitted through F32/U32 field by field, NOT a std::span
        // memcpy of the structs -- MeshArtifactVertex is eight floats with no padding on
        // every toolchain we build, but the artifact's whole determinism contract is "never
        // trust a struct's memory layout", and honouring it here costs one loop.
        ByteWriter vertexData;
        for (const MeshArtifactVertex& v : vertices)
        {
            vertexData.F32(v.px); vertexData.F32(v.py); vertexData.F32(v.pz);
            vertexData.F32(v.nx); vertexData.F32(v.ny); vertexData.F32(v.nz);
            vertexData.F32(v.u);  vertexData.F32(v.v);
        }

        ByteWriter indexData;
        for (std::uint32_t idx : indices)
            indexData.U32(idx);

        // Fixed header: magic, artifactVersion, then the desc fields in declaration order.
        ByteWriter header;
        for (std::uint8_t b : kMagic) header.U8(b);
        header.U32(kArtifactVersion);
        header.U8(static_cast<std::uint8_t>(desc.contentKind));
        header.U64(desc.sourceGuid.hi);
        header.U64(desc.sourceGuid.lo);
        header.U64(desc.sourceHash);
        header.U32(desc.importerVersion);
        header.U32(desc.vertexCount);
        header.U32(desc.indexCount);
        header.U32(desc.sectionCount);
        header.U8(desc.indexWidth);
        header.F32(desc.aabbMin[0]); header.F32(desc.aabbMin[1]); header.F32(desc.aabbMin[2]);
        header.F32(desc.aabbMax[0]); header.F32(desc.aabbMax[1]); header.F32(desc.aabbMax[2]);

        struct Section { SectionTag tag; std::span<const std::byte> data; };
        const std::array<Section, 3> sections{ {
            { SectionTag::VertexData,   std::span<const std::byte>(vertexData.buf) },
            { SectionTag::IndexData,    std::span<const std::byte>(indexData.buf) },
            { SectionTag::SectionTable, std::span<const std::byte>(sectionRecords.buf) },
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

    std::optional<LoadedMeshArtifact> ReadMeshArtifact(const std::filesystem::path& path)
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

        LoadedMeshArtifact loaded;
        MeshArtifactDesc& desc = loaded.desc;

        std::uint8_t contentKind = 0;
        if (!r.U8(contentKind)) return std::nullopt;
        // Fail closed on anything but Mesh -- the same gate ReadTextureArtifact carries for
        // Texture, so a mesh reader never "parses" (misreads) a texture artifact's bytes just
        // because the fixed header widths happen to still fit.
        if (contentKind != static_cast<std::uint8_t>(ContentKind::Mesh))
            return std::nullopt;
        desc.contentKind = static_cast<ContentKind>(contentKind);

        if (!r.U64(desc.sourceGuid.hi)) return std::nullopt;
        if (!r.U64(desc.sourceGuid.lo)) return std::nullopt;
        if (!r.U64(desc.sourceHash)) return std::nullopt;
        if (!r.U32(desc.importerVersion)) return std::nullopt;

        if (!r.U32(desc.vertexCount)) return std::nullopt;
        if (!r.U32(desc.indexCount)) return std::nullopt;
        if (!r.U32(desc.sectionCount)) return std::nullopt;

        if (!r.U8(desc.indexWidth)) return std::nullopt;
        // Comparison A5: the byte is DECLARED, not inferred. v1's only legal value is 4; a
        // file claiming otherwise is refused rather than read as though its indices were u32
        // anyway -- a future 16-bit path is a reader BRANCH, not a silent reinterpretation.
        if (desc.indexWidth != 4)
            return std::nullopt;

        for (float& c : desc.aabbMin) { if (!r.F32(c)) return std::nullopt; }
        for (float& c : desc.aabbMax) { if (!r.F32(c)) return std::nullopt; }

        std::uint32_t containerSectionCount = 0;
        if (!r.U32(containerSectionCount)) return std::nullopt;

        // Same cheap sanity bound as ReadTextureArtifact's own section table check -- this is
        // literally the SAME on-disk structure (the container's top-level {tag,offset,size}
        // index), shared by both kinds.
        if (static_cast<std::uint64_t>(containerSectionCount) * kSectionEntrySize > raw.size())
            return std::nullopt;

        std::vector<SectionTableEntry> entries;
        entries.reserve(containerSectionCount);
        for (std::uint32_t i = 0; i < containerSectionCount; ++i)
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
            case SectionTag::VertexData:
            {
                if (body.size() != static_cast<std::uint64_t>(desc.vertexCount) * sizeof(MeshArtifactVertex))
                    return std::nullopt;
                ByteReader vr(body.data(), body.size());
                loaded.vertices.clear();
                loaded.vertices.reserve(desc.vertexCount);
                for (std::uint32_t i = 0; i < desc.vertexCount; ++i)
                {
                    MeshArtifactVertex v{};
                    if (!vr.F32(v.px) || !vr.F32(v.py) || !vr.F32(v.pz) ||
                        !vr.F32(v.nx) || !vr.F32(v.ny) || !vr.F32(v.nz) ||
                        !vr.F32(v.u)  || !vr.F32(v.v))
                        return std::nullopt;
                    loaded.vertices.push_back(v);
                }
                break;
            }
            case SectionTag::IndexData:
            {
                if (body.size() != static_cast<std::uint64_t>(desc.indexCount) * 4)
                    return std::nullopt;
                ByteReader ir(body.data(), body.size());
                loaded.indices.clear();
                loaded.indices.reserve(desc.indexCount);
                for (std::uint32_t i = 0; i < desc.indexCount; ++i)
                {
                    std::uint32_t idx = 0;
                    if (!ir.U32(idx)) return std::nullopt;
                    loaded.indices.push_back(idx);
                }
                break;
            }
            case SectionTag::SectionTable:
            {
                ByteReader mr(body.data(), body.size());
                std::uint32_t meshSectionCount = 0;
                if (!mr.U32(meshSectionCount)) return std::nullopt;

                // Same reserve-bound guard the mip table has (I3): the minimum on-disk width
                // of one entry is kMeshSectionEntrySize (a zero-length name), so a count that
                // could not possibly fit in THIS section's own body is corrupt -- refuse
                // cleanly rather than let a crafted count throw bad_alloc out of the reserve
                // below.
                if (static_cast<std::uint64_t>(meshSectionCount) * kMeshSectionEntrySize > body.size())
                    return std::nullopt;

                desc.sections.clear();
                desc.sections.reserve(meshSectionCount);
                for (std::uint32_t i = 0; i < meshSectionCount; ++i)
                {
                    std::uint16_t nameLen = 0;
                    if (!mr.U16(nameLen)) return std::nullopt;

                    std::string name;
                    name.reserve(nameLen);   // nameLen is u16-bounded (<=65535) -- safe to reserve directly
                    for (std::uint16_t k = 0; k < nameLen; ++k)
                    {
                        std::uint8_t ch = 0;
                        if (!mr.U8(ch)) return std::nullopt;   // a truncated name IS a bounds failure
                        name.push_back(static_cast<char>(ch));
                    }

                    MeshArtifactSection section{};
                    section.name = std::move(name);
                    if (!mr.U32(section.indexOffset) || !mr.U32(section.indexCount) || !mr.U32(section.slotIndex))
                        return std::nullopt;
                    desc.sections.push_back(std::move(section));
                }
                break;
            }
            default:
                // Forward-compat: a section tag this reader doesn't recognise (e.g. a future
                // Tangents/Thumbnail body, or even one of the TEXTURE kind's own tags) is
                // skipped, not an error -- see the file banner's shared-tag-space rationale.
                break;
            }
        }

        // One extra validation the texture reader has no analogue for: a section pointing
        // past the index buffer is a corrupt file, and letting it through would hand Plan 2's
        // draw path an out-of-range CmdDrawIndexed.
        for (const MeshArtifactSection& section : desc.sections)
        {
            if (static_cast<std::uint64_t>(section.indexOffset) + section.indexCount > desc.indexCount)
                return std::nullopt;
            // SLOTINDEX BOUND (final-review fix, 2026-09-11; ArtifactFormat.hpp's banner): a
            // slotIndex at or past sectionCount cannot occur in a well-formed artifact
            // (every slot has >= 1 section, so max(slotIndex) + 1 <= sectionCount) and
            // would size SlotNamesFromSections' table -- and the editor's companion slot
            // array -- by a corrupt value. Refused, never clamped; the client reader
            // applies the identical rule.
            if (section.slotIndex >= desc.sectionCount)
                return std::nullopt;
        }
        // The header's declared sectionCount must agree with what the SectionTable section
        // actually carried (or, if that section was absent entirely, with zero).
        if (static_cast<std::uint32_t>(desc.sections.size()) != desc.sectionCount)
            return std::nullopt;

        // The header's declared vertexCount/indexCount must likewise agree with what was
        // actually DECODED -- a file that declares nonzero counts but omits the VertexData
        // and/or IndexData section entirely would otherwise pass every check above and come
        // back as a LoadedMeshArtifact with nonzero declared counts paired with empty arrays:
        // silently inconsistent output. An absent body disagrees with its declared count
        // maximally, the same class of corruption the per-section size checks above already
        // refuse for a body that is merely the WRONG size.
        if (loaded.vertices.size() != desc.vertexCount) return std::nullopt;
        if (loaded.indices.size() != desc.indexCount) return std::nullopt;

        return loaded;
    }

    std::vector<std::string> SlotNamesFromSections(std::span<const MeshArtifactSection> sections)
    {
        bool any = false;
        std::uint32_t maxSlot = 0;
        for (const MeshArtifactSection& s : sections)
        {
            any = true;
            if (s.slotIndex > maxSlot) maxSlot = s.slotIndex;
        }
        if (!any) return {};

        // Dedup BY NAME (A1): the first section that points at a given slot names it; every
        // later section pointing at the same slot agrees by construction (the two sections
        // sharing one glTF material share one name too), so "first wins" is equivalent to
        // "they all agree".
        std::vector<std::string> names(static_cast<std::size_t>(maxSlot) + 1);
        std::vector<bool> filled(names.size(), false);
        for (const MeshArtifactSection& s : sections)
        {
            if (!filled[s.slotIndex])
            {
                names[s.slotIndex] = s.name;
                filled[s.slotIndex] = true;
            }
        }
        return names;
    }
}
