#include <Arcane/Render/SpriteMaterialCache.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <utility>
#include <vector>

namespace Arcane
{
    namespace
    {
        // Coalesce keys for cache-owned compiles. 0x4/0x8 keep them disjoint
        // from ShaderEditorDocument's 0x1/0x2 keys for the SAME asset -- an
        // open document and a scene reference must never cancel each other's
        // in-flight compiles.
        std::uint64_t CacheStageKey(const Guid& id, bool vertex)
        {
            return (id.hi ^ (id.lo * 1099511628211ull)) ^ (vertex ? 0x4u : 0x8u);
        }
    }

    struct SpriteMaterialCache::Impl
    {
        struct Pending
        {
            Guid id;
            std::uint64_t vsJob = 0, psJob = 0;
            std::vector<std::uint8_t> vsBytes, psBytes;
            std::shared_ptr<MaterialTemplate> templ;
            MaterialAssetData data;                     // the SAVED asset
            std::vector<MaterialAssetData> parentChain; // immediate parent .. base
        };

        Services services;
        std::unordered_map<Guid, Pending> pending;
        std::unordered_map<Guid, std::uint16_t> table;
        std::unordered_set<Guid> failed;
        std::unordered_set<Guid> needsRefresh;   // invalidated but still bound

        void Bind(Pending& p, Batcher2D& batcher);
    };

    SpriteMaterialCache::SpriteMaterialCache(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    SpriteMaterialCache::~SpriteMaterialCache() { delete m_impl; }

    const std::unordered_map<Guid, std::uint16_t>& SpriteMaterialCache::Table() const
    {
        return m_impl->table;
    }

    void SpriteMaterialCache::Invalidate(const Guid& id)
    {
        // Keep the table entry: it stays bound (last-good) until the fresh
        // compile lands and UpdateMaterial swaps the slot's contents.
        m_impl->pending.erase(id);
        m_impl->failed.erase(id);
        if (m_impl->table.contains(id))
            m_impl->needsRefresh.insert(id);
    }

    void SpriteMaterialCache::Clear()
    {
        m_impl->pending.clear();
        m_impl->table.clear();
        m_impl->failed.clear();
        m_impl->needsRefresh.clear();
    }

    void SpriteMaterialCache::Request(const Guid& id, double now)
    {
        Impl& im = *m_impl;
        if (!id.IsValid() || !im.services.compiler || !im.services.sources ||
            !im.services.resolveAsset)
            return;
        if (im.pending.contains(id) || im.failed.contains(id))
            return;
        if (im.table.contains(id) && !im.needsRefresh.contains(id))
            return;

        auto fail = [&](const std::string& why)
        {
            ARC_WARN("SpriteMaterialCache: material {} unavailable -- {}",
                     id.ToString(), why);
            im.failed.insert(id);
            im.needsRefresh.erase(id);
        };

        const auto path = im.services.resolveAsset(id);
        if (!path)
            return fail("not in the asset registry");
        auto data = LoadMaterialAsset(*path);
        if (!data)
            return fail("asset failed to load");

        // Instance chain: walk to the base (cycle-guarded) -- the base owns the
        // snippet; every hop's saved values layer under the child's.
        std::vector<MaterialAssetData> chain;
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

        const std::string& snippet = chain.empty() ? data->snippet
                                                   : chain.back().snippet;
        const auto templateText =
            im.services.sources->Get(MaterialTemplateFile(MaterialSurface::Sprite));
        if (!templateText)
            return fail("sprite template not found");

        MaterialBuildResult build = BuildMaterialShaderSource(
            *templateText, snippet, data->name, MaterialSurface::Sprite);
        if (!build.errors.empty())
            return fail("material source errors: " + build.errors.front());

        Impl::Pending p;
        p.id = id;
        p.templ = std::make_shared<MaterialTemplate>(std::move(build.templ));
        p.data = std::move(*data);
        p.parentChain = std::move(chain);

        ShaderCompileRequest req;
        req.debugName = p.data.name + ".sprite.hlsl";
        req.sourceUtf8 = build.hlsl;
        req.entry = kPsEntry;
        req.profile = kPsProfile;
        req.coalesceKey = CacheStageKey(id, /*vertex=*/false);
        p.psJob = im.services.compiler->Submit(req, now);
        req.entry = kVsEntry;
        req.profile = kVsProfile;
        req.coalesceKey = CacheStageKey(id, /*vertex=*/true);
        p.vsJob = im.services.compiler->Submit(std::move(req), now);

        im.pending.emplace(id, std::move(p));
        im.needsRefresh.erase(id);
    }

    bool SpriteMaterialCache::ConsumeResult(const ShaderCompileResult& result,
                                            Batcher2D& batcher)
    {
        Impl& im = *m_impl;
        for (auto it = im.pending.begin(); it != im.pending.end(); ++it)
        {
            Impl::Pending& p = it->second;
            const bool isVs = result.jobId == p.vsJob && p.vsJob != 0;
            const bool isPs = result.jobId == p.psJob && p.psJob != 0;
            if (!isVs && !isPs)
                continue;

            const auto& target = im.services.backend == GraphicsBackend::Vulkan
                                     ? result.spirv : result.dxil;
            if (!target.succeeded)
            {
                // A failed FIRST compile marks the material failed; a failed
                // RE-compile keeps the previous registration bound (the table
                // entry survives -- last-good).
                ARC_WARN("SpriteMaterialCache: material {} failed to compile",
                         p.id.ToString());
                if (!im.table.contains(p.id))
                    im.failed.insert(p.id);
                im.pending.erase(it);
                return true;
            }

            (isVs ? p.vsBytes : p.psBytes) = target.bytecode;
            if (p.vsBytes.empty() || p.psBytes.empty())
                return true;   // the other stage is still in flight

            im.Bind(p, batcher);
            im.pending.erase(it);
            return true;
        }
        return false;
    }

    void SpriteMaterialCache::Impl::Bind(Pending& p, Batcher2D& batcher)
    {
        Impl& im = *this;

        // THE SEVERANCE (NRI Phase 3, Task 2). createShader is the ONE line in
        // this whole function that needs a graphics device, and what it
        // produces -- the two nvrhi handles -- is consumed by the NVRHI
        // recorder alone. The BLOBS below are what the graph recorder builds
        // its own pipeline from, and they exist whether or not a device does.
        // So a device-less run skips this and registers a bytes-only material
        // (Batcher2D::BuildEntry accepts exactly that); nothing else here
        // changes, and with a device present the behaviour is unchanged.
        nvrhi::ShaderHandle vs, ps;
        if (im.services.device)
        {
            vs = im.services.device->createShader(
                nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex)
                    .setEntryName(kVsEntry)
                    .setDebugName((p.data.name + "_sprite_vs").c_str()),
                p.vsBytes.data(), p.vsBytes.size());
            ps = im.services.device->createShader(
                nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel)
                    .setEntryName(kPsEntry)
                    .setDebugName((p.data.name + "_sprite_ps").c_str()),
                p.psBytes.data(), p.psBytes.size());
            if (!vs || !ps)
            {
                ARC_WARN("SpriteMaterialCache: createShader failed for material {}",
                         p.id.ToString());
                if (!im.table.contains(p.id))
                    im.failed.insert(p.id);
                return;
            }
        }

        // Layer the SAVED values exactly like the editor's bind: template <-
        // base's params <- ... <- this asset's params (overrides on top).
        auto inst = std::make_shared<MaterialInstance>(
            std::shared_ptr<const MaterialTemplate>(p.templ));
        for (auto itChain = p.parentChain.rbegin(); itChain != p.parentChain.rend(); ++itChain)
        {
            ApplyMaterialParams(*itChain, *inst);
            inst = std::make_shared<MaterialInstance>(
                std::shared_ptr<const MaterialInstance>(inst));
        }
        ApplyMaterialParams(p.data, *inst);

        // Declared texture params -> GPU handles through the Assets GUID seam
        // (null slots fall back to the batcher's white texel).
        Material2DDesc desc;
        desc.vs = vs;
        desc.ps = ps;
        desc.templ = p.templ;
        desc.instance = inst;
        // The SAME blobs the two handles above were created from, retained for
        // the second recorder (NRI Phase 2, Task 9 -- Material2DDesc::vsBytes).
        // Moved out of the pending entry, which is erased right after this
        // returns, so nothing is copied and nothing is compiled twice. Exactly
        // ONE target survives: the one `im.services.backend` selected above,
        // which is the backend the whole process runs on.
        desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(p.vsBytes));
        desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(p.psBytes));
        const std::vector<Guid> texGuids = inst->ResolveTextures();
        // SIZED even device-less: the table's WIDTH is the template's texture
        // count and both recorders bind that many t1.. slots. Only the
        // CONTENTS need a device -- and the graph recorder never reads them
        // (it resolves the same Guids through NriTextureCache onto its own
        // device), so a device-less run leaves them null rather than warming a
        // per-texture GetTexture failure into the Assets cache.
        desc.paramTextures.resize(texGuids.size());
        if (im.services.assets && im.services.device)
            for (std::size_t i = 0; i < texGuids.size(); ++i)
                if (texGuids[i].IsValid())
                    desc.paramTextures[i] =
                        im.services.assets->GetTexture(AssetId::FromGuid(texGuids[i]));

        const auto existing = im.table.find(p.id);
        if (existing != im.table.end())
        {
            if (!batcher.UpdateMaterial(existing->second, std::move(desc)))
                ARC_WARN("SpriteMaterialCache: UpdateMaterial failed for {}",
                         p.id.ToString());
            return;
        }
        const std::uint16_t materialId = batcher.RegisterMaterial(std::move(desc));
        if (materialId == Batcher2D::kInvalidMaterialId)
        {
            im.failed.insert(p.id);
            return;
        }
        im.table.emplace(p.id, materialId);
    }
}
