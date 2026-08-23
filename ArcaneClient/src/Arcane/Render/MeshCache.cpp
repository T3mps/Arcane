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
    // (MeshData/MeshBounds), nothing GPU. ONE map plus the failed set is the
    // whole state -- the loaded MeshAssetData is read, distilled into the
    // MeshEntry (geometry, bounds, the default material Guid) and dropped;
    // see MeshCache.hpp's "WHAT IT DOES NOT KEEP".
    struct MeshCache::Impl
    {
        Services services;
        std::unordered_map<Guid, MeshEntry> table;
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

    void MeshCache::Request(const Guid& id)
    {
        Impl& im = *m_impl;
        if (!id.IsValid() || im.table.contains(id) || im.failed.contains(id))
            return;

        // Negative-result caching into a SEPARATE set, deliberately unlike
        // SpriteCache::Request's cacheDefault: there is no placeholder mesh
        // to hand back, so a failure must stay OUT of the published table --
        // SpriteMaterialCache's discipline (its `failed` set,
        // SpriteMaterialCache.cpp:49, and the fail lambda at :97-101), applied
        // here instead of SpriteCache's own. The `failed`
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
        // The ONE field of the loaded asset that outlives this call: Task 5's
        // submission sweep reads the mesh's own default material straight off
        // the published MeshTable (MeshEntry::material) rather than through
        // the cache, so it never needs a MeshCache pointer of its own.
        // Everything else in `data` is already baked into the geometry above.
        entry.material = data->material;

        im.table.emplace(id, std::move(entry));
    }

    void MeshCache::Invalidate(const Guid& id)
    {
        // The TABLE entry goes, and that is where this cache differs from
        // SpriteMaterialCache::Invalidate (SpriteMaterialCache.cpp:68-76),
        // which deliberately KEEPS its table entry as last-good: a sprite
        // material stays bound and drawable while its replacement compiles,
        // and the entry is swapped only when the fresh compile lands. There is
        // no such gap here -- Request resolves synchronously in one shot -- so
        // holding a stale MeshEntry would buy nothing and would keep the
        // pre-edit geometry on screen for no reason. (SceneRenderResolver::
        // InvalidateMesh pairs this erase with an immediate Request precisely
        // so no frame can observe the hole.)
        //
        // `failed` is erased unconditionally, exactly as SpriteMaterialCache
        // does at the same site: that is what lets a since-fixed .arcmesh
        // succeed on the very next Request instead of staying memoized as
        // broken forever.
        m_impl->table.erase(id);
        m_impl->failed.erase(id);
    }

    void MeshCache::Clear()
    {
        m_impl->table.clear();
        m_impl->failed.clear();
    }
}
