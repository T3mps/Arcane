#include <Arcane/Host/SceneRenderResolver.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/MeshCache.hpp>
#include <Arcane/Render/MeshMaterialCache.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/SpriteCache.hpp>
#include <Arcane/Render/SpriteMaterialCache.hpp>
#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace Arcane
{
    struct SceneRenderResolver::Impl
    {
        Services services;

        std::unique_ptr<SpriteCache>         sprites;
        std::unique_ptr<SpriteMaterialCache> materials;
        std::unique_ptr<PostChainCache>      post;
        // F2a (Task 6) siblings, one dimension up. Neither compiles anything
        // (see this header's WHAT IT OWNS block), so neither needs a
        // ShaderCompiler/backend/batcher wired into its own Services --
        // `resolveAsset` is the only thing they share with the three above.
        std::unique_ptr<MeshCache>         meshes;
        std::unique_ptr<MeshMaterialCache> meshMaterials;

        GlobalParams globals{};

        // The post assignment this frame + what it resolved to. Latched (rather
        // than re-queried by the host) so a host cannot read Instance() for a
        // DIFFERENT Guid than the one the sweep found.
        Guid                     postId{};
        const MaterialInstance*  postInstance = nullptr;
        // The same bind as bytes, for the `--nri-graph` vehicle (Task 10).
        const PostChainDesc*     postDesc     = nullptr;

        // Warn-once latches. Cleared when the condition clears, so the message
        // returns if the scene regains the problem -- the shape the editor's
        // slice-3 post sweep already used.
        bool warnedMultiPost = false;

        // Last reported census (see Refresh): -1 so the first sweep always logs.
        int lastSeenSprites   = -1;
        int lastTableSize     = -1;
        int lastMaterialCount = -1;

        bool CompilerReady() const
        {
            return services.compiler && services.compiler->IsAvailable();
        }
    };

    SceneRenderResolver::SceneRenderResolver(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);

        // ONE asset resolver for all three caches, built here from the open
        // project rather than passed in per host: a Guid resolves through the
        // CURRENT project's registry, and having both hosts hand-roll this
        // lambda is how they drifted apart in the first place. Re-reads
        // CurrentProject() per call, so it survives a project switch (paired
        // with Clear()).
        auto resolveAsset = [rt = m_impl->services.runtime](const Guid& g)
            -> std::optional<std::filesystem::path>
        {
            if (!rt)
                return std::nullopt;
            const Project* project = rt->CurrentProject();
            return project ? project->ResolveAsset(AssetId::FromGuid(g)) : std::nullopt;
        };

        // Hoisted, not read back off a moved-from Services below: all three
        // caches load textures through the same facade.
        Assets* assets = m_impl->services.runtime
                       ? &m_impl->services.runtime->AssetsFacade() : nullptr;

        // No batcher forward: SpriteCache::Services lost its `batcher` at ABI
        // v15 with Batcher2D::RemoveTexture, the only thing it fed.
        SpriteCache::Services spriteServices;
        spriteServices.assets       = assets;
        spriteServices.resolveAsset = resolveAsset;
        m_impl->sprites = std::make_unique<SpriteCache>(std::move(spriteServices));

        SpriteMaterialCache::Services materialServices;
        materialServices.compiler     = m_impl->services.compiler;
        materialServices.sources      = m_impl->services.sources;
        materialServices.assets       = assets;
        materialServices.backend      = m_impl->services.backend;
        materialServices.resolveAsset = resolveAsset;
        m_impl->materials = std::make_unique<SpriteMaterialCache>(std::move(materialServices));

        PostChainCache::Services postServices;
        postServices.compiler     = m_impl->services.compiler;
        postServices.sources      = m_impl->services.sources;
        postServices.assets       = assets;
        postServices.backend      = m_impl->services.backend;
        postServices.resolveAsset = resolveAsset;
        m_impl->post = std::make_unique<PostChainCache>(std::move(postServices));

        // F2a (Task 6) siblings: `resolveAsset` only -- neither cache builds
        // a compiled artifact, so there is no compiler/sources/backend/assets
        // field on either Services struct to fill in (MeshCache.hpp,
        // MeshMaterialCache.hpp).
        MeshCache::Services meshServices;
        meshServices.resolveAsset = resolveAsset;
        m_impl->meshes = std::make_unique<MeshCache>(std::move(meshServices));

        MeshMaterialCache::Services meshMaterialServices;
        meshMaterialServices.resolveAsset = resolveAsset;
        m_impl->meshMaterials = std::make_unique<MeshMaterialCache>(std::move(meshMaterialServices));
    }

    SceneRenderResolver::~SceneRenderResolver()
    {
        // The registry's SpriteTable / SpriteMaterialTable hold NON-OWNING
        // pointers into the caches below (SceneResources.hpp:96-99), so they
        // must be un-published before those maps die. Safe because every host
        // declares this object so it destructs BEFORE the Runtime (header
        // contract) -- the alternative is a live registry pointing at freed
        // maps, which is only harmless as long as nothing renders again, and
        // "nothing renders again" is not an invariant worth betting on.
        if (m_impl->services.runtime)
        {
            m_impl->services.runtime->SetSpriteTable(nullptr);
            m_impl->services.runtime->SetSpriteMaterials(nullptr);
            // F2a (Task 6) siblings: the SAME dangling-pointer hazard, one
            // dimension up (MeshTable::meshes / MeshMaterialTable::materials
            // are non-owning too -- SceneResources.hpp).
            m_impl->services.runtime->SetMeshTable(nullptr);
            m_impl->services.runtime->SetMeshMaterials(nullptr);
        }
        delete m_impl;
    }

    const GlobalParams& SceneRenderResolver::Globals() const { return m_impl->globals; }

    const MaterialInstance*  SceneRenderResolver::PostInstance() const { return m_impl->postInstance; }
    const PostChainDesc*     SceneRenderResolver::PostDesc()     const { return m_impl->postDesc; }

    SceneRenderResolver::MaterialCensus SceneRenderResolver::Materials() const
    {
        // BOTH halves are a live read of the CURRENT registry against the
        // CURRENT caches -- deliberately not a replay of what the last Refresh
        // latched. A half-live census would answer differently depending on
        // whether a Refresh had run yet, and the caller most in need of this
        // (anything polling it around Refresh calls, waiting on a fully-bound
        // frame) is exactly the caller that would trip over that ordering.
        MaterialCensus census;
        Impl& im = *m_impl;
        if (!im.services.runtime)
            return census;
        Astra::Registry& reg = im.services.runtime->Registry();

        reg.CreateView<SpriteRenderer>().ForEach(
            [&](Astra::Entity, SpriteRenderer& s)
        {
            if (!s.material.IsValid())
                return;
            ++census.spriteReferenced;
            // Table() IS the SpriteMaterialTable payload: present == registered
            // with the batcher == this sprite draws through its material rather
            // than the plain pipeline (RenderSystems.hpp's Resolve).
            if (im.materials->Table().contains(s.material))
                ++census.spriteBound;
        });

        // "First entity with a valid material wins" -- the SAME rule Refresh's
        // post sweep applies, restated rather than shared because Refresh also
        // owns the warn-once bookkeeping this must not touch. If that rule ever
        // changes, both copies change together.
        Guid postId{};
        reg.CreateView<PostProcess>().ForEach(
            [&](Astra::Entity, PostProcess& pp)
        {
            if (pp.material.IsValid() && !postId.IsValid())
                postId = pp.material;
        });
        census.postReferenced = postId.IsValid();
        // A non-null Desc() IS "bound and ready": PostChainCache only ever
        // publishes one once ConsumeResult's compile succeeded (Bind), and it
        // keeps a previously-bound one alive across a failed RE-compile
        // (last-good) rather than clearing it. Desc() is the ONLY "is the post
        // chain bound" signal there is.
        census.postBound = postId.IsValid() && im.post->Desc(postId) != nullptr;
        return census;
    }

    void SceneRenderResolver::InvalidateSprite(const Guid& id)
    {
        // Invalidate THEN Request synchronously, not Invalidate alone: a host may
        // render before its next Refresh, and an Invalidate-only call would erase
        // the entry and let that frame draw the 1x1 placeholder before anything
        // re-resolved it. SpriteCache::Request has no async step, so the entry is
        // back before this returns -- the no-gap property the material caches get
        // from their last-good scheme instead (SpriteMaterialCache.hpp:67-70).
        m_impl->sprites->Invalidate(id);
        m_impl->sprites->Request(id);
    }

    void SceneRenderResolver::InvalidateMesh(const Guid& id)
    {
        // Invalidate THEN Request synchronously -- identical shape and
        // identical reasoning to InvalidateSprite above (MeshCache::Request
        // has no async step, so the entry is rebuilt from the saved file
        // before this returns). See this method's header comment for the
        // borrow-lifetime rule every caller owes, and for why the
        // mesh-MATERIAL cache is deliberately left alone here.
        m_impl->meshes->Invalidate(id);
        m_impl->meshes->Request(id);
    }

    void SceneRenderResolver::InvalidateMaterial(const Guid& id)
    {
        m_impl->materials->Invalidate(id);
        m_impl->post->Invalidate(id);
        // Whole-cache, not Invalidate(id) -- see this method's own header
        // comment for why a targeted drop cannot be correct here (no reverse
        // parent -> child index, so a base's instances are not identifiable
        // from `id` alone). The stale mesh GEOMETRY cache (MeshCache) is
        // untouched: this hook is a MATERIAL (.arcmat) invalidation, and a
        // mesh's own default material is only a Guid riding inside its
        // MeshEntry, not something re-saving a .arcmat could ever change.
        m_impl->meshMaterials->Clear();
    }

    void SceneRenderResolver::Clear()
    {
        m_impl->sprites->Clear();
        m_impl->materials->Clear();
        m_impl->post->Clear();
        // F2a (Task 6) siblings: same project-switch contract (this method's
        // own header comment) -- a Guid resolves through the CURRENT
        // project's registry, so these two forget everything right alongside
        // the three above.
        m_impl->meshes->Clear();
        m_impl->meshMaterials->Clear();
        m_impl->postId       = Guid{};
        m_impl->postInstance = nullptr;
        m_impl->postDesc     = nullptr;
    }

    void SceneRenderResolver::Refresh(const FrameInfo& frame)
    {
        Impl& im = *m_impl;
        if (!im.services.runtime)
            return;

        im.globals.time           = (float)frame.now;
        im.globals.deltaTime      = (float)frame.dt;
        im.globals.viewportWidth  = frame.viewportWidth;
        im.globals.viewportHeight = frame.viewportHeight;

        Astra::Registry& reg = im.services.runtime->Registry();
        const bool compilerReady = im.CompilerReady();

        // (1) ONE CreateView<SpriteRenderer> walk for BOTH per-sprite Guids, so
        // adding the material path costs no second full component-view pass.
        // Request no-ops once a Guid is known either way, making this a cheap
        // per-frame guarantee that whatever the scene references is resolved
        // (sprites) or compiling/bound (materials).
        //
        // The sprite half is deliberately NOT gated on the compiler: it is a
        // synchronous JSON + texture-facade load with no compile step, and
        // gating it was a real bug (a machine without dxcompiler.dll drew every
        // sprite as an untextured 1x1 quad with no diagnostic). The material
        // half additionally needs a batcher, since a compiled material can only
        // be bound by registering it with one -- without it the compile would
        // be drained and dropped, stranding the material as permanently
        // pending.
        const bool materialsReady = compilerReady && im.services.batcher != nullptr;
        int seenSprites = 0, seenSpriteGuids = 0, seenMaterialGuids = 0;
        reg.CreateView<SpriteRenderer>().ForEach(
            [&](Astra::Entity, SpriteRenderer& s)
        {
            ++seenSprites;
            if (s.sprite.IsValid())
            {
                ++seenSpriteGuids;
                im.sprites->Request(s.sprite);
            }
            if (s.material.IsValid())
                ++seenMaterialGuids;
            if (materialsReady && s.material.IsValid())
                im.materials->Request(s.material, frame.now);
        });

        // Census, logged once per distinct outcome. "Nothing draws" has too many
        // indistinguishable causes to debug from a silent log: no SpriteRenderer in
        // the scene, a nil sprite Guid, a Guid that would not resolve, or a
        // component view that is empty because THIS module disagrees with the
        // loader about the component id. Those need different fixes, and the
        // absence of a warning used to be consistent with all of them.
        // Deliberately counted in Arcane.dll, where the loader also runs, so
        // comparing this line against a host-side count localises a cross-module
        // id problem immediately.
        // Watches the MATERIAL count too, not just the sprite counts: materials bind
        // asynchronously (compile + drain), so the first frame always reports zero of
        // them and a sprite-only trigger would never correct itself. Verified the hard
        // way -- the log claimed "0 bound material(s)" for a run whose screenshot
        // showed the material visibly animating.
        if (seenSprites != im.lastSeenSprites ||
            (int)im.sprites->Table().size() != im.lastTableSize ||
            (int)im.materials->Table().size() != im.lastMaterialCount)
        {
            im.lastSeenSprites   = seenSprites;
            im.lastTableSize     = (int)im.sprites->Table().size();
            im.lastMaterialCount = (int)im.materials->Table().size();
            ARC_INFO("scene resolution: {} SpriteRenderer(s), {} sprite guid(s), "
                     "{} material guid(s) -> {} resolved sprite(s), {} bound material(s)",
                     seenSprites, seenSpriteGuids, seenMaterialGuids,
                     im.sprites->Table().size(), im.materials->Table().size());
        }

        // (1b) F2a (Task 6): ONE CreateView<MeshRenderer> walk, mirroring (1)
        // above one dimension up -- a SEPARATE view, not folded into (1),
        // because MeshRenderer and SpriteRenderer are different component
        // types with no shared entity requirement.
        //
        // UNGATED on the compiler, unlike the sprite-material half of (1):
        // neither mesh cache compiles anything (this header's WHAT IT OWNS
        // block; MeshMaterialCache.hpp:9-21), so there is no async compile
        // step whose readiness this sweep needs to wait on -- the same
        // reason the sprite GEOMETRY half of (1) is not gated either. No
        // batcher check either, for the same reason: nothing here registers
        // a pipeline with one.
        //
        // ORDERING IS LOAD-BEARING: mesh first, then materials, WITHIN THE
        // SAME entity and the SAME Refresh call -- never two independent
        // views. A MeshRenderer's own default material (MeshEntry::material)
        // is only knowable AFTER MeshCache::Request resolves the mesh (it
        // arrives copied off the loaded .arcmesh, MeshCache.cpp), so
        // requesting it from a stale prior-frame table read (or not at all
        // this frame) would leave a newly-referenced mesh's default material
        // one frame late every time -- exactly the kind of bug that is
        // miserable to find at a desk.
        //
        // No census log for this sweep (unlike (1)'s ARC_INFO block above):
        // that instrument is scoped to the SpriteRenderer path it was built
        // to debug, and bolting a parallel counter/log pair on here without
        // a concrete "nothing draws" report to chase would be exactly the
        // kind of unrequested widening this task's dispatch warns against.
        reg.CreateView<MeshRenderer>().ForEach(
            [&](Astra::Entity, MeshRenderer& mr)
        {
            if (!mr.mesh.IsValid())
                return;
            im.meshes->Request(mr.mesh);

            // Look the JUST-RESOLVED (or already-known, or freshly-failed)
            // entry up rather than trusting `mr.mesh` resolved: a broken
            // .arcmesh Guid leaves no entry at all (MeshCache's own FAILURE
            // DISCIPLINE), and there is no default material to chase for a
            // mesh that itself never resolved.
            const auto& meshTable = im.meshes->Table();
            const auto it = meshTable.find(mr.mesh);
            if (it == meshTable.end())
                return;

            if (mr.materialOverride.IsValid())
                im.meshMaterials->Request(mr.materialOverride);
            if (it->second.material.IsValid())
                im.meshMaterials->Request(it->second.material);
        });

        // (2) The scene's post assignment: FIRST entity with a valid material
        // wins (one scene, one chain -- per-camera stacks are a non-goal), and
        // more than one warns once.
        Guid postId{};
        int  postCount = 0;
        reg.CreateView<PostProcess>().ForEach(
            [&](Astra::Entity, PostProcess& pp)
        {
            if (!pp.material.IsValid())
                return;
            if (postCount++ == 0)
                postId = pp.material;
        });
        if (postCount > 1)
        {
            if (!im.warnedMultiPost)
                ARC_WARN("scene carries {} PostProcess assignments -- the first found wins",
                         postCount);
            im.warnedMultiPost = true;
        }
        else
        {
            im.warnedMultiPost = false;
        }
        if (compilerReady && postId.IsValid())
            im.post->Request(postId, frame.now);

        // (3) The ONE drain site in the process: this is where a completed
        // compile is taken up (ShaderCompiler.hpp:9-13). Results are
        // offered to the host first (the editor's open shader documents), then
        // the two material caches. The ordering is safe because cache-owned
        // compiles use coalesce keys disjoint from a document's for the same
        // asset (SpriteMaterialCache.cpp:26-29), so neither can claim the
        // other's result.
        if (compilerReady)
        {
            im.services.compiler->Poll(frame.now);
            for (const ShaderCompileResult& r : im.services.compiler->Drain())
            {
                bool consumed = im.services.consumeFirst && im.services.consumeFirst(r);
                if (!consumed && im.services.batcher)
                    consumed = im.materials->ConsumeResult(r, *im.services.batcher);
                if (!consumed)
                    consumed = im.post->ConsumeResult(r);
            }
        }

        // (4) Latch the post hook AFTER the drain, so a chain that finished
        // compiling THIS frame is bound for this frame's render rather than the
        // next one.
        im.postId       = postId;
        im.postInstance = postId.IsValid() ? im.post->Instance(postId) : nullptr;
        im.postDesc     = postId.IsValid() ? im.post->Desc(postId)     : nullptr;

        // (5) Publish. Every frame, not once at boot: the map addresses are
        // stable, but re-setting keeps the resources honest across project
        // switches and registry swaps (play-in-editor restores a snapshot into a
        // fresh registry, which drops resources with it).
        im.services.runtime->SetSpriteTable(&im.sprites->Table());
        im.services.runtime->SetSpriteMaterials(&im.materials->Table());
        // F2a (Task 6) siblings -- same every-frame reasoning.
        im.services.runtime->SetMeshTable(&im.meshes->Table());
        im.services.runtime->SetMeshMaterials(&im.meshMaterials->Table());
    }
}
