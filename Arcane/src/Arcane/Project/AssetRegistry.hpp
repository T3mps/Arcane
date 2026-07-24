#pragma once

// AssetRegistry: the one map that matters -- Guid -> mount path ("game://a/b.json") --
// built by scanning a project's content tree at open. The physical file then comes from
// the MountTable (mount path -> file), so an asset can move on disk while its Guid (the
// stable reference) never changes. Where the Guid lives splits by asset kind (Unity's
// model): native JSON assets carry a top-level "id"; imported binary originals
// (.png/.wav/.ttf/...) that cannot embed one keep it in a sibling "<file>.meta" sidecar.
// In both cases a missing/invalid id is minted and written back (Unity-style auto-import).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unordered_map member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API AssetRegistry
    {
    public:
        // Clear the registry, then scan `contentDir` for assets -- native JSON (embedded
        // "id") and imported binary originals (id in a sibling "<file>.meta") -- assigning
        // or reading a stable Guid per asset, and register Guid -> "<scheme>://<relative>".
        // A file with no valid id gets one generated and written back. Returns the number
        // of assets registered. This is Clear() + AddContent(); a full rebuild from scratch.
        std::size_t ScanContent(const std::filesystem::path& contentDir, std::string_view scheme);

        // Scan another content root into the SAME registry WITHOUT clearing it first --
        // used to fold a mounted plugin's (or engine's) content in beside game:// content,
        // each under its own scheme ("plugin/<name>", "engine"). Returns the number of
        // assets newly registered by THIS call (duplicates keep the first + warn).
        std::size_t AddContent(const std::filesystem::path& contentDir, std::string_view scheme);

        // Guid -> mount path ("game://a/b.json"); nullopt if the id is unknown.
        std::optional<std::string> Resolve(const Guid& id) const;

        // Snapshot of every (guid, mount path) pair -- the asset browser's feed.
        // Unordered (map iteration order); presentation sorts as it likes.
        std::vector<std::pair<Guid, std::string>> All() const;

        std::size_t Count() const { return m_byGuid.size(); }
        void        Clear()       { m_byGuid.clear(); }

    private:
        std::unordered_map<Guid, std::string> m_byGuid;   // guid -> mount path
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
