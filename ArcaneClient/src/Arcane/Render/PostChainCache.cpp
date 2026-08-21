#include <Arcane/Render/PostChainCache.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Arcane
{
    namespace
    {
        // Coalesce keys for post-chain compiles. The low byte carries ONLY
        // stage bits in every scheme, so 0x10/0x20 stay disjoint from the
        // document's 0x1/0x2 and the sprite cache's 0x4/0x8 for the SAME
        // asset; pass bits sit at << 8 exactly like the document's. (Node
        // previews live in a salted band none of these reach.)
        std::uint64_t PostStageKey(const Guid& id, bool vertex, std::size_t pass)
        {
            return (id.hi ^ (id.lo * 1099511628211ull)) ^
                   (vertex ? 0x10u : 0x20u) ^
                   (static_cast<std::uint64_t>(pass) << 8);
        }
    }

    struct PostChainCache::Impl
    {
        struct PassJob
        {
            std::uint64_t vsJob = 0, psJob = 0;
            std::vector<std::uint8_t> vsBytes, psBytes;
        };

        struct Pending
        {
            Guid id;
            std::vector<PassJob> jobs;                  // one per chain pass
            std::shared_ptr<MaterialTemplate> templ;    // the ONE merged layout
            std::vector<std::vector<std::uint32_t>> passInputs;
            std::uint32_t chainInputSlots = 1;
            MaterialAssetData data;                     // the SAVED asset
            std::vector<MaterialAssetData> parentChain; // immediate parent .. base
        };

        struct Entry
        {
            std::shared_ptr<MaterialInstance> instance;
            // The SAME bind, as bytes. Filled beside
            // `instance` so the two can never disagree about which compile is
            // live -- see PostChainCache::Desc.
            PostChainDesc desc;
        };

        Services services;
        std::unordered_map<Guid, Pending> pending;
        std::unordered_map<Guid, Entry> bound;
        std::unordered_set<Guid> failed;
        std::unordered_set<Guid> needsRefresh;   // invalidated but still bound

        void Bind(Pending& p);
    };

    PostChainCache::PostChainCache(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    PostChainCache::~PostChainCache() { delete m_impl; }

    const MaterialInstance* PostChainCache::Instance(const Guid& id) const
    {
        const auto it = m_impl->bound.find(id);
        return it != m_impl->bound.end() ? it->second.instance.get() : nullptr;
    }

    const PostChainDesc* PostChainCache::Desc(const Guid& id) const
    {
        const auto it = m_impl->bound.find(id);
        if (it == m_impl->bound.end() || it->second.desc.passes.empty())
            return nullptr;
        return &it->second.desc;
    }

    void PostChainCache::Invalidate(const Guid& id)
    {
        // Keep the bound entry: it stays rendering (last-good) until the
        // fresh compile lands and Bind() overwrites e.desc.passes wholesale
        // -- that publish is the only swap there is.
        m_impl->pending.erase(id);
        m_impl->failed.erase(id);
        if (m_impl->bound.contains(id))
            m_impl->needsRefresh.insert(id);
    }

    void PostChainCache::Clear()
    {
        m_impl->pending.clear();
        m_impl->bound.clear();
        m_impl->failed.clear();
        m_impl->needsRefresh.clear();
    }

    void PostChainCache::Request(const Guid& id, double now)
    {
        Impl& im = *m_impl;
        if (!id.IsValid() || !im.services.compiler || !im.services.sources ||
            !im.services.resolveAsset)
            return;
        if (im.pending.contains(id) || im.failed.contains(id))
            return;
        if (im.bound.contains(id) && !im.needsRefresh.contains(id))
            return;

        auto fail = [&](const std::string& why)
        {
            ARC_WARN("PostChainCache: material {} unavailable -- {}",
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

        // Instance chain: walk to the base (cycle-guarded) -- the base owns
        // the snippet/passes/vertex stage; every hop's saved values layer
        // under the child's.
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

        const MaterialAssetData& base = chain.empty() ? *data : chain.back();
        if (MaterialSurfaceForKind(base.kind) != MaterialSurface::Fullscreen)
            return fail("not a fullscreen material -- the post slot runs "
                        "fullscreen chains only");

        const auto templateText =
            im.services.sources->Get(MaterialTemplateFile(MaterialSurface::Fullscreen));
        if (!templateText)
            return fail("fullscreen template not found");

        // POST mode, always as a chain: a plain single-pass material is a
        // one-desc chain, so every post material renders through the ONE
        // chain path (and kSceneInput wires are valid).
        std::vector<MaterialChainPassDesc> descs;
        descs.reserve(1 + base.passes.size());
        descs.push_back({ base.snippet, base.baseInputs });
        for (const MaterialPass& pass : base.passes)
            descs.push_back({ pass.snippet, pass.inputs });

        MaterialChainBuildResult build = BuildMaterialChainSource(
            *templateText, descs, data->name, base.vertexSnippet,
            /*externalInput=*/true);
        if (!build.Ok())
        {
            for (const auto& pe : build.passErrors)
                if (!pe.empty())
                    return fail("material source errors: " + pe.front());
            return fail("material source errors: " + build.errors.front());
        }

        Impl::Pending p;
        p.id = id;
        p.templ = std::make_shared<MaterialTemplate>(std::move(build.templ));
        p.passInputs = std::move(build.passInputs);
        p.chainInputSlots = build.chainInputSlots;
        p.data = std::move(*data);
        p.parentChain = std::move(chain);
        p.jobs.resize(build.hlsl.size());
        for (std::size_t i = 0; i < build.hlsl.size(); ++i)
        {
            ShaderCompileRequest req;
            req.debugName = p.data.name + ".post.p" + std::to_string(i) + ".hlsl";
            req.sourceUtf8 = build.hlsl[i];
            req.entry = kPsEntry;
            req.profile = kPsProfile;
            req.coalesceKey = PostStageKey(id, /*vertex=*/false, i);
            p.jobs[i].psJob = im.services.compiler->Submit(req, now);
            req.entry = kVsEntry;
            req.profile = kVsProfile;
            req.coalesceKey = PostStageKey(id, /*vertex=*/true, i);
            p.jobs[i].vsJob = im.services.compiler->Submit(std::move(req), now);
        }

        im.pending.emplace(id, std::move(p));
        im.needsRefresh.erase(id);
    }

    bool PostChainCache::ConsumeResult(const ShaderCompileResult& result)
    {
        Impl& im = *m_impl;
        for (auto it = im.pending.begin(); it != im.pending.end(); ++it)
        {
            Impl::Pending& p = it->second;
            for (Impl::PassJob& job : p.jobs)
            {
                const bool isVs = result.jobId == job.vsJob && job.vsJob != 0;
                const bool isPs = result.jobId == job.psJob && job.psJob != 0;
                if (!isVs && !isPs)
                    continue;

                const auto& target = im.services.backend == GraphicsBackend::Vulkan
                                         ? result.spirv : result.dxil;
                if (!target.succeeded)
                {
                    // A failed FIRST compile marks the material failed; a
                    // failed RE-compile keeps the previous chain bound (the
                    // bound entry survives -- last-good).
                    ARC_WARN("PostChainCache: material {} failed to compile",
                             p.id.ToString());
                    if (!im.bound.contains(p.id))
                        im.failed.insert(p.id);
                    im.pending.erase(it);
                    return true;
                }

                (isVs ? job.vsBytes : job.psBytes) = target.bytecode;
                for (const Impl::PassJob& j : p.jobs)
                    if (j.vsBytes.empty() || j.psBytes.empty())
                        return true;   // other stages still in flight

                im.Bind(p);
                im.pending.erase(it);
                return true;
            }
        }
        return false;
    }

    void PostChainCache::Impl::Bind(Pending& p)
    {
        Impl& im = *this;

        // The compiled blobs for the graph recorder's PostChainNode
        // (PostChainDesc). MOVED out of the pending jobs rather than copied,
        // so nothing is duplicated and nothing is compiled twice.
        // `passBytes` is all this loop collects.
        std::vector<PostChainPassDesc> passBytes;
        passBytes.reserve(p.jobs.size());
        for (std::size_t i = 0; i < p.jobs.size(); ++i)
        {
            PostChainPassDesc bytes;
            bytes.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
                std::move(p.jobs[i].vsBytes));
            bytes.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
                std::move(p.jobs[i].psBytes));
            bytes.inputs = p.passInputs[i];
            passBytes.push_back(std::move(bytes));
        }

        // Layer the SAVED values exactly like the sprite cache's bind:
        // template <- base's params <- ... <- this asset's params.
        auto inst = std::make_shared<MaterialInstance>(
            std::shared_ptr<const MaterialTemplate>(p.templ));
        for (auto itChain = p.parentChain.rbegin(); itChain != p.parentChain.rend(); ++itChain)
        {
            ApplyMaterialParams(*itChain, *inst);
            inst = std::make_shared<MaterialInstance>(
                std::shared_ptr<const MaterialInstance>(inst));
        }
        ApplyMaterialParams(p.data, *inst);

        // THE PUBLISH IS UNCONDITIONAL: there is no device-side chain to
        // rebuild first, so nothing gates it.
        Entry& e = im.bound[p.id];
        e.instance = std::move(inst);

        // The byte-level twin of what was just bound, published in the same
        // breath so Instance()/Desc() can never name different compiles
        // (PostChainCache::Desc). Overwritten wholesale on a successful
        // REBIND, and left untouched by a failed one -- last-good applies to
        // both together.
        e.desc.templ           = p.templ;
        e.desc.instance        = e.instance;
        e.desc.chainInputSlots = p.chainInputSlots;
        e.desc.passes          = std::move(passBytes);

        // NO TEXTURE-PARAM WARM HAPPENS HERE. Residency for these Guids is
        // the graph recorder's job, on its OWN device, through
        // NriTextureCache; there was never a second place for it to happen.
    }
}
