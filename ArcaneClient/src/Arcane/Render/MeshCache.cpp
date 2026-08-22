#include <Arcane/Render/MeshCache.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>

#include <string>
#include <unordered_set>
#include <utility>

namespace Arcane
{
    // THERE IS NO KEEP-ALIVE OF ANYTHING DEVICE-SIDE, for the same reason
    // SpriteCache carries none: a MeshEntry owns plain CPU vectors
    // (MeshData/MeshBounds), nothing GPU. `assets` is the ONLY reason this
    // Impl needs a second map beside `table` -- AssetFor's backing store, kept
    // separate from MeshEntry so a reader of Table() (which Task 5's
    // submission sweep is) never has to carry MeshAssetData weight it does
    // not need per frame.
    struct MeshCache::Impl
    {
        Services services;
        std::unordered_map<Guid, MeshEntry> table;
        std::unordered_map<Guid, MeshAssetData> assets;
        std::unordered_set<Guid> failed;
    };

    MeshCache::MeshCache(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    MeshCache::~MeshCache() { delete m_impl; }

    const std::unordered_map<Guid, MeshEntry>& MeshCache::Table() const
    {
        return m_impl->table;
    }

    const MeshAssetData* MeshCache::AssetFor(const Guid& id) const
    {
        if (!id.IsValid())
            return nullptr;
        auto it = m_impl->assets.find(id);
        return it != m_impl->assets.end() ? &it->second : nullptr;
    }

    void MeshCache::Request(const Guid& id)
    {
        Impl& im = *m_impl;
        if (!id.IsValid() || im.table.contains(id) || im.failed.contains(id))
            return;

        // Negative-result caching into a SEPARATE set, deliberately unlike
        // SpriteCache::Request's cacheDefault: there is no placeholder mesh
        // to hand back, so a failure must stay OUT of the published table --
        // SpriteMaterialCache's discipline (SpriteMaterialCache.cpp:47,
        // 95-101), applied here instead of SpriteCache's own. The `failed`
        // set is what keeps this WARN to exactly once per broken Guid: the
        // guard above already turns back a second Request before this lambda
        // ever runs again.
        auto fail = [&](const std::string& why)
        {
            ARC_WARN("MeshCache: mesh {} unavailable -- {}", id.ToString(), why);
            im.failed.insert(id);
        };

        if (!im.services.resolveAsset)
            return fail("no asset resolver installed");
        const auto path = im.services.resolveAsset(id);
        if (!path)
            return fail("not in the asset registry");
        auto data = LoadMeshAsset(*path);
        if (!data)
            return fail("asset failed to load");

        // BuildMeshData returns nullopt EXACTLY when ValidateMeshAsset
        // refuses `*data` (MeshAsset.hpp's own contract) -- re-deriving the
        // human-readable reason here, rather than threading a
        // std::string through BuildMeshData's optional, costs one extra call
        // only on the failure path, which is not the one anything needs to be
        // fast on.
        auto meshData = BuildMeshData(*data);
        if (!meshData)
        {
            const auto reason = ValidateMeshAsset(*data);
            return fail(reason ? *reason : std::string("mesh failed to build"));
        }

        MeshEntry entry;
        entry.bounds   = ComputeMeshBounds(*meshData);
        entry.data     = std::move(*meshData);
        // Copied out before `data` moves into `im.assets` below -- Task 5's
        // submission sweep reads this straight off the published MeshTable
        // (MeshEntry::material) rather than through AssetFor, so it never
        // needs a MeshCache pointer of its own.
        entry.material = data->material;

        im.assets.emplace(id, std::move(*data));
        im.table.emplace(id, std::move(entry));
    }

    void MeshCache::Invalidate(const Guid& id)
    {
        m_impl->table.erase(id);
        m_impl->assets.erase(id);
        // Unlike SpriteMaterialCache::Invalidate, which KEEPS a failed Guid
        // out of its `failed` set only when there was never a table entry to
        // begin with, this cache has no "last-good" concept to preserve --
        // Request resolves synchronously in one shot, so there is nothing
        // stale left bound while a fresh compile lands. Clearing `failed`
        // here is what lets a since-fixed .arcmesh succeed on the very next
        // Request rather than staying memoized as broken forever.
        m_impl->failed.erase(id);
    }

    void MeshCache::Clear()
    {
        m_impl->table.clear();
        m_impl->assets.clear();
        m_impl->failed.clear();
    }
}
