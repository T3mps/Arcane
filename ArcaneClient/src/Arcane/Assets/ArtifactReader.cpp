#include <Arcane/Assets/ArtifactReader.hpp>

#include <array>
#include <fstream>
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
        // Mirrors AssetPipeline::ContentKind::Texture's numeric value (ArtifactFormat.hpp)
        // -- kept in lockstep BY HAND, same discipline as ArtifactPixelFormatValue's own
        // mirrored values (ArtifactReader.hpp).
        constexpr std::uint8_t kContentKindTexture = 1;

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

        // Parses magic + artifactVersion + the fixed header fields ONLY -- stops before the
        // section table. Used both by the full read (ReadArtifactFile below) and by
        // FindArtifactForGuid's directory scan, which never needs a candidate's sections
        // and would rather not pay to read/copy them for every file it rejects.
        [[nodiscard]] bool ParseHeader(ByteReader& r, ParsedHeader& out) noexcept
        {
            std::uint8_t magic[4]{};
            for (std::uint8_t& b : magic)
                if (!r.U8(b)) return false;
            if (magic[0] != kMagic[0] || magic[1] != kMagic[1] || magic[2] != kMagic[2] || magic[3] != kMagic[3])
                return false;

            std::uint32_t version = 0;
            if (!r.U32(version)) return false;
            if (version != kArtifactVersion) return false;

            if (!r.U8(out.contentKind)) return false;
            // Minor fix (final-review wave): mirrors ArtifactFormat.cpp's own fail-closed
            // contentKind check -- refuse a non-Texture artifact here too, rather than
            // silently parsing (misreading) a future artifact kind's bytes as if they were
            // this reader's own Texture shape.
            if (out.contentKind != kContentKindTexture) return false;
            if (!r.U64(out.sourceGuid.hi)) return false;
            if (!r.U64(out.sourceGuid.lo)) return false;
            if (!r.U64(out.sourceHash)) return false;
            if (!r.U32(out.importerVersion)) return false;
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
        // ReadHeaderOnly below must never pay for anything past the header.
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

        // Header-only parse -- FindArtifactForGuid's workhorse, one call per directory-scan
        // candidate. REVIEW FIX (post-Task-7): this used to call ReadWholeFile -- the
        // ENTIRE artifact, payload and thumbnail included -- and parse only ~64 bytes out
        // of it, exactly the "read everything to use tens of bytes of it" cost
        // ArtifactReader.hpp's own header banner claimed this function did NOT pay. It now
        // reads only ReadFilePrefix's bounded kHeaderProbeBytes, which is what makes the
        // banner's claim true.
        [[nodiscard]] std::optional<ParsedHeader> ReadHeaderOnly(const std::filesystem::path& path)
        {
            const std::optional<std::vector<std::byte>> raw = ReadFilePrefix(path, kHeaderProbeBytes);
            if (!raw) return std::nullopt;

            ByteReader r(raw->data(), raw->size());
            ParsedHeader h;
            if (!ParseHeader(r, h)) return std::nullopt;
            return h;
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

            const std::optional<ParsedHeader> header = ReadHeaderOnly(entry.path());
            if (!header) continue;   // unreadable/corrupt candidate -- skip, never abort the scan

            // C1(b) fix: collect EVERY guid match rather than returning the first --
            // the caller validates each candidate and picks the first clean one (see
            // ArtifactReader.hpp's own C1(b) paragraph).
            if (header->sourceGuid == guid)
                matches.push_back(entry.path());
        }

        return matches;
    }
}
