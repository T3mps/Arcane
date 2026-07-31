#include <Arcane/Project/AssetRegistry.hpp>

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace Arcane
{
    namespace
    {
        // Lowercased extension of a path ("HERO.PNG" -> ".png"), so extension matching is
        // case-insensitive on case-preserving filesystems (Windows/macOS).
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        // Imported external originals that cannot embed an id -- their Guid lives in a
        // sibling "<file>.meta" (Unity's model, scoped to imported source assets only).
        bool IsImportedBinary(std::string_view extLower)
        {
            static constexpr std::string_view kBinaryExts[] = {
                ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr",  // images
                ".wav", ".ogg", ".mp3", ".flac",                  // audio
                ".ttf", ".otf",                                   // fonts
            };
            for (std::string_view e : kBinaryExts)
                if (extLower == e)
                    return true;
            return false;
        }

        // Read a canonical Guid out of a parsed object's string field; nullopt if the field
        // is absent, non-string, or not a valid 8-4-4-4-12 guid.
        std::optional<Guid> ReadGuidField(const nlohmann::json& doc, const char* key)
        {
            if (auto it = doc.find(key); it != doc.end() && it->is_string())
                if (auto parsed = Guid::FromString(it->get<std::string>()); parsed && parsed->IsValid())
                    return *parsed;
            return std::nullopt;
        }

        // Native asset: id embedded as a top-level "id". A missing/invalid id is minted and
        // written back into the asset (auto-import), so it is born with a durable identity.
        Guid ResolveNativeId(const std::filesystem::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) { ARC_WARN("AssetRegistry: cannot read '{}'", file.generic_string()); return {}; }
            auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
            in.close();
            if (!doc.is_object())
            {
                ARC_WARN("AssetRegistry: '{}' is not a JSON object -- skipped", file.generic_string());
                return {};
            }

            if (auto id = ReadGuidField(doc, "id"))
                return *id;

            Guid id = Guid::Generate();
            doc["id"] = id.ToString();
            std::ofstream out(file, std::ios::binary);
            if (out) out << doc.dump(2) << '\n';
            else     ARC_WARN("AssetRegistry: cannot write id back to '{}'", file.generic_string());
            return id;
        }

        // Imported binary original: id lives in a sibling "<file>.meta". The ".meta" is
        // APPENDED to the full filename (Unity convention: hero.png -> hero.png.meta) rather
        // than replacing the extension, so "a.png" and "a.wav" get distinct sidecars. A
        // missing/invalid sidecar is minted and written beside the original (auto-import).
        Guid ResolveSidecarId(const std::filesystem::path& file)
        {
            std::filesystem::path meta = file;
            meta += ".meta";

            std::error_code ec;
            if (std::filesystem::exists(meta, ec))
            {
                std::ifstream in(meta, std::ios::binary);
                if (in)
                {
                    auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
                    in.close();
                    if (doc.is_object())
                        if (auto id = ReadGuidField(doc, "guid"))
                            return *id;
                }
                ARC_WARN("AssetRegistry: sidecar '{}' has no valid guid -- regenerating",
                         meta.generic_string());
            }

            Guid id = Guid::Generate();
            nlohmann::json doc;
            doc["guid"]    = id.ToString();
            doc["version"] = 1;                 // sidecar format version (import settings land here later)
            std::ofstream out(meta, std::ios::binary);
            if (out) out << doc.dump(2) << '\n';
            else     ARC_WARN("AssetRegistry: cannot write sidecar '{}'", meta.generic_string());
            return id;
        }
    }

    std::size_t AssetRegistry::ScanContent(const std::filesystem::path& contentDir, std::string_view scheme,
                                           ScanProgressFn onProgress)
    {
        m_byGuid.clear();

        // No observer: identical to this function's pre-callback shape (and
        // cost) -- a single walk via AddContent, no counting pass.
        if (!onProgress)
            return AddContent(contentDir, scheme);

        std::error_code ec;
        if (!std::filesystem::is_directory(contentDir, ec))
            return 0;

        // Denominator: a cheap stat-only pass (is_regular_file only -- no file
        // reads) over the SAME population AddFile below will be offered, so
        // `done` reaching `total` on the real pass is guaranteed rather than
        // hoped for. This is the one added cost this parameter brings, and it
        // is only paid when a caller actually wants progress (see above).
        std::size_t total = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
            if (entry.is_regular_file())
                ++total;

        const std::size_t before = m_byGuid.size();
        std::size_t done = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            // Per-file rules (kind routing, id resolution, mount-path shape) live in
            // AddFile so this progress-reporting walk and AddContent's plain one can
            // never drift apart on what counts as a trackable asset.
            AddFile(entry.path(), contentDir, scheme);
            ++done;
            onProgress(done, total);
        }

        return m_byGuid.size() - before;
    }

    std::size_t AssetRegistry::AddContent(const std::filesystem::path& contentDir, std::string_view scheme)
    {
        const std::size_t before = m_byGuid.size();

        std::error_code ec;
        if (!std::filesystem::is_directory(contentDir, ec))
            return 0;

        // Per-file rules (kind routing, id resolution, mount-path shape) live in
        // AddFile so the open-time scan and the editor's incremental registration
        // can never drift apart.
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            AddFile(entry.path(), contentDir, scheme);
        }

        return m_byGuid.size() - before;
    }

    std::optional<Guid> AssetRegistry::AddFile(const std::filesystem::path& file,
                                               const std::filesystem::path& contentDir,
                                               std::string_view scheme)
    {
        const std::string ext = LowerExt(file);

        // A .meta is a sidecar carrying another file's identity, not an asset itself.
        if (ext == ".meta")
            return std::nullopt;

        // Route by asset kind: native JSON embeds its id, imported binaries use
        // a sidecar, anything else is not an asset we track.
        //
        // .arcscene is on the native list because a scene has to be
        // referenceable by Guid rather than by path: ProjectManifest::bootScene
        // stores one, so a scene that moves on disk must keep opening, and the
        // asset browser can only list what this registry knows about.
        //
        // .arcsprite is native for the same reason .arcmat is: SaveSpriteAsset
        // embeds a top-level "id" (SpriteAsset.cpp:14), so it rides the same
        // ResolveNativeId path rather than a sidecar.
        Guid id;
        if (ext == ".json" || ext == ".arcmat" || ext == ".arcscene" || ext == ".arcsprite")
            id = ResolveNativeId(file);
        else if (IsImportedBinary(ext))
            id = ResolveSidecarId(file);
        else
            return std::nullopt;

        if (!id.IsValid())
            return std::nullopt;   // read/parse failure -- already warned

        // Mount path: "<scheme>://<relative-to-contentDir, forward slashes>". The
        // ORIGINAL file is registered (the .meta only stores the id), so a resolved
        // Guid still points at the real asset the loader reads. A file outside
        // contentDir has no expressible mount path -- refuse it.
        std::error_code ec;
        const auto rel = std::filesystem::relative(file, contentDir, ec);
        if (ec || rel.empty() || rel.is_absolute() || *rel.begin() == "..")
        {
            ARC_WARN("AssetRegistry: '{}' is not under content root '{}' -- not registered",
                     file.generic_string(), contentDir.generic_string());
            return std::nullopt;
        }
        const std::string mountPath = std::string(scheme) + "://" + rel.generic_string();

        if (auto [it, inserted] = m_byGuid.try_emplace(id, mountPath);
            !inserted && it->second != mountPath)
        {
            // Same id from a DIFFERENT file: scan rule -- keep the first, warn.
            // (Same id + same mapping = idempotent re-registration, silent.)
            ARC_WARN("AssetRegistry: duplicate id {} (also '{}') -- keeping first",
                     id.ToString(), file.generic_string());
        }
        return id;
    }

    std::optional<std::string> AssetRegistry::Resolve(const Guid& id) const
    {
        auto it = m_byGuid.find(id);
        if (it == m_byGuid.end())
            return std::nullopt;
        return it->second;
    }

    std::vector<std::pair<Guid, std::string>> AssetRegistry::All() const
    {
        std::vector<std::pair<Guid, std::string>> out;
        out.reserve(m_byGuid.size());
        for (const auto& [id, mountPath] : m_byGuid)
            out.emplace_back(id, mountPath);
        return out;
    }
}
