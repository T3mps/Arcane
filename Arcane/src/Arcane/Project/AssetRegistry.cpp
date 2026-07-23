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

    std::size_t AssetRegistry::ScanContent(const std::filesystem::path& contentDir, std::string_view scheme)
    {
        m_byGuid.clear();

        std::error_code ec;
        if (!std::filesystem::is_directory(contentDir, ec))
            return 0;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
        {
            if (!entry.is_regular_file())
                continue;

            const std::string ext = LowerExt(entry.path());

            // A .meta is a sidecar carrying another file's identity, not an asset itself.
            if (ext == ".meta")
                continue;

            // Route by asset kind: native JSON embeds its id; imported binaries use a
            // sidecar; anything else is not an asset we track.
            Guid id;
            if (ext == ".json")
                id = ResolveNativeId(entry.path());
            else if (IsImportedBinary(ext))
                id = ResolveSidecarId(entry.path());
            else
                continue;

            if (!id.IsValid())
                continue;   // read/parse failure -- already warned

            // Mount path: "<scheme>://<relative-to-contentDir, forward slashes>". The
            // ORIGINAL file is registered (the .meta only stores the id), so a resolved
            // Guid still points at the real asset the loader reads.
            const auto rel = std::filesystem::relative(entry.path(), contentDir, ec);
            std::string mountPath = std::string(scheme) + "://" + rel.generic_string();

            if (auto [_, inserted] = m_byGuid.try_emplace(id, std::move(mountPath)); !inserted)
                ARC_WARN("AssetRegistry: duplicate id {} (also '{}') -- keeping first",
                         id.ToString(), entry.path().generic_string());
        }

        return m_byGuid.size();
    }

    std::optional<std::string> AssetRegistry::Resolve(const Guid& id) const
    {
        auto it = m_byGuid.find(id);
        if (it == m_byGuid.end())
            return std::nullopt;
        return it->second;
    }
}
