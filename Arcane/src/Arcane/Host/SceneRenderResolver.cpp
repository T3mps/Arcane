#include <Arcane/Host/SceneRenderResolver.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Batcher2D.hpp>
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

        GlobalParams globals{};

        // The post assignment this frame + what it resolved to. Latched (rather
        // than re-queried by the host) so a host cannot read Chain() for a
        // DIFFERENT Guid than the one the sweep found.
        Guid                     postId{};
        FullscreenMaterialChain* postChain    = nullptr;
        const MaterialInstance*  postInstance = nullptr;

        // Warn-once latches. Cleared when the condition clears, so the message
        // returns if the scene regains the problem -- the shape the editor's
        // slice-3 post sweep already used.
        bool warnedMultiPost = false;

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

        SpriteCache::Services spriteServices;
        spriteServices.assets       = m_impl->services.runtime
                                    ? &m_impl->services.runtime->AssetsFacade() : nullptr;
        spriteServices.batcher      = m_impl->services.batcher;
        spriteServices.resolveAsset = resolveAsset;
        m_impl->sprites = std::make_unique<SpriteCache>(std::move(spriteServices));

        SpriteMaterialCache::Services materialServices;
        materialServices.compiler     = m_impl->services.compiler;
        materialServices.sources      = m_impl->services.sources;
        materialServices.assets       = spriteServices.assets;
        materialServices.device       = m_impl->services.device;
        materialServices.backend      = m_impl->services.backend;
        materialServices.resolveAsset = resolveAsset;
        m_impl->materials = std::make_unique<SpriteMaterialCache>(std::move(materialServices));

        PostChainCache::Services postServices;
        postServices.compiler     = m_impl->services.compiler;
        postServices.sources      = m_impl->services.sources;
        postServices.assets       = spriteServices.assets;
        postServices.device       = m_impl->services.device;
        postServices.backend      = m_impl->services.backend;
        postServices.resolveAsset = resolveAsset;
        m_impl->post = std::make_unique<PostChainCache>(std::move(postServices));
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
        }
        delete m_impl;
    }

    const GlobalParams& SceneRenderResolver::Globals() const { return m_impl->globals; }

    FullscreenMaterialChain* SceneRenderResolver::PostChain()    const { return m_impl->postChain; }
    const MaterialInstance*  SceneRenderResolver::PostInstance() const { return m_impl->postInstance; }

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

    void SceneRenderResolver::InvalidateMaterial(const Guid& id)
    {
        m_impl->materials->Invalidate(id);
        m_impl->post->Invalidate(id);
    }

    void SceneRenderResolver::Clear()
    {
        m_impl->sprites->Clear();
        m_impl->materials->Clear();
        m_impl->post->Clear();
        m_impl->postId       = Guid{};
        m_impl->postChain    = nullptr;
        m_impl->postInstance = nullptr;
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
        reg.CreateView<SpriteRenderer>().ForEach(
            [&](Astra::Entity, SpriteRenderer& s)
        {
            if (s.sprite.IsValid())
                im.sprites->Request(s.sprite);
            if (materialsReady && s.material.IsValid())
                im.materials->Request(s.material, frame.now);
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
        // compile becomes an NVRHI shader (ShaderCompiler.hpp:9-13). Results are
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
        im.postChain    = postId.IsValid() ? im.post->Chain(postId)    : nullptr;
        im.postInstance = postId.IsValid() ? im.post->Instance(postId) : nullptr;

        // (5) Publish. Every frame, not once at boot: the map addresses are
        // stable, but re-setting keeps the resources honest across project
        // switches and registry swaps (play-in-editor restores a snapshot into a
        // fresh registry, which drops resources with it).
        im.services.runtime->SetSpriteTable(&im.sprites->Table());
        im.services.runtime->SetSpriteMaterials(&im.materials->Table());
    }
}
