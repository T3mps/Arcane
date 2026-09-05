#include "Arcane/AssetPipeline/CookSession.hpp"

#include "Arcane/AssetPipeline/ArtifactFormat.hpp"
#include "Arcane/AssetPipeline/ArtifactStore.hpp"
#include "Arcane/AssetPipeline/CookKey.hpp"

#include <Json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

// Only for CurrentProcessId() below -- the failure-memo writer's per-invocation tmp path,
// same reasoning/mechanism as ArtifactStore.cpp's own (duplicated rather than shared: that
// helper lives in ArtifactStore.cpp's anonymous namespace, not exported).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace Arcane::AssetPipeline
{
    namespace fs = std::filesystem;

    namespace
    {
        // ---- Source enumeration --------------------------------------------------------

        std::string LowerExt(const fs::path& p)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        // Every ".png" under `contentDir` (recursive) that has a sibling "<file>.meta"
        // sidecar -- see CookSession.hpp's file header for the "why" (Unity convention,
        // never minted here). Sorted for deterministic iteration order (no dependence on
        // filesystem enumeration order across runs/platforms).
        std::vector<fs::path> EnumerateTextureSources(const fs::path& contentDir)
        {
            std::vector<fs::path> sources;

            std::error_code ec;
            if (!fs::exists(contentDir, ec) || ec) return sources;

            fs::recursive_directory_iterator it(contentDir, ec);
            if (ec) return sources;
            const fs::recursive_directory_iterator end;
            for (; it != end; it.increment(ec))
            {
                if (ec) break;

                const fs::directory_entry& entry = *it;
                std::error_code fileEc;
                if (!entry.is_regular_file(fileEc) || fileEc) continue;
                if (LowerExt(entry.path()) != ".png") continue;

                fs::path meta = entry.path();
                meta += ".meta";
                std::error_code metaEc;
                if (!fs::exists(meta, metaEc) || metaEc) continue;   // not a registered source -- skip

                sources.push_back(entry.path());
            }

            std::sort(sources.begin(), sources.end());
            return sources;
        }

        // ---- .meta reading (guid + Task 3's "texture" settings block) -------------------

        struct SourceMeta
        {
            Guid guid;
            TextureMetaSettings settings;
        };

        // Tolerant read: absent/malformed guid or texture block never throws -- an invalid
        // sidecar just means "not a cookable source yet" (nullopt), same tolerant contract
        // AssetRegistry.cpp's own ReadGuidField/FromMetaJson both already keep.
        std::optional<SourceMeta> ReadSourceMeta(const fs::path& metaPath)
        {
            std::ifstream in(metaPath, std::ios::binary);
            if (!in) return std::nullopt;

            const nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
            in.close();
            if (!doc.is_object()) return std::nullopt;

            const auto guidIt = doc.find("guid");
            if (guidIt == doc.end() || !guidIt->is_string()) return std::nullopt;
            const std::optional<Guid> guid = Guid::FromString(guidIt->get<std::string>());
            if (!guid || !guid->IsValid()) return std::nullopt;

            nlohmann::json textureBlock = nlohmann::json::object();
            if (const auto texIt = doc.find("texture"); texIt != doc.end() && texIt->is_object())
                textureBlock = *texIt;

            return SourceMeta{ *guid, TextureMetaSettings::FromMetaJson(textureBlock) };
        }

        std::optional<std::vector<std::byte>> ReadWholeFileBytes(const fs::path& path)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) return std::nullopt;

            ifs.seekg(0, std::ios::end);
            const std::streamoff len = ifs.tellg();
            if (len < 0) return std::nullopt;
            ifs.seekg(0, std::ios::beg);

            std::vector<std::byte> out(static_cast<std::size_t>(len));
            if (!out.empty())
            {
                ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
                if (!ifs) return std::nullopt;
            }
            return out;
        }

        // ---- Failure memo: <intermediateDir>/Failed/<hex16>.fail -------------------------
        // One flat file per failing cook key -- no 256-way sharding like ArtifactStore's
        // artifacts (failures are rare relative to successful artifacts, so the directory
        // never grows large enough to need it). The file's whole content is the human-
        // readable reason string; existence alone is the memo signal.

        constexpr std::size_t kHexDigits = 16;

        std::string ToHex16(std::uint64_t v)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string s(kHexDigits, '0');
            for (std::size_t i = 0; i < kHexDigits; ++i)
            {
                const int shift = 4 * static_cast<int>(kHexDigits - 1 - i);
                s[i] = kDigits[(v >> shift) & 0xFu];
            }
            return s;
        }

        [[nodiscard]] std::uint32_t CurrentProcessId() noexcept
        {
#if defined(_WIN32)
            return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
            return static_cast<std::uint32_t>(::getpid());
#endif
        }

        [[nodiscard]] std::uint64_t NextInvocationId() noexcept
        {
            static std::atomic<std::uint64_t> counter{ 0 };
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        fs::path FailedDir(const fs::path& intermediateDir) { return intermediateDir / "Failed"; }

        fs::path FailedPathFor(const fs::path& intermediateDir, std::uint64_t cookKey)
        {
            return FailedDir(intermediateDir) / (ToHex16(cookKey) + ".fail");
        }

        std::optional<std::string> ReadFailureMemo(const fs::path& intermediateDir, std::uint64_t cookKey)
        {
            std::ifstream in(FailedPathFor(intermediateDir, cookKey), std::ios::binary);
            if (!in) return std::nullopt;
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        // Best-effort: a failure memo is a diagnostic convenience, not correctness-critical
        // (worst case on a write failure is one extra importer run next session). Written
        // via a per-invocation tmp + rename, same mechanism as ArtifactStore::Commit, so two
        // stagers racing to memoize the SAME failing key (both computing the SAME reason
        // string, since the reason is a pure function of the same source bytes + settings)
        // never interleave/corrupt each other's bytes.
        void WriteFailureMemo(const fs::path& intermediateDir, std::uint64_t cookKey, const std::string& reason)
        {
            const fs::path dir = FailedDir(intermediateDir);
            std::error_code ec;
            fs::create_directories(dir, ec);

            const fs::path finalPath = FailedPathFor(intermediateDir, cookKey);
            const fs::path tmpPath = dir
                / (ToHex16(cookKey) + "." + std::to_string(CurrentProcessId()) + "-"
                   + std::to_string(NextInvocationId()) + ".tmp");

            {
                std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
                if (!out) return;
                out << reason;
                if (!out.good()) { out.close(); fs::remove(tmpPath, ec); return; }
            }

            fs::rename(tmpPath, finalPath, ec);
            if (ec) fs::remove(tmpPath, ec);
        }
    }

    void CookSession::SetProgress(ProgressFn fn) { m_progress = std::move(fn); }
    void CookSession::SetImporterForTesting(ImporterFn fn) { m_importer = std::move(fn); }

    const std::unordered_map<Guid, std::string>& CookSession::LastFailures() const { return m_lastFailures; }

    CookResult CookSession::CookProject(const fs::path& projectDir)
    {
        CookResult result;
        m_lastFailures.clear();

        const fs::path contentDir = projectDir / "Content";
        const fs::path intermediateDir = projectDir / "Intermediate";
        ArtifactStore store(intermediateDir);
        store.RebuildIndexFromScan();   // warms the index for future guid->key consumers (e.g. --dump-dds)

        for (const fs::path& source : EnumerateTextureSources(contentDir))
        {
            fs::path metaPath = source;
            metaPath += ".meta";

            const std::optional<SourceMeta> meta = ReadSourceMeta(metaPath);
            if (!meta) continue;   // no valid sidecar -- not a cookable source

            const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(source);
            if (!bytes) continue;   // unreadable file -- an I/O condition, not an import failure

            const std::uint64_t cookKey = ComputeCookKey(*bytes, meta->settings, kTextureImporterVersion);
            const fs::path finalPath = store.PathFor(cookKey);

            std::error_code existsEc;
            if (fs::exists(finalPath, existsEc))
            {
                store.PutIndex(meta->guid, cookKey);
                ++result.upToDate;
                if (m_progress) m_progress(source, true, "up to date");
                continue;
            }

            if (const std::optional<std::string> memo = ReadFailureMemo(intermediateDir, cookKey))
            {
                m_lastFailures[meta->guid] = *memo;
                result.failures.emplace_back(meta->guid, *memo);
                ++result.failed;
                if (m_progress) m_progress(source, false, *memo);
                continue;   // memoized -- the importer NEVER runs again for this exact key
            }

            const std::optional<ImportedTexture> imported = m_importer
                ? m_importer(*bytes, meta->guid, meta->settings)
                : ImportTexture(*bytes, meta->guid, meta->settings);

            if (!imported)
            {
                const std::string reason = "texture import failed: '" + source.filename().string()
                    + "' did not decode as a supported image (corrupt or unrecognised source bytes)";
                WriteFailureMemo(intermediateDir, cookKey, reason);
                m_lastFailures[meta->guid] = reason;
                result.failures.emplace_back(meta->guid, reason);
                ++result.failed;
                if (m_progress) m_progress(source, false, reason);
                continue;
            }

            const bool committed = store.Commit(cookKey, [&](const fs::path& tmp)
            {
                return WriteTextureArtifact(tmp, imported->desc, imported->payload, imported->thumbRgba);
            });

            if (!committed)
            {
                // Deliberately NOT memoized -- see CookSession.hpp's file header: a commit
                // failure is a rare IO condition independent of the source's own bytes, and
                // retrying it next call costs one file write, not a decode+encode.
                const std::string reason = "artifact commit failed for '" + source.filename().string()
                    + "' (disk write error)";
                m_lastFailures[meta->guid] = reason;
                result.failures.emplace_back(meta->guid, reason);
                ++result.failed;
                if (m_progress) m_progress(source, false, reason);
                continue;
            }

            store.PutIndex(meta->guid, cookKey);
            result.cookedGuids.push_back(meta->guid);
            ++result.cooked;
            if (m_progress) m_progress(source, true, "cooked");
        }

        return result;
    }

    bool CookSession::CheckProject(const fs::path& projectDir) const
    {
        const fs::path contentDir = projectDir / "Content";
        const fs::path intermediateDir = projectDir / "Intermediate";
        const ArtifactStore store(intermediateDir);

        for (const fs::path& source : EnumerateTextureSources(contentDir))
        {
            fs::path metaPath = source;
            metaPath += ".meta";

            const std::optional<SourceMeta> meta = ReadSourceMeta(metaPath);
            if (!meta) continue;

            const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(source);
            if (!bytes) continue;

            const std::uint64_t cookKey = ComputeCookKey(*bytes, meta->settings, kTextureImporterVersion);

            std::error_code ec;
            if (!fs::exists(store.PathFor(cookKey), ec))
                return true;   // at least one stale source -- short-circuit
        }

        return false;
    }

    std::optional<fs::path> CookSession::ResolveCurrentArtifactPath(const fs::path& projectDir,
                                                                      const Guid& guid) const
    {
        const fs::path contentDir = projectDir / "Content";
        const fs::path intermediateDir = projectDir / "Intermediate";
        const ArtifactStore store(intermediateDir);

        // Deliberately NOT store.RebuildIndexFromScan()/Lookup() -- see the header
        // comment: that index resolves a Guid shared by an orphaned artifact (an old
        // cook key still on disk after a `.meta` settings edit) and the current one
        // non-deterministically. Scanning sources and recomputing today's key directly
        // sidesteps the ambiguity entirely, same discipline CookProject/CheckProject
        // already use for staleness.
        for (const fs::path& source : EnumerateTextureSources(contentDir))
        {
            fs::path metaPath = source;
            metaPath += ".meta";

            const std::optional<SourceMeta> meta = ReadSourceMeta(metaPath);
            if (!meta || meta->guid != guid) continue;   // wrong/unreadable source -- keep scanning

            const std::optional<std::vector<std::byte>> bytes = ReadWholeFileBytes(source);
            if (!bytes) return std::nullopt;   // the one matching source is unreadable

            const std::uint64_t cookKey = ComputeCookKey(*bytes, meta->settings, kTextureImporterVersion);
            const fs::path artifactPath = store.PathFor(cookKey);

            std::error_code ec;
            if (!fs::exists(artifactPath, ec))
                return std::nullopt;   // uncooked/stale -- NEVER fall back to another key

            return artifactPath;
        }

        return std::nullopt;   // no source under Content/ carries this guid
    }
}
