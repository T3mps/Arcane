#include <Arcane/Assets/ArtifactReader.hpp>

#include <Json.hpp>

#include <array>
#include <bit>
#include <fstream>
#include <string>
#include <system_error>

namespace Arcane
{
    namespace
    {
        constexpr std::uint32_t kArtifactVersion = 1;
        constexpr std::array<std::uint8_t, 4> kMagic{
            static_cast<std::uint8_t>('A'), static_cast<std::uint8_t>('R'),
            static_cast<std::uint8_t>('C'), static_cast<std::uint8_t>('A'),
        };
        // On-disk width of one section table entry: tag(u32) + offset(u64) + size(u64).
        // Mirrors ArtifactFormat.cpp's own kSectionEntrySize -- duplicated, not shared,
        // per this file's no-pipeline-code banner (ArtifactReader.hpp).
        constexpr std::uint64_t kSectionEntrySize = 4 + 8 + 8;
        // On-disk width of one MipTable entry: offset(u64) + size(u64) + width(u32) +
        // height(u32). Mirrors ArtifactFormat.cpp's own kMipEntrySize -- same duplication
        // discipline as kSectionEntrySize above.
        constexpr std::uint64_t kMipEntrySize = 8 + 8 + 4 + 4;
        // Minimum on-disk width of one MESH SectionTable entry (a zero-length name):
        // nameLen(u16) + indexOffset(u32) + indexCount(u32) + slotIndex(u32). Mirrors
        // ArtifactFormat.cpp's own kMeshSectionEntrySize -- same duplication discipline as
        // kSectionEntrySize/kMipEntrySize above.
        constexpr std::uint64_t kMeshSectionEntrySize = 2 + 4 + 4 + 4;
        // Mirrors AssetPipeline::ContentKind's numeric values (ArtifactFormat.hpp) -- kept
        // in lockstep BY HAND, same discipline as ArtifactPixelFormatValue's own mirrored
        // values (ArtifactReader.hpp).
        constexpr std::uint8_t kContentKindTexture = 1;
        constexpr std::uint8_t kContentKindMesh = 2;

        // Bounds-checked little-endian reader over an in-memory buffer, independently
        // reimplemented from ArtifactFormat.cpp's own ByteReader (that class is private to
        // the pipeline's anonymous namespace and is not reachable from here even if we
        // wanted it -- ArcaneClient does not link ArcaneAssetPipeline at all). Every
        // accessor returns false (never throws/UB) on a short read so callers can turn ANY
        // truncation into a single Missing refusal.
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

            // Floats go through their EXACT bit pattern via std::bit_cast, never a numeric
            // conversion -- same discipline as ArtifactFormat.cpp's own F32 accessor (an
            // AABB that round-trips approximately is an AABB that fails the mesh's own
            // framing math on the second load).
            [[nodiscard]] bool F32(float& out) noexcept
            {
                std::uint32_t bits = 0;
                if (!U32(bits)) return false;
                out = std::bit_cast<float>(bits);
                return true;
            }

            // Copies `count` bytes starting at ABSOLUTE offset `at` against the whole
            // backing buffer (not the sequential cursor) -- section bodies are addressed by
            // the section table's own offset/size, not read in sequence.
            [[nodiscard]] bool Slice(std::uint64_t at, std::uint64_t count, std::vector<std::byte>& out) const
            {
                if (at > m_size || count > m_size - at) return false;
                const auto* begin = m_data + static_cast<std::size_t>(at);
                out.assign(begin, begin + static_cast<std::size_t>(count));
                return true;
            }

            [[nodiscard]] std::size_t Pos() const noexcept { return m_pos; }

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

        // Fixed header, parsed out of a whole-file buffer. Mirrors
        // AssetPipeline::TextureArtifactDesc field-for-field (ArtifactFormat.hpp), minus the
        // pipeline type -- see this file's header banner for why this is a hand-mirrored
        // reimplementation rather than a shared struct.
        struct ParsedHeader
        {
            std::uint8_t  contentKind = 0;
            Guid          sourceGuid;
            std::uint64_t sourceHash = 0;
            std::uint32_t importerVersion = 0;
            std::uint8_t  format = 0;
            std::uint8_t  dimension = 0;
            std::uint32_t arrayOrDepth = 0;
            std::uint32_t width = 0, height = 0, mipCount = 0;
            bool          srgb = false;
            std::uint32_t thumbWidth = 0, thumbHeight = 0;
        };

        // The KIND-AGNOSTIC COMMON PREFIX every artifact kind shares, laid out before
        // any kind-specific field by F2b's own design (ArtifactFormat.hpp's header
        // ordering). Split out at F2c Task 4 because ParseHeader below FAILS CLOSED on
        // a non-Texture contentKind -- correct for a texture READ, fatal for the guid
        // SCAN, which must recognise every kind or a cooked mesh artifact resolves as
        // Missing forever no matter how many times it is cooked.
        struct CommonPrefix
        {
            std::uint8_t  contentKind = 0;
            Guid          sourceGuid;
            std::uint64_t sourceHash = 0;
            std::uint32_t importerVersion = 0;
        };

        [[nodiscard]] bool ParseCommonPrefix(ByteReader& r, CommonPrefix& out) noexcept
        {
            std::uint8_t magic[4]{};
            for (std::uint8_t& b : magic)
                if (!r.U8(b)) return false;
            if (magic[0] != kMagic[0] || magic[1] != kMagic[1] ||
                magic[2] != kMagic[2] || magic[3] != kMagic[3])
                return false;

            std::uint32_t version = 0;
            if (!r.U32(version)) return false;
            if (version != kArtifactVersion) return false;

            if (!r.U8(out.contentKind)) return false;
            // NO contentKind GATE HERE, deliberately: this function's whole purpose is
            // to answer "whose guid is this" for ANY kind. The per-kind readers below
            // apply their own fail-closed check, unchanged.
            if (!r.U64(out.sourceGuid.hi)) return false;
            if (!r.U64(out.sourceGuid.lo)) return false;
            if (!r.U64(out.sourceHash)) return false;
            return r.U32(out.importerVersion);
        }

        // Parses the common prefix, then the TEXTURE-specific tail ONLY -- stops before
        // the section table. Used both by the full texture read (ReadArtifactFile below)
        // and (before the F2c Task 4 split, above) by FindArtifactForGuid's directory
        // scan -- that scan now goes through ParseCommonPrefix directly instead, so this
        // function's fail-closed contentKind check no longer blinds it to other kinds.
        [[nodiscard]] bool ParseHeader(ByteReader& r, ParsedHeader& out) noexcept
        {
            CommonPrefix prefix;
            if (!ParseCommonPrefix(r, prefix)) return false;

            // Minor fix (final-review wave), PRESERVED by the Task 4 split: refuse a
            // non-Texture artifact here, rather than silently parsing (misreading) a
            // future/other artifact kind's bytes as if they were this reader's own
            // Texture shape.
            if (prefix.contentKind != kContentKindTexture) return false;

            out.contentKind     = prefix.contentKind;
            out.sourceGuid      = prefix.sourceGuid;
            out.sourceHash      = prefix.sourceHash;
            out.importerVersion = prefix.importerVersion;

            if (!r.U8(out.format)) return false;
            if (!r.U8(out.dimension)) return false;
            if (!r.U32(out.arrayOrDepth)) return false;
            if (!r.U32(out.width)) return false;
            if (!r.U32(out.height)) return false;
            if (!r.U32(out.mipCount)) return false;
            std::uint8_t srgb = 0;
            if (!r.U8(srgb)) return false;
            out.srgb = (srgb != 0);
            if (!r.U32(out.thumbWidth)) return false;
            if (!r.U32(out.thumbHeight)) return false;
            return true;
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path& path)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) return std::nullopt;

            ifs.seekg(0, std::ios::end);
            const std::streamoff len = ifs.tellg();
            if (len < 0) return std::nullopt;
            ifs.seekg(0, std::ios::beg);

            std::vector<std::byte> raw(static_cast<std::size_t>(len));
            if (!raw.empty())
            {
                ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                if (!ifs) return std::nullopt;
            }
            return raw;
        }

        // REVIEW FIX (post-Task-7): the fixed header ParseHeader actually reads is exactly
        // 64 bytes (magic 4 + version 4 + contentKind 1 + sourceGuid 16 + sourceHash 8 +
        // importerVersion 4 + format 1 + dimension 1 + arrayOrDepth 4 + width 4 + height 4
        // + mipCount 4 + srgb 1 + thumbWidth 4 + thumbHeight 4). 256 is generous headroom
        // over that exact count so a future field ADDED to ParseHeader does not silently
        // start under-reading -- still minuscule next to a real artifact's payload (a
        // single BC7 mip alone is already this size or larger), which is the whole point:
        // ReadCommonPrefixOnly below must never pay for anything past the header.
        constexpr std::size_t kHeaderProbeBytes = 256;

        // Reads at most `maxBytes` from the START of `path` -- NEVER the whole file. A
        // file shorter than `maxBytes` on disk reads however many bytes actually exist;
        // ParseHeader's own bounds-checked accessors then correctly refuse anything
        // truncated inside the header, exactly as they would reading the same short buffer
        // out of a full-file read. `ifs.bad()` (a genuine I/O error) is distinguished from
        // merely hitting EOF before `maxBytes` (which sets failbit/eofbit, not badbit, and
        // is the ordinary "short file" case, not a failure).
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

        // Common-prefix-only parse -- FindArtifactForGuid's workhorse, one call per
        // directory-scan candidate. REVIEW FIX (post-Task-7): this used to call
        // ReadWholeFile -- the ENTIRE artifact, payload and thumbnail included -- and parse
        // only ~64 bytes out of it, exactly the "read everything to use tens of bytes of
        // it" cost ArtifactReader.hpp's own header banner claimed this function did NOT
        // pay. It now reads only ReadFilePrefix's bounded kHeaderProbeBytes, which is what
        // makes the banner's claim true.
        //
        // F2c TASK 4 FIX: used to call ParseHeader (the TEXTURE-shaped parse, which fails
        // closed on any contentKind != Texture) -- that made this scan blind to every mesh
        // artifact, the seam this task exists to close (see ArtifactReader.hpp's own "F2c
        // TASK 4 FIX" banner paragraph). Now calls ParseCommonPrefix directly, which reads
        // only the 37-byte kind-agnostic prefix and answers "whose guid is this" for ANY
        // content kind.
        [[nodiscard]] std::optional<CommonPrefix> ReadCommonPrefixOnly(const std::filesystem::path& path)
        {
            const std::optional<std::vector<std::byte>> raw = ReadFilePrefix(path, kHeaderProbeBytes);
            if (!raw) return std::nullopt;

            ByteReader r(raw->data(), raw->size());
            CommonPrefix prefix;
            if (!ParseCommonPrefix(r, prefix)) return std::nullopt;
            return prefix;
        }

        // Full parse: header + section table + the three known section bodies. Same
        // forward-compat rule as ArtifactFormat.hpp's own reader: a section tag this reader
        // does not recognise is skipped, never an error (a future artifact kind, e.g. F2c's
        // mesh artifacts, can add a section without breaking old readers).
        struct FullyParsed
        {
            ParsedHeader header;
            std::vector<MipView> mips;
            std::vector<std::byte> payload;
            std::vector<std::byte> thumbRgba;
        };

        [[nodiscard]] std::optional<FullyParsed> ReadArtifactFile(const std::filesystem::path& path)
        {
            const std::optional<std::vector<std::byte>> raw = ReadWholeFile(path);
            if (!raw) return std::nullopt;

            ByteReader r(raw->data(), raw->size());
            FullyParsed out;
            if (!ParseHeader(r, out.header)) return std::nullopt;

            std::uint32_t sectionCount = 0;
            if (!r.U32(sectionCount)) return std::nullopt;

            // Same cheap sanity bound as ArtifactFormat.cpp's reader: a section count that
            // could not possibly fit in the remaining file is corrupt, and rejecting it here
            // avoids reserving an attacker-controlled amount of memory before the per-entry
            // bounds checks below get a chance to fail closed.
            if (static_cast<std::uint64_t>(sectionCount) * kSectionEntrySize > raw->size())
                return std::nullopt;

            std::vector<SectionTableEntry> entries;
            entries.reserve(sectionCount);
            for (std::uint32_t i = 0; i < sectionCount; ++i)
            {
                SectionTableEntry e{};
                if (!r.U32(e.tag) || !r.U64(e.offset) || !r.U64(e.size)) return std::nullopt;
                entries.push_back(e);
            }

            ByteReader whole(raw->data(), raw->size());   // Slice() addresses the WHOLE buffer
            for (const SectionTableEntry& e : entries)
            {
                std::vector<std::byte> body;
                if (!whole.Slice(e.offset, e.size, body))
                    return std::nullopt;   // section runs past EOF -- truncated/corrupt file

                switch (e.tag)
                {
                case 1:   // MipTable
                {
                    ByteReader mr(body.data(), body.size());
                    std::uint32_t mipCount = 0;
                    if (!mr.U32(mipCount)) return std::nullopt;

                    // I3 fix (final-review wave): mirrors ArtifactFormat.cpp's own
                    // MipTable bound and rationale -- each mip entry costs at least
                    // kMipEntrySize bytes on disk, so a mip count that could not possibly
                    // fit in THIS section's own body is corrupt. Without this, a crafted
                    // mipCount (e.g. 0xFFFFFFFF) would ask std::vector::reserve for ~64 GB
                    // and THROW std::bad_alloc out of this read instead of refusing it
                    // cleanly (nullopt, which callers turn into a single Missing refusal).
                    if (static_cast<std::uint64_t>(mipCount) * kMipEntrySize > body.size())
                        return std::nullopt;

                    out.mips.clear();
                    out.mips.reserve(mipCount);
                    for (std::uint32_t i = 0; i < mipCount; ++i)
                    {
                        MipView mip{};
                        if (!mr.U64(mip.offset) || !mr.U64(mip.size) || !mr.U32(mip.width) || !mr.U32(mip.height))
                            return std::nullopt;
                        out.mips.push_back(mip);
                    }
                    break;
                }
                case 2:   // Payload
                    out.payload = std::move(body);
                    break;
                case 3:   // Thumbnail
                    out.thumbRgba = std::move(body);
                    break;
                default:
                    // Forward-compat: skip, never fail.
                    break;
                }
            }

            return out;
        }

        // ---- Mesh artifacts (F2c Task 4) --------------------------------------------------
        // Independent reimplementation of the mesh half -- see ArtifactReader.hpp's own
        // header banner (MESH TAIL layout) for the contract this mirrors field-for-field
        // from AssetPipeline::MeshArtifactDesc/MeshArtifactSection, without including that
        // header.

        // Fixed mesh header tail, parsed out of a whole-file buffer. Mirrors
        // AssetPipeline::MeshArtifactDesc field-for-field (ArtifactFormat.hpp), minus the
        // pipeline type -- see this file's header banner for why this is a hand-mirrored
        // reimplementation rather than a shared struct.
        struct ParsedMeshHeader
        {
            std::uint8_t  contentKind = 0;
            Guid          sourceGuid;
            std::uint64_t sourceHash = 0;
            std::uint32_t importerVersion = 0;
            std::uint32_t vertexCount = 0;
            std::uint32_t indexCount = 0;
            std::uint32_t sectionCount = 0;
            std::uint8_t  indexWidth = 0;
            float aabbMin[3]{};
            float aabbMax[3]{};
        };

        // Parses the common prefix, then the MESH-specific tail ONLY -- stops before the
        // section table. Mirrors ParseHeader's own shape (texture), applying the mesh
        // kind's own fail-closed contentKind check and its own indexWidth check (A5: the
        // byte is DECLARED, not inferred -- v1's only legal value is 4).
        [[nodiscard]] bool ParseMeshHeader(ByteReader& r, ParsedMeshHeader& out) noexcept
        {
            CommonPrefix prefix;
            if (!ParseCommonPrefix(r, prefix)) return false;

            // Fail-closed contentKind check -- the mesh reader's own copy of the same rule
            // ParseHeader (texture) carries: refuse rather than misread a non-Mesh
            // artifact's bytes as this reader's own Mesh tail shape.
            if (prefix.contentKind != kContentKindMesh) return false;

            out.contentKind     = prefix.contentKind;
            out.sourceGuid      = prefix.sourceGuid;
            out.sourceHash      = prefix.sourceHash;
            out.importerVersion = prefix.importerVersion;

            if (!r.U32(out.vertexCount)) return false;
            if (!r.U32(out.indexCount)) return false;
            if (!r.U32(out.sectionCount)) return false;
            if (!r.U8(out.indexWidth)) return false;
            // Comparison A5 (ArtifactFormat.hpp): a file claiming an indexWidth other than
            // 4 is refused rather than read as though its indices were u32 anyway -- a
            // future 16-bit path is a reader BRANCH, never a silent reinterpretation.
            if (out.indexWidth != 4) return false;
            for (float& c : out.aabbMin) { if (!r.F32(c)) return false; }
            for (float& c : out.aabbMax) { if (!r.F32(c)) return false; }
            return true;
        }

        // Full mesh parse: header + container section table + the three written section
        // bodies (VertexData/IndexData/SectionTable). Same forward-compat rule as
        // ArtifactFormat.hpp's own reader: a section tag this reader does not recognise --
        // including a tag that belongs to the TEXTURE kind, or Mesh's own reserved-
        // unwritten Tangents(7)/Thumbnail(3) -- is skipped, never an error.
        struct FullyParsedMesh
        {
            ParsedMeshHeader header;
            std::vector<float> vertices;   // 8 floats per vertex: pos, normal, uv
            std::vector<std::uint32_t> indices;
            std::vector<MeshSectionView> sections;
        };

        [[nodiscard]] std::optional<FullyParsedMesh> ReadMeshArtifactFile(const std::filesystem::path& path)
        {
            const std::optional<std::vector<std::byte>> raw = ReadWholeFile(path);
            if (!raw) return std::nullopt;

            ByteReader r(raw->data(), raw->size());
            FullyParsedMesh out;
            if (!ParseMeshHeader(r, out.header)) return std::nullopt;

            std::uint32_t containerSectionCount = 0;
            if (!r.U32(containerSectionCount)) return std::nullopt;

            // Same cheap sanity bound ReadArtifactFile's own section table check applies --
            // this is literally the SAME on-disk structure (the container's top-level
            // {tag,offset,size} index), shared by both kinds.
            if (static_cast<std::uint64_t>(containerSectionCount) * kSectionEntrySize > raw->size())
                return std::nullopt;

            std::vector<SectionTableEntry> entries;
            entries.reserve(containerSectionCount);
            for (std::uint32_t i = 0; i < containerSectionCount; ++i)
            {
                SectionTableEntry e{};
                if (!r.U32(e.tag) || !r.U64(e.offset) || !r.U64(e.size)) return std::nullopt;
                entries.push_back(e);
            }

            constexpr std::uint64_t kVertexStride = 8 * 4;   // 8 floats, 4 bytes each

            ByteReader whole(raw->data(), raw->size());   // Slice() addresses the WHOLE buffer
            for (const SectionTableEntry& e : entries)
            {
                std::vector<std::byte> body;
                if (!whole.Slice(e.offset, e.size, body))
                    return std::nullopt;   // section runs past EOF -- truncated/corrupt file

                switch (e.tag)
                {
                case 4:   // VertexData
                {
                    if (body.size() != static_cast<std::uint64_t>(out.header.vertexCount) * kVertexStride)
                        return std::nullopt;
                    ByteReader vr(body.data(), body.size());
                    out.vertices.clear();
                    out.vertices.reserve(static_cast<std::size_t>(out.header.vertexCount) * 8);
                    for (std::uint32_t i = 0; i < out.header.vertexCount; ++i)
                    {
                        float f[8]{};
                        for (float& v : f) { if (!vr.F32(v)) return std::nullopt; }
                        for (float v : f) out.vertices.push_back(v);
                    }
                    break;
                }
                case 5:   // IndexData
                {
                    if (body.size() != static_cast<std::uint64_t>(out.header.indexCount) * 4)
                        return std::nullopt;
                    ByteReader ir(body.data(), body.size());
                    out.indices.clear();
                    out.indices.reserve(out.header.indexCount);
                    for (std::uint32_t i = 0; i < out.header.indexCount; ++i)
                    {
                        std::uint32_t idx = 0;
                        if (!ir.U32(idx)) return std::nullopt;
                        out.indices.push_back(idx);
                    }
                    break;
                }
                case 6:   // SectionTable -- the MESH's own per-section records, NOT the
                          // container's own section table above.
                {
                    ByteReader mr(body.data(), body.size());
                    std::uint32_t meshSectionCount = 0;
                    if (!mr.U32(meshSectionCount)) return std::nullopt;

                    // Same reserve-bound guard the texture MipTable has (I3): the minimum
                    // on-disk width of one entry is kMeshSectionEntrySize (a zero-length
                    // name), so a count that could not possibly fit in THIS section's own
                    // body is corrupt -- refuse cleanly rather than let a crafted count
                    // throw bad_alloc out of the reserve below.
                    if (static_cast<std::uint64_t>(meshSectionCount) * kMeshSectionEntrySize > body.size())
                        return std::nullopt;

                    out.sections.clear();
                    out.sections.reserve(meshSectionCount);
                    for (std::uint32_t i = 0; i < meshSectionCount; ++i)
                    {
                        std::uint16_t nameLen = 0;
                        if (!mr.U16(nameLen)) return std::nullopt;

                        std::string name;
                        name.reserve(nameLen);   // nameLen is u16-bounded -- safe to reserve directly
                        for (std::uint16_t k = 0; k < nameLen; ++k)
                        {
                            std::uint8_t ch = 0;
                            if (!mr.U8(ch)) return std::nullopt;   // a truncated name IS a bounds failure
                            name.push_back(static_cast<char>(ch));
                        }

                        MeshSectionView section{};
                        section.name = std::move(name);
                        if (!mr.U32(section.indexOffset) || !mr.U32(section.indexCount) || !mr.U32(section.slotIndex))
                            return std::nullopt;
                        out.sections.push_back(std::move(section));
                    }
                    break;
                }
                default:
                    // Forward-compat: skip, never fail -- includes the TEXTURE kind's own
                    // tags (1/2) and Mesh's own reserved-unwritten Tangents(7)/Thumbnail(3).
                    break;
                }
            }

            // One extra validation the texture reader has no analogue for: a section
            // pointing past the index buffer is a corrupt file.
            for (const MeshSectionView& section : out.sections)
            {
                if (static_cast<std::uint64_t>(section.indexOffset) + section.indexCount > out.header.indexCount)
                    return std::nullopt;
                // SLOTINDEX BOUND (final-review fix, 2026-09-11; ArtifactReader.hpp's MESH
                // TAIL banner): slotIndex >= sectionCount is impossible in a well-formed
                // artifact (every slot has >= 1 section) and would size the editor's
                // SlotNamesFromSections mirror by a corrupt value -- refused (Missing),
                // never clamped; the pipeline's ReadMeshArtifact applies the identical
                // rule, agreeing through the written contract rather than shared code.
                if (section.slotIndex >= out.header.sectionCount)
                    return std::nullopt;
            }
            // The header's declared sectionCount must agree with what the SectionTable
            // section actually carried (or, if that section was absent entirely, with
            // zero).
            if (static_cast<std::uint32_t>(out.sections.size()) != out.header.sectionCount)
                return std::nullopt;

            // COUNT-AGREEMENT RULE (fix-round hardening, carried into this mirror per the
            // pipeline's own ReadMeshArtifact banner): the header's declared vertexCount/
            // indexCount must agree with what was actually DECODED -- a file that declares
            // nonzero counts but omits the VertexData and/or IndexData section entirely is
            // refused here rather than returned with nonzero declared counts paired with
            // empty arrays. An absent body disagrees with its declared count maximally, the
            // same class of corruption the per-section size checks above already refuse for
            // a body that is merely the WRONG size.
            if (out.vertices.size() != static_cast<std::size_t>(out.header.vertexCount) * 8)
                return std::nullopt;
            if (out.indices.size() != out.header.indexCount)
                return std::nullopt;

            return out;
        }

        // FNV-1a 64-bit over just the source bytes -- the SAME algorithm and constants
        // TextureImporter.cpp's own HashSourceBytes uses to write an artifact's sourceHash
        // field, duplicated here rather than shared (this file's no-pipeline-code banner).
        // TextureImporter.cpp's own comment notes it is ALREADY a second copy of CookKey.cpp's
        // private Fnv1a64 -- this makes a third, the same "no std::hash for anything that
        // lands on disk, duplicate per translation boundary" discipline the pipeline itself
        // already established twice over.
        [[nodiscard]] std::uint64_t HashSourceBytes(std::span<const std::byte> bytes) noexcept
        {
            std::uint64_t h = 14695981039346656037ULL;
            constexpr std::uint64_t prime = 1099511628211ULL;
            for (std::byte b : bytes)
            {
                h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b));
                h *= prime;
            }
            return h;
        }

        // Review fix (post-Task-11): ONE hex digit's value, or -1 -- mirrors
        // ThirdParty/cgltf/cgltf.h's own file-local `cgltf_unhex` exactly (0-9, A-F, a-f;
        // anything else, INCLUDING the string's own null terminator when probed past the
        // end, is -1). DecodeUriPercentEscapes below is the only caller.
        [[nodiscard]] int UnhexDigit(char ch) noexcept
        {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        }

        // Review fix (post-Task-11): percent-decode a URI, mirroring
        // ThirdParty/cgltf/cgltf.h's own `cgltf_decode_uri` BYTE FOR BYTE (this file's
        // no-shared-code banner -- cgltf is a pipeline-side dependency, never linked here).
        // ReadClientExternalBuffers below is the ONE caller, and it is REQUIRED: a glTF
        // buffer uri is percent-encoded whenever it carries a reserved character (a space
        // -> "%20", non-ASCII -- common in an artist's export), and the pipeline's OWN
        // ReadExternalBuffers (MeshImporter.cpp) runs cgltf_decode_uri on every buffer uri
        // via DecodeUriToPath before opening the file -- skipping the decode here would
        // make this reader look for a file literally named "my%20mesh.bin", which was never
        // written, and a legitimately-cooked mesh would refuse permanently (Missing, or
        // HashMismatch once ANY external buffer read fails and the hash input differs).
        //
        // `%XY` (two valid hex digits) decodes to the one byte X*16+Y and advances 3
        // characters; anything else at a '%' -- end of string, a non-hex digit, only one
        // hex digit before the end -- passes the '%' through VERBATIM and advances 1,
        // exactly cgltf_decode_uri's own "a malformed escape is not an error" posture (its
        // fallthrough `*write++ = *i++;` at the bottom of the loop). The bounds check below
        // (`i + 2 < uri.size()`) is this function's own version of cgltf reading past a
        // short match into the C string's null terminator: UnhexDigit(0) is -1 too, so both
        // implementations reach the identical "malformed, pass through" outcome for a
        // trailing "...%4" or a bare "...%" at the end of the string.
        [[nodiscard]] std::string DecodeUriPercentEscapes(const std::string& uri)
        {
            std::string out;
            out.reserve(uri.size());
            std::size_t i = 0;
            while (i < uri.size())
            {
                if (uri[i] == '%' && i + 2 < uri.size())
                {
                    const int hi = UnhexDigit(uri[i + 1]);
                    const int lo = UnhexDigit(uri[i + 2]);
                    if (hi >= 0 && lo >= 0)
                    {
                        out.push_back(static_cast<char>(hi * 16 + lo));
                        i += 3;
                        continue;
                    }
                }
                out.push_back(uri[i]);
                ++i;
            }
            return out;
        }
    }

    ArtifactReadResult ReadClientArtifact(const std::filesystem::path& path,
                                           std::span<const std::byte> currentSourceBytes,
                                           const Guid& expectedSourceGuid)
    {
        ArtifactReadResult result;

        std::optional<FullyParsed> parsed = ReadArtifactFile(path);
        if (!parsed)
        {
            result.refusal = ArtifactRefusal::Missing;   // absent, unparseable, or corrupt
            return result;
        }

        if (parsed->header.sourceGuid != expectedSourceGuid)
        {
            result.refusal = ArtifactRefusal::Missing;   // a stray/renamed artifact, not ours
            return result;
        }

        // SUBSUMPTION CLAUSE (see ArtifactReader.hpp's header banner): an artifact OLDER
        // than the engine collapses into Missing, not a distinct refusal -- CookSession's
        // cook key already folds in importerVersion, so an artifact from an older importer
        // sits at a DIFFERENT key than what today's importer would produce; the CURRENT key
        // simply has no artifact, and ordinary Missing already covers that. Only NEWER is a
        // named refusal.
        if (parsed->header.importerVersion > kClientTextureImporterVersionMirror)
        {
            result.refusal = ArtifactRefusal::VersionNewerThanEngine;
            return result;
        }
        if (parsed->header.importerVersion < kClientTextureImporterVersionMirror)
        {
            result.refusal = ArtifactRefusal::Missing;
            return result;
        }

        if (parsed->header.sourceHash != HashSourceBytes(currentSourceBytes))
        {
            result.refusal = ArtifactRefusal::HashMismatch;
            return result;
        }

        LoadedClientArtifact art;
        art.info.width    = parsed->header.width;
        art.info.height   = parsed->header.height;
        art.info.mipCount = parsed->header.mipCount;
        art.info.srgb     = parsed->header.srgb;
        art.format        = static_cast<ArtifactPixelFormatValue>(parsed->header.format);
        art.mips          = std::move(parsed->mips);
        art.payload       = std::move(parsed->payload);
        art.thumbRgba     = std::move(parsed->thumbRgba);
        art.thumbWidth    = parsed->header.thumbWidth;
        art.thumbHeight   = parsed->header.thumbHeight;

        result.refusal = ArtifactRefusal::None;
        result.artifact = std::move(art);
        return result;
    }

    MeshArtifactReadResult ReadClientMeshArtifact(const std::filesystem::path& path,
                                                   std::span<const std::byte> currentSourceBytes,
                                                   const Guid& expectedSourceGuid)
    {
        MeshArtifactReadResult result;

        std::optional<FullyParsedMesh> parsed = ReadMeshArtifactFile(path);
        if (!parsed)
        {
            // Absent file, unparseable/corrupt bytes, wrong contentKind (ParseMeshHeader's
            // own fail-closed check), or any contract validation ReadMeshArtifactFile
            // enforces (section-range, slotIndex bound, sectionCount/vertexCount/indexCount
            // agreement, indexWidth != 4) -- every one of these collapses into Missing, the same
            // "cannot be used and is not one of the two named refusals" catch-all
            // ReadClientArtifact uses for the texture kind.
            result.refusal = ArtifactRefusal::Missing;
            return result;
        }

        if (parsed->header.sourceGuid != expectedSourceGuid)
        {
            result.refusal = ArtifactRefusal::Missing;   // a stray/renamed artifact, not ours
            return result;
        }

        // SUBSUMPTION CLAUSE (see ArtifactReader.hpp's header banner): an artifact OLDER
        // than the engine collapses into Missing, not a distinct refusal -- same rule
        // ReadClientArtifact applies for texture artifacts, checked here against the MESH
        // mirror (kClientMeshImporterVersionMirror) instead of the texture one.
        if (parsed->header.importerVersion > kClientMeshImporterVersionMirror)
        {
            result.refusal = ArtifactRefusal::VersionNewerThanEngine;
            return result;
        }
        if (parsed->header.importerVersion < kClientMeshImporterVersionMirror)
        {
            result.refusal = ArtifactRefusal::Missing;
            return result;
        }

        if (parsed->header.sourceHash != HashSourceBytes(currentSourceBytes))
        {
            result.refusal = ArtifactRefusal::HashMismatch;
            return result;
        }

        LoadedClientMesh mesh;
        mesh.vertices = std::move(parsed->vertices);
        mesh.indices  = std::move(parsed->indices);
        mesh.sections = std::move(parsed->sections);
        for (int i = 0; i < 3; ++i)
        {
            mesh.aabbMin[i] = parsed->header.aabbMin[i];
            mesh.aabbMax[i] = parsed->header.aabbMax[i];
        }

        result.refusal = ArtifactRefusal::None;
        result.mesh = std::move(mesh);
        return result;
    }

    std::vector<std::filesystem::path> FindArtifactForGuid(const std::filesystem::path& intermediateDir,
                                                             const Guid& guid)
    {
        std::vector<std::filesystem::path> matches;

        const std::filesystem::path artifactsDir = intermediateDir / "Artifacts";

        std::error_code ec;
        if (!std::filesystem::exists(artifactsDir, ec) || ec)
            return matches;

        std::filesystem::recursive_directory_iterator it(artifactsDir, ec);
        if (ec) return matches;

        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec))
        {
            if (ec) break;

            const std::filesystem::directory_entry& entry = *it;
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc) continue;
            if (entry.path().extension() != ".arcart") continue;

            const std::optional<CommonPrefix> prefix = ReadCommonPrefixOnly(entry.path());
            if (!prefix) continue;   // unreadable/corrupt candidate -- skip, never abort the scan

            // C1(b) fix: collect EVERY guid match rather than returning the first --
            // the caller validates each candidate and picks the first clean one (see
            // ArtifactReader.hpp's own C1(b) paragraph). F2c Task 4: this now matches ANY
            // content kind's sourceGuid, not just Texture's -- see ParseCommonPrefix.
            if (prefix->sourceGuid == guid)
                matches.push_back(entry.path());
        }

        return matches;
    }

    namespace
    {
        // Final-review fix I1 (2026-09-11): locate the JSON DOCUMENT inside `sourceBytes`,
        // whichever glTF CONTAINER it is. A .gltf IS its JSON text (the whole file); a
        // .glb wraps it: 12-byte header (magic "glTF", u32 version == 2, u32 totalLength)
        // followed by chunk 0, which the GLB spec REQUIRES to be the JSON chunk (u32
        // chunkLength, u32 chunkType == 0x4E4F534A "JSON", then chunkLength bytes of
        // JSON, space-padded to a 4-byte boundary -- the padding is whitespace, so the
        // parser tolerates it). Every read below is bounds-checked against the buffer
        // BEFORE it happens; a malformed GLB (short header, wrong version, chunk 0 not
        // JSON, a chunk length past the end) yields an EMPTY span, which the caller
        // treats exactly like unparseable JSON (nullopt -> the hash cannot match ->
        // Missing/HashMismatch, never a silent pass or an out-of-bounds read).
        //
        // WHY THIS EXISTS: the GLB container only requires buffer 0 to be the BIN chunk
        // (and only when a BIN chunk is present at all); buffers[1..] may carry a `uri`
        // like any .gltf's. The pipeline's ReadExternalBuffers (MeshImporter.cpp) hands
        // BOTH containers to cgltf_parse, which sniffs the container and reads
        // `buffers[].uri` identically for either -- so a .glb whose second buffer names
        // an external .bin cooks with hash(.glb ++ .bin). Before this fix the client read
        // "a .glb carries no external buffers" as a rule, hashed the .glb alone, and
        // refused that legal file with HashMismatch permanently. The rule is now "every
        // external buffer, whichever container" on both sides.
        [[nodiscard]] std::span<const char> LocateGltfJsonDocument(std::span<const std::byte> sourceBytes) noexcept
        {
            const auto* text = reinterpret_cast<const char*>(sourceBytes.data());
            const std::size_t size = sourceBytes.size();

            const bool isGlb = size >= 4 && text[0] == 'g' && text[1] == 'l' && text[2] == 'T' && text[3] == 'F';
            if (!isGlb)
                return std::span<const char>(text, size);   // a .gltf: the whole file is the document

            constexpr std::uint32_t kGlbVersion   = 2;
            constexpr std::uint32_t kJsonChunkType = 0x4E4F534A;   // "JSON", little-endian
            constexpr std::size_t   kHeaderBytes   = 12;            // magic + version + totalLength
            constexpr std::size_t   kChunkHeaderBytes = 8;          // chunkLength + chunkType

            ByteReader r(sourceBytes.data(), size);
            std::uint32_t magic = 0, version = 0, totalLength = 0;
            if (!r.U32(magic) || !r.U32(version) || !r.U32(totalLength))
                return {};
            if (version != kGlbVersion)
                return {};
            if (totalLength > size || totalLength < kHeaderBytes + kChunkHeaderBytes)
                return {};   // a declared length past the buffer, or too short to hold chunk 0

            std::uint32_t chunkLength = 0, chunkType = 0;
            if (!r.U32(chunkLength) || !r.U32(chunkType))
                return {};
            if (chunkType != kJsonChunkType)
                return {};   // chunk 0 MUST be JSON (GLB spec) -- anything else is malformed
            const std::size_t jsonStart = kHeaderBytes + kChunkHeaderBytes;
            if (chunkLength > totalLength - jsonStart)
                return {};   // JSON chunk runs past the declared total length
            return std::span<const char>(text + jsonStart, chunkLength);
        }
    }

    // F2c Task 11: see this function's own doc comment (ArtifactReader.hpp) for the full
    // contract and the ReadExternalBuffers (ArcaneAssetPipeline/MeshImporter.cpp) peer this
    // must agree with BY HAND. Core logic is the JSON read: no cgltf, no full glTF parse --
    // just `buffers[].uri`, skipping an absent/embedded uri and a `data:` one, PERCENT-
    // DECODING every other uri (DecodeUriPercentEscapes, mirroring cgltf_decode_uri) before
    // reading the referenced file relative to the source's own directory.
    //
    // Final-review fix I1: CONTAINER-AWARE. The JSON document is located first
    // (LocateGltfJsonDocument above: the whole file for a .gltf, chunk 0's slice for a
    // .glb) and only THAT slice is parsed -- the raw bytes of a .glb are not JSON and
    // used to fail the parse outright, which (combined with the caller's old `.gltf`-only
    // gate) is exactly how a .glb with a `uri` buffer beyond its BIN chunk refused forever.
    std::optional<std::vector<std::vector<std::byte>>> ReadClientExternalBuffers(
        std::span<const std::byte> sourceBytes, const std::filesystem::path& sourcePath)
    {
        const std::span<const char> document = LocateGltfJsonDocument(sourceBytes);
        if (document.empty())
            return std::nullopt;   // a malformed GLB container (or an empty source)
        const auto doc = nlohmann::json::parse(document.data(), document.data() + document.size(),
                                                /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object())
            return std::nullopt;

        std::vector<std::vector<std::byte>> buffers;
        const auto buffersIt = doc.find("buffers");
        if (buffersIt == doc.end() || !buffersIt->is_array())
            return buffers;   // no buffers array at all -- zero external buffers, not a failure

        const std::filesystem::path baseDir = sourcePath.parent_path();
        for (const auto& b : *buffersIt)
        {
            if (!b.is_object())
                continue;
            const auto uriIt = b.find("uri");
            if (uriIt == b.end() || !uriIt->is_string())
                continue;   // embedded (GLB BIN chunk) -- nothing to read
            const std::string uri = uriIt->get<std::string>();
            if (uri.rfind("data:", 0) == 0)
                continue;   // inline data URI -- already resolved wherever it is consumed;
                            // checked against the RAW (still-encoded) uri, matching
                            // MeshImporter.cpp's own order (its StartsWith(uri, "data:")
                            // check runs before cgltf ever decodes anything for this buffer)

            // Review fix (post-Task-11): PERCENT-DECODE before the filesystem join -- a raw
            // uri carrying a reserved character (space -> "%20", ...) is not a valid
            // filename component as-is. See DecodeUriPercentEscapes's own comment for why
            // this step is required to agree with the pipeline's cgltf_decode_uri call.
            std::optional<std::vector<std::byte>> bytes =
                ReadWholeFile(baseDir / DecodeUriPercentEscapes(uri));
            if (!bytes)
                return std::nullopt;   // referenced buffer unreadable
            buffers.push_back(std::move(*bytes));
        }
        return buffers;
    }
}
