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

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unordered_map member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API AssetRegistry
    {
    public:
        // Recursively scan `contentDir` for assets -- native JSON (embedded "id") and
        // imported binary originals (id in a sibling "<file>.meta") -- assigning or reading
        // a stable Guid per asset, and register Guid -> "<scheme>://<relative>". A file with
        // no valid id gets one generated and written back. Returns the number of assets
        // registered. Safe to call repeatedly (rebuilds from scratch).
        std::size_t ScanContent(const std::filesystem::path& contentDir, std::string_view scheme);

        // Guid -> mount path ("game://a/b.json"); nullopt if the id is unknown.
        std::optional<std::string> Resolve(const Guid& id) const;

        std::size_t Count() const { return m_byGuid.size(); }
        void        Clear()       { m_byGuid.clear(); }

    private:
        std::unordered_map<Guid, std::string> m_byGuid;   // guid -> mount path
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
