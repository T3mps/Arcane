#pragma once

// Asset Browser (shader-editor Slice 6, spec Fold 2): the REAL browser over the
// AssetRegistry -- every registered asset, kind-classified from its mount-path
// extension, with filter + search, type icons, double-click -> editor routing,
// and drag-source rows (texture params accept the drop). The pure parts
// (classification, entry building, filtering) live HERE so the [editor] units
// drive them headless; DrawAssetBrowserPanel in the .cpp is the only ImGui.

#include <Arcane/Guid.hpp>
#include <Arcane/Project/AssetRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class DocumentHost;

    enum class AssetKind : int
    {
        Material = 0,
        Texture,
        Audio,
        Font,
        Data,
        Scene,
        Sprite,
        Other,
    };
    inline constexpr int kAssetKindCount = 8;

    // The ImGui drag-drop payload type for browser rows (the params panel's
    // texture slots accept it). Payload bytes = AssetDragPayload (POD).
    inline constexpr const char* kAssetDragType = "ARCANE_ASSET";
    struct AssetDragPayload
    {
        Arcane::Guid guid;
        AssetKind kind;
    };

    // Classify by extension (matches the registry's own scan rules: .arcmat and
    // .json are native; the binary list mirrors AssetRegistry's IsImportedBinary).
    inline AssetKind AssetKindOf(std::string_view mountPath)
    {
        const std::size_t dot = mountPath.rfind('.');
        if (dot == std::string_view::npos)
            return AssetKind::Other;
        std::string ext(mountPath.substr(dot));
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".arcmat")
            return AssetKind::Material;
        if (ext == ".arcscene")
            return AssetKind::Scene;
        if (ext == ".arcsprite")
            return AssetKind::Sprite;
        for (const char* e : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" })
            if (ext == e) return AssetKind::Texture;
        for (const char* e : { ".wav", ".ogg", ".mp3", ".flac" })
            if (ext == e) return AssetKind::Audio;
        if (ext == ".ttf" || ext == ".otf")
            return AssetKind::Font;
        if (ext == ".json")
            return AssetKind::Data;
        return AssetKind::Other;
    }

    struct AssetEntry
    {
        Arcane::Guid guid;
        std::string  mountPath;   // "game://materials/glow.arcmat"
        std::string  name;        // "glow" (stem)
        AssetKind    kind = AssetKind::Other;
    };

    // Registry snapshot -> classified entries, sorted by name (path breaks ties)
    // for a stable listing.
    inline std::vector<AssetEntry> BuildAssetEntries(const Arcane::AssetRegistry& registry)
    {
        std::vector<AssetEntry> entries;
        for (auto& [guid, mountPath] : registry.All())
        {
            AssetEntry e;
            e.guid = guid;
            e.kind = AssetKindOf(mountPath);
            const std::size_t slash = mountPath.rfind('/');
            std::string_view file = slash == std::string::npos
                                        ? std::string_view(mountPath)
                                        : std::string_view(mountPath).substr(slash + 1);
            const std::size_t dot = file.rfind('.');
            e.name = std::string(dot == std::string_view::npos ? file : file.substr(0, dot));
            e.mountPath = std::move(mountPath);
            entries.push_back(std::move(e));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const AssetEntry& a, const AssetEntry& b)
                  { return a.name != b.name ? a.name < b.name : a.mountPath < b.mountPath; });
        return entries;
    }

    // Inspector asset-ref (Guid) fields: infer the expected asset kind from the
    // FIELD NAME -- reflection carries no per-field attributes yet, so this
    // heuristic is the seam until it does. Case-insensitive substring match:
    // "material" -> Material, "texture" -> Texture, "sprite" -> Sprite;
    // anything else -> -1 (all kinds, same convention as MatchesFilter's
    // kindFilter). Sprite is checked AFTER material/texture ON PURPOSE: a
    // field named e.g. "spriteMaterial" contains both substrings and must
    // still resolve Material (the material IS what such a field means), so
    // the material/texture branches have to win the race.
    inline int AssetKindFilterForFieldName(std::string_view fieldName)
    {
        std::string lower(fieldName);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("material") != std::string::npos)
            return static_cast<int>(AssetKind::Material);
        if (lower.find("texture") != std::string::npos)
            return static_cast<int>(AssetKind::Texture);
        if (lower.find("sprite") != std::string::npos)
            return static_cast<int>(AssetKind::Sprite);
        return -1;
    }

    // kindFilter: -1 = all kinds. `search`: case-insensitive substring over
    // name AND mount path; empty matches everything.
    inline bool MatchesFilter(const AssetEntry& entry, int kindFilter, std::string_view search)
    {
        if (kindFilter >= 0 && static_cast<int>(entry.kind) != kindFilter)
            return false;
        if (search.empty())
            return true;

        auto containsCI = [](std::string_view hay, std::string_view needle)
        {
            if (needle.size() > hay.size())
                return false;
            auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
            {
                std::size_t j = 0;
                while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        };
        return containsCI(entry.name, search) || containsCI(entry.mountPath, search);
    }

    struct AssetBrowserState
    {
        char search[128] = {};
        int  kindFilter = -1;   // -1 = all
    };

    // Row actions the APP resolves after the draw (dialog launches never happen
    // inside the panel).
    struct AssetBrowserActions
    {
        Arcane::Guid createInstanceOf;   // context menu "New Instance..." on a material

        // Context menu "Create Sprite" on a texture row: mint (or reuse) a
        // .arcsprite that wraps this texture (sprite-asset spec, Section 3).
        // The panel only hands back WHICH texture; the app resolves reuse-or-
        // mint and opens the result (see AssetBrowserActions consumer).
        Arcane::Guid createSpriteFrom;

        // A scene is NOT a DocumentHost document -- double-clicking one must load
        // it into the editor session (replacing the Edit-mode registry), not open
        // a tab. The panel hands back the resolved path; the host is the one that
        // knows about SceneSession, so it is the host that calls Request() and
        // gates the load behind the same unsaved-changes guard the File menu uses.
        std::filesystem::path openScene;

        // Context menu "Set as Boot Scene" on a scene row.
        Arcane::Guid setBootScene;
    };

    // The "Assets" panel: filter row + entry table over the open project's
    // registry (rebuilt per frame -- registries are small; revisit with caching
    // when they are not). Every row is a kAssetDragType drag source.
    // Double-click routes into `docs` (materials open the shader editor) for
    // every kind EXCEPT Scene, which is not a document: its path comes back in
    // AssetBrowserActions::openScene for the host to load. Material, scene,
    // and texture rows each carry their own context menu ("New Instance..." /
    // "Set as Boot Scene" / "Create Sprite"); other kinds have none.
    AssetBrowserActions DrawAssetBrowserPanel(AssetBrowserState& state,
                                              const Arcane::Project* project,
                                              DocumentHost& docs);
}
