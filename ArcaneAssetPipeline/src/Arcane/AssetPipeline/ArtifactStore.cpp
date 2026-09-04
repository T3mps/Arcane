#include "Arcane/AssetPipeline/ArtifactStore.hpp"

#include "Arcane/AssetPipeline/ArtifactFormat.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Arcane::AssetPipeline
{
    namespace
    {
        constexpr std::size_t kHexDigits = 16;   // std::uint64_t as lowercase hex, zero-padded

        // Explicit digit-by-digit formatting -- no <sstream>/<iomanip> locale surprises, and
        // matches the byte-explicit discipline used elsewhere in this library.
        [[nodiscard]] std::string ToHex16(std::uint64_t v)
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

        [[nodiscard]] std::optional<std::uint64_t> FromHex16(std::string_view hex)
        {
            if (hex.size() != kHexDigits) return std::nullopt;

            std::uint64_t v = 0;
            for (char c : hex)
            {
                std::uint64_t digit;
                if (c >= '0' && c <= '9')      digit = static_cast<std::uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f')  digit = static_cast<std::uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')  digit = static_cast<std::uint64_t>(c - 'A' + 10);
                else return std::nullopt;

                v = (v << 4) | digit;
            }
            return v;
        }
    }

    ArtifactStore::ArtifactStore(std::filesystem::path intermediateDir)
        : m_intermediateDir(std::move(intermediateDir))
    {
    }

    std::filesystem::path ArtifactStore::PathFor(std::uint64_t cookKey) const
    {
        const std::string hex = ToHex16(cookKey);
        const std::string hh = hex.substr(0, 2);
        return m_intermediateDir / "Artifacts" / hh / (hex + ".arcart");
    }

    bool ArtifactStore::Commit(std::uint64_t cookKey,
                                const std::function<bool(const std::filesystem::path&)>& writer)
    {
        const std::filesystem::path finalPath = PathFor(cookKey);

        std::error_code ec;
        std::filesystem::create_directories(finalPath.parent_path(), ec);

        std::filesystem::path tmpPath = finalPath;
        tmpPath += ".tmp";

        // A throwing writer must still leave no file at the final name and no stray .tmp;
        // Commit's contract is a plain bool, so the exception is caught here rather than
        // propagated -- callers of Commit never need to guard against an arbitrary writer's
        // exception type.
        bool wrote = false;
        try
        {
            wrote = writer(tmpPath);
        }
        catch (...)
        {
            wrote = false;
        }

        if (!wrote)
        {
            std::filesystem::remove(tmpPath, ec);
            return false;
        }

        std::filesystem::rename(tmpPath, finalPath, ec);
        if (ec)
        {
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
        return true;
    }

    std::optional<std::uint64_t> ArtifactStore::Lookup(const Guid& guid) const
    {
        const auto it = m_index.find(guid);
        if (it == m_index.end()) return std::nullopt;
        return it->second;
    }

    void ArtifactStore::PutIndex(const Guid& guid, std::uint64_t cookKey)
    {
        m_index[guid] = cookKey;
    }

    void ArtifactStore::RebuildIndexFromScan()
    {
        m_index.clear();

        const std::filesystem::path artifactsDir = m_intermediateDir / "Artifacts";

        std::error_code ec;
        if (!std::filesystem::exists(artifactsDir, ec) || ec)
            return;

        std::filesystem::recursive_directory_iterator it(artifactsDir, ec);
        if (ec) return;

        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec))
        {
            if (ec) break;

            const std::filesystem::directory_entry& entry = *it;

            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc) continue;

            const std::filesystem::path& path = entry.path();
            if (path.extension() != ".arcart") continue;

            const std::optional<std::uint64_t> cookKey = FromHex16(path.stem().string());
            if (!cookKey) continue;   // not a name this store minted -- skip, never abort the scan

            const std::optional<LoadedArtifact> loaded = ReadTextureArtifact(path);
            if (!loaded) continue;   // corrupt/unreadable -- skip, never abort the scan

            m_index[loaded->desc.sourceGuid] = *cookKey;
        }
    }

    std::size_t ArtifactStore::SweepOrphans(const std::unordered_set<Guid>& liveGuids)
    {
        std::size_t removed = 0;

        for (auto it = m_index.begin(); it != m_index.end(); )
        {
            if (liveGuids.contains(it->first))
            {
                ++it;
                continue;
            }

            std::error_code ec;
            if (std::filesystem::remove(PathFor(it->second), ec))
                ++removed;

            it = m_index.erase(it);
        }

        return removed;
    }
}
