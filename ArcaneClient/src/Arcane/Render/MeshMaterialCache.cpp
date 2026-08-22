#include <Arcane/Render/MeshMaterialCache.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Arcane
{
    // No pending/keep-alive state of any kind: unlike SpriteMaterialCache,
    // this cache submits no compile job, so Request() resolves synchronously
    // in one call and there is nothing for a later drain to finish. `failed`
    // is the ONLY extra bookkeeping beyond `table` -- see the header's
    // FAILURE DISCIPLINE block.
    struct MeshMaterialCache::Impl
    {
        Services services;
        std::unordered_map<Guid, ResolvedMeshMaterial> table;
        std::unordered_set<Guid> failed;
    };

    namespace
    {
        // This ONE level's own saved "baseColor", if it declares one -- Color
        // and Float4 both pack as a linear float4 (MatParamType's own
        // comment), and a "mesh"-kind asset carries no //@param decl to check
        // the author's spelling against (it has no snippet at all), so both
        // spellings are accepted the same way a real decl eventually would.
        // Any OTHER type saved under the name "baseColor" (a hand-edited
        // file, most likely) is ignored rather than reinterpreted -- reading
        // .f[0..3] off a Texture value would read `tex`'s bytes as floats.
        //
        // Returns the LAST match, matching MaterialAssetData::params being an
        // ordered vector rather than a map -- a hand-edited file with the
        // same name saved twice should not be ambiguous about which one won.
        std::optional<glm::vec4> OwnBaseColor(const MaterialAssetData& data)
        {
            std::optional<glm::vec4> found;
            for (const auto& [name, value] : data.params)
            {
                if (name != "baseColor")
                    continue;
                if (value.type != MatParamType::Color && value.type != MatParamType::Float4)
                    continue;
                found = glm::vec4(value.f[0], value.f[1], value.f[2], value.f[3]);
            }
            return found;
        }
    }

    MeshMaterialCache::MeshMaterialCache(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    MeshMaterialCache::~MeshMaterialCache() { delete m_impl; }

    const std::unordered_map<Guid, ResolvedMeshMaterial>& MeshMaterialCache::Table() const
    {
        return m_impl->table;
    }

    void MeshMaterialCache::Request(const Guid& id)
    {
        Impl& im = *m_impl;
        if (!id.IsValid() || im.table.contains(id) || im.failed.contains(id))
            return;

        // Same shape as SpriteMaterialCache::Request's fail() -- WARN once,
        // memoize into `failed` so the guard above turns back every later
        // Request for this Guid before this lambda runs again.
        auto fail = [&](const std::string& why)
        {
            ARC_WARN("MeshMaterialCache: material {} unavailable -- {}", id.ToString(), why);
            im.failed.insert(id);
        };

        if (!im.services.resolveAsset)
            return fail("no asset resolver installed");
        const auto path = im.services.resolveAsset(id);
        if (!path)
            return fail("not in the asset registry");
        auto data = LoadMaterialAsset(*path);
        if (!data)
            return fail("asset failed to load");

        // Walk to the base, EXACTLY SpriteMaterialCache::Request's cycle
        // guard and chain-must-reach-a-base refusal
        // (SpriteMaterialCache.cpp:112-133) -- collecting raw
        // MaterialAssetData instead of feeding a MaterialInstance, since
        // there is no template to build one over.
        std::vector<MaterialAssetData> chain;   // immediate parent .. base
        std::vector<Guid> visited{ id };
        Guid cursor = data->parent;
        while (cursor.IsValid())
        {
            for (const Guid& seen : visited)
                if (seen == cursor)
                    return fail("parent chain contains a cycle");
            visited.push_back(cursor);
            const auto parentPath = im.services.resolveAsset(cursor);
            if (!parentPath)
                return fail("parent not in the asset registry");
            auto parent = LoadMaterialAsset(*parentPath);
            if (!parent)
                return fail("parent failed to load");
            cursor = parent->parent;
            chain.push_back(std::move(*parent));
        }
        if (!chain.empty() && chain.back().IsInstance())
            return fail("parent chain never reaches a base material");

        // Layer base -> ... -> immediate parent -> this asset, last write
        // wins -- the same order SpriteMaterialCache::Impl::Bind applies
        // saved values in (base's params <- ... <- this asset's params,
        // "overrides on top"), just without a MaterialInstance to apply them
        // onto. A level that declares no "baseColor" of its own leaves the
        // running value untouched, so an instance that overrides nothing
        // inherits its base's colour exactly as UMaterialInstance would.
        ResolvedMeshMaterial resolved;   // baseColor defaults to (1,1,1,1)
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if (auto c = OwnBaseColor(*it))
                resolved.baseColor = *c;
        if (auto c = OwnBaseColor(*data))
            resolved.baseColor = *c;

        im.table.emplace(id, resolved);
    }

    void MeshMaterialCache::Invalidate(const Guid& id)
    {
        m_impl->table.erase(id);
        m_impl->failed.erase(id);
    }

    void MeshMaterialCache::Clear()
    {
        m_impl->table.clear();
        m_impl->failed.clear();
    }
}
