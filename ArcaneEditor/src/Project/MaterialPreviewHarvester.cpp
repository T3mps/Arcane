#include "Project/MaterialPreviewHarvester.hpp"

#include <Arcane/Assets/Assets.hpp>          // LoadDisplayPixels / WriteThumbnailPngRgba
#include <Arcane/Assets/ImageIo.hpp>         // PixelData
#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/MeshBuilder.hpp>
#include <Arcane/Render/MeshMaterialCache.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/PostChainCache.hpp>   // PostChainDesc
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Scene/SceneCamera.hpp>       // PerspectiveProjection

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        // 64px, the size the Assets panel's tile draws at. Fixed rather than
        // configurable: it is also the on-disk PNG's size and the loader's
        // maxSize, and three numbers that must agree are better as one.
        constexpr std::uint32_t kThumbSize = 64;

        // The checkerboard cell, scaled from the shader editor's 32-at-512 to
        // keep the same 4x4-ish read at a sixteenth of the area.
        constexpr float kCheckerCell = 16.0f;

        // A FIXED clock for every thumbnail. A time-driven material (a pulse,
        // a scroll) would otherwise harvest a different picture every time,
        // which makes the persisted PNG and the live render disagree for no
        // reason anybody can act on. Non-zero so a material whose animation
        // starts flat at t=0 still shows something.
        constexpr float kThumbTime = 0.35f;

        // ===== THE HARVESTER'S SLOT IN THE PROCESS-WIDE STAGE-KEY SCHEME ====
        // 0x40/0x80, disjoint from ShaderEditorDocument's 0x1/0x2, the sprite
        // cache's 0x4/0x8 and the post cache's 0x10/0x20 for the SAME asset.
        // This is not tidiness: ShaderCompiler::Submit COALESCES by key and a
        // newer Submit CANCELS the older pending one, so a thumbnail compile
        // sharing a key with the scene's would silently strand the scene's
        // cache on a result that never arrives. `pass` mixes in for the same
        // reason PostStageKey's does -- a chain's passes are separate jobs.
        std::uint64_t ThumbStageKey(const Arcane::Guid& id, bool vertex, std::size_t pass = 0)
        {
            return (id.hi ^ (id.lo * 1099511628211ull)) ^
                   (vertex ? 0x40u : 0x80u) ^
                   (static_cast<std::uint64_t>(pass) << 32);
        }

        // Layer the SAVED values onto a fresh instance -- template <- base's
        // params <- ... <- the leaf's params. Byte-for-byte the shape
        // SpriteMaterialCache::Impl::Bind and PostChainCache::Impl::Bind both
        // use; kept here rather than shared because neither exports it.
        std::shared_ptr<Arcane::MaterialInstance> LayerParams(
            const std::shared_ptr<Arcane::MaterialTemplate>& templ,
            const std::vector<Arcane::MaterialAssetData>& parentChain,
            const Arcane::MaterialAssetData& leaf)
        {
            auto inst = std::make_shared<Arcane::MaterialInstance>(
                std::shared_ptr<const Arcane::MaterialTemplate>(templ));
            for (auto it = parentChain.rbegin(); it != parentChain.rend(); ++it)
            {
                Arcane::ApplyMaterialParams(*it, *inst);
                inst = std::make_shared<Arcane::MaterialInstance>(
                    std::shared_ptr<const Arcane::MaterialInstance>(inst));
            }
            Arcane::ApplyMaterialParams(leaf, *inst);
            return inst;
        }
    }

    struct MaterialPreviewHarvester::Impl
    {
        // ---- what one material's compile is waiting on ------------------
        struct PassJob
        {
            std::uint64_t vsJob = 0, psJob = 0;
            std::vector<std::uint8_t> vsBytes, psBytes;
        };
        struct Pending
        {
            Arcane::Guid id;
            Arcane::MaterialSurface surface = Arcane::MaterialSurface::Sprite;
            std::shared_ptr<Arcane::MaterialTemplate> templ;
            Arcane::MaterialAssetData data;
            std::vector<Arcane::MaterialAssetData> parentChain;
            std::vector<PassJob> jobs;                        // 1 for sprite, N for a chain
            std::vector<std::vector<std::uint32_t>> passInputs;   // fullscreen only
            std::uint32_t chainInputSlots = 1;                    // fullscreen only
        };

        // ---- what one material renders as, once the compile has landed --
        struct Ready
        {
            Arcane::Guid id;
            Arcane::MaterialSurface surface = Arcane::MaterialSurface::Sprite;
            std::uint16_t spriteMaterial = Arcane::Batcher2D::kInvalidMaterialId;  // Sprite
            Arcane::PostChainDesc post;                                            // Fullscreen
            glm::vec4 meshColor{1.0f};                                             // Mesh
            std::uint32_t meshSlot = 0xFFFFFFFFu;                                  // Mesh
        };

        // ---- one material's finished thumbnail --------------------------
        struct Entry
        {
            Arcane::Guid      thumbId;          // synthetic chrome-cache key
            Arcane::PixelData pixels;           // 64px RGBA, the supply's answer
            std::uint64_t     texture = 0;      // ImTextureID, 0 until resolved
        };

        Services services;

        // The vehicle + its recorder. Created LAZILY, ONCE, on the first
        // actual harvest -- never at construction (no device then) and never
        // per harvest.
        std::unique_ptr<Arcane::NriGraphContext> ctx;
        std::unique_ptr<Arcane::Batcher2D>       batch;
        // Mesh thumbnails: one sphere for the whole session, and a
        // params-only material cache. MeshMaterialCache touches NO compiler
        // (its own header says so), so a second instance of it cannot
        // collide with the scene resolver's the way a second
        // SpriteMaterialCache/PostChainCache would.
        Arcane::MeshData sphere;
        std::unique_ptr<Arcane::MeshMaterialCache> meshMats;

        // THE QUEUE IS A STACK. Task 10's Browse draw pushes every VISIBLE
        // un-thumbed material each frame, so the newest push is the one on
        // screen right now -- LIFO is what makes the frame's single harvest
        // land on something the user is looking at (UE's thumbnail pool is
        // LIFO for exactly this).
        std::deque<Arcane::Guid> queue;
        std::unordered_map<Arcane::Guid, Pending> pending;
        std::vector<Ready> ready;                    // also LIFO (back = newest)
        std::unordered_set<Arcane::Guid> failed;     // memoized refusals
        std::unordered_map<Arcane::Guid, Entry> entries;
        std::unordered_map<Arcane::Guid, Arcane::Guid> thumbToMaterial;

        // `inFlight` IS THE ONE MEMBERSHIP TEST, and it spans all three
        // stages -- `queue`, `pending` and `ready`. Keeping one set rather
        // than probing three containers is what makes Request()'s per-frame,
        // per-visible-row call a single hash lookup, and what makes "did this
        // guid get dropped somewhere" a single invariant instead of three.
        // Every terminal outcome (a harvest, a refusal) erases from it;
        // nothing else does, except Invalidate, which re-arms the guid.
        std::unordered_set<Arcane::Guid> inFlight;

        [[nodiscard]] Arcane::NriGraphContext* Chrome() const
        {
            return services.chromeGraph ? services.chromeGraph() : nullptr;
        }

        [[nodiscard]] std::filesystem::path ThumbPath(const Arcane::Guid& id) const
        {
            if (!services.thumbnailDir)
                return {};
            const std::filesystem::path dir = services.thumbnailDir();
            if (dir.empty())
                return {};
            return dir / (id.ToString() + ".png");
        }

        void Push(const Arcane::Guid& id)
        {
            if (inFlight.insert(id).second)
                queue.push_front(id);   // LIFO
        }

        bool EnsureVehicle(Arcane::NriGraphContext& chrome);
        void StartOne(const Arcane::Guid& id, double now);
        [[nodiscard]] static bool AllStagesLanded(const Pending& p);
        [[nodiscard]] bool Publish(Pending& p);   // pending -> ready; false = refused
        bool Harvest(Ready& r);                   // ONE device idle
        std::uint64_t EnsureTexture(const Arcane::Guid& material, Entry& e) const;
        // BY VALUE, deliberately: every caller passes a Guid that lives INSIDE
        // the `pending` node this function erases.
        void Fail(Arcane::Guid id, std::string_view why);
    };

    // =====================================================================
    // Lifecycle
    // =====================================================================
    MaterialPreviewHarvester::MaterialPreviewHarvester(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    MaterialPreviewHarvester::~MaterialPreviewHarvester()
    {
        // The vehicle BORROWS the chrome context's device, so it must not
        // outlive it. EditorApp::ShutdownGraphPath calls Shutdown() while
        // that device is provably alive; this is the backstop for a run that
        // never got that far (a failed boot), where nothing was ever created
        // in the first place.
        delete m_impl;
    }

    void MaterialPreviewHarvester::Shutdown()
    {
        Impl& im = *m_impl;
        // Order matters only in one direction: the batcher is device-less and
        // the vehicle owns real NRI objects, so the vehicle goes while the
        // device it borrowed is still alive.
        im.ctx.reset();
        im.batch.reset();
        im.meshMats.reset();
        im.sphere = {};
        // The thumbnails themselves are CPU bytes and survive -- but their
        // nri::Texture* ids named textures the CHROME cache owns, and that
        // cache dies with the chrome context. Zero them so nothing can hand a
        // dangling ImTextureID to ImGui afterwards.
        for (auto& [id, e] : im.entries)
            e.texture = 0;
    }

    void MaterialPreviewHarvester::Clear()
    {
        Impl& im = *m_impl;
        // A Guid means something else in a different project, and the
        // batcher's registered material slots are the OLD project's, so the
        // vehicle goes with everything else. The next Pump rebuilds it.
        Shutdown();
        im.queue.clear();
        im.inFlight.clear();
        im.pending.clear();
        im.ready.clear();
        im.failed.clear();
        im.entries.clear();
        im.thumbToMaterial.clear();
    }

    // =====================================================================
    // The queue
    // =====================================================================
    void MaterialPreviewHarvester::Request(const Arcane::Guid& material)
    {
        Impl& im = *m_impl;
        if (!material.IsValid())
            return;
        // The steady-state cost of the Browse draw's per-frame push for every
        // visible row: three hash lookups and out.
        if (im.entries.contains(material) || im.failed.contains(material) ||
            im.inFlight.contains(material))
            return;
        im.Push(material);
    }

    void MaterialPreviewHarvester::Invalidate(const Arcane::Guid& material)
    {
        Impl& im = *m_impl;
        if (!material.IsValid())
            return;
        // The EXISTING thumbnail stays served until the new one lands
        // (last-good -- the same rule SpriteMaterialCache::Invalidate keeps),
        // so `entries` is deliberately NOT erased here. What IS dropped is
        // every memo that would stop a re-harvest: the refusal memo, any
        // half-finished compile, and the on-disk PNG's claim (the harvest
        // overwrites it, but a crash between here and then must not leave a
        // stale PNG that PrimeFromDisk would trust on the next boot -- the
        // .arcmat mtime comparison would catch a disk edit, but not a cook or
        // a parent-chain change, which touch this material's file not at all).
        im.failed.erase(material);
        im.pending.erase(material);
        im.ready.erase(std::remove_if(im.ready.begin(), im.ready.end(),
                                      [&](const Impl::Ready& r) { return r.id == material; }),
                       im.ready.end());
        im.queue.erase(std::remove(im.queue.begin(), im.queue.end(), material), im.queue.end());
        // AFTER the three erases above, never before: `inFlight` is what Push
        // dedupes against, so a material already somewhere in the pipeline
        // with now-STALE compile jobs or stale bytes would otherwise be
        // silently dropped rather than restarted.
        im.inFlight.erase(material);
        if (const std::filesystem::path png = im.ThumbPath(material); !png.empty())
        {
            std::error_code ec;
            std::filesystem::remove(png, ec);
        }
        im.Push(material);
    }

    std::size_t MaterialPreviewHarvester::PendingCount() const
    {
        const Impl& im = *m_impl;
        return im.queue.size() + im.pending.size() + im.ready.size();
    }

    // =====================================================================
    // Persistence -- the load half
    // =====================================================================
    void MaterialPreviewHarvester::PrimeFromDisk(const std::vector<Arcane::Guid>& materials)
    {
        Impl& im = *m_impl;
        if (!im.services.resolveAsset)
            return;

        std::size_t loaded = 0, staleOrMissing = 0;
        for (const Arcane::Guid& id : materials)
        {
            if (!id.IsValid())
                continue;
            const std::filesystem::path png = im.ThumbPath(id);
            bool usable = !png.empty();
            if (usable)
            {
                std::error_code ec;
                const auto pngTime = std::filesystem::last_write_time(png, ec);
                if (ec)
                {
                    usable = false;   // no PNG at all
                }
                else if (const auto src = im.services.resolveAsset(id))
                {
                    std::error_code srcEc;
                    const auto srcTime = std::filesystem::last_write_time(*src, srcEc);
                    // A .arcmat NEWER than its PNG means the thumbnail is of
                    // a material that no longer exists. An unreadable source
                    // is not a reason to throw a good PNG away.
                    if (!srcEc && srcTime > pngTime)
                        usable = false;
                }
            }

            if (usable)
            {
                Impl::Entry e;
                // maxSize 64 -- the loader's own cap, matching what the
                // harvest wrote, so a hand-dropped oversized PNG still lands
                // as a 64px thumbnail rather than a surprise upload.
                if (Arcane::LoadDisplayPixels(png, kThumbSize, e.pixels) && e.pixels.Valid())
                {
                    e.thumbId = Arcane::Guid::Generate();
                    im.thumbToMaterial.emplace(e.thumbId, id);
                    im.entries.emplace(id, std::move(e));
                    ++loaded;
                    continue;   // NOT queued: this is the whole point
                }
                usable = false;   // decode failed -- fall through and re-harvest
            }
            ++staleOrMissing;
            im.Push(id);
        }
        ARC_INFO("[thumbs] material previews: {} loaded from disk, {} queued for harvest",
                 loaded, staleOrMissing);
    }

    // =====================================================================
    // The compile half
    // =====================================================================
    void MaterialPreviewHarvester::Impl::Fail(Arcane::Guid id, std::string_view why)
    {
        ARC_WARN("[thumbs] no preview for material {} -- {}", id.ToString(), why);
        failed.insert(id);
        pending.erase(id);
        inFlight.erase(id);
    }

    void MaterialPreviewHarvester::Impl::StartOne(const Arcane::Guid& id, double now)
    {
        if (!services.resolveAsset)
            return Fail(id, "no asset resolver");
        const auto path = services.resolveAsset(id);
        if (!path)
            return Fail(id, "not in the asset registry");
        auto data = Arcane::LoadMaterialAsset(*path);
        if (!data)
            return Fail(id, "asset failed to load");

        // The parent chain, through the SHARED cycle-guarded walker every
        // material cache in the tree uses.
        std::vector<Arcane::MaterialAssetData> chain;
        if (const auto why =
                Arcane::LoadMaterialParentChain(services.resolveAsset, id, data->parent, chain))
            return Fail(id, *why);

        // The BASE's kind, not the leaf's -- an instance carries no kind of
        // its own (the same rule all three engine caches state).
        const Arcane::MaterialAssetData& base = chain.empty() ? *data : chain.back();
        const Arcane::MaterialSurface surface = Arcane::MaterialSurfaceForKind(base.kind);

        // ---- MESH: nothing to compile ----------------------------------
        // A "mesh"-kind .arcmat has no snippet and no template at all
        // (MeshMaterialCache.hpp's own opening), so it resolves
        // SYNCHRONOUSLY into a colour + an optional albedo slot and goes
        // straight to the render queue.
        if (surface == Arcane::MaterialSurface::Mesh)
        {
            // Pump guarantees the vehicle exists before it calls this (the
            // cache is created alongside it), so this is a defensive read
            // rather than a state the queue can sit in.
            if (!meshMats)
                return Fail(id, "the preview vehicle is not available");
            meshMats->Request(id);
            const auto& table = meshMats->Table();
            const auto it = table.find(id);
            if (it == table.end())
                return Fail(id, "mesh material did not resolve");
            Ready r;
            r.id = id;
            r.surface = surface;
            r.meshColor = it->second.baseColor;
            r.meshSlot = it->second.materialSlot;
            ready.push_back(std::move(r));
            return;
        }

        if (!services.compiler || !services.compiler->IsAvailable() || !services.sources)
            return Fail(id, "no shader compiler (dxcompiler.dll) -- material previews need one");

        const auto templateText = services.sources->Get(Arcane::MaterialTemplateFile(surface));
        if (!templateText)
            return Fail(id, "engine material template not found");

        Pending p;
        p.id = id;
        p.surface = surface;

        if (surface == Arcane::MaterialSurface::Sprite)
        {
            Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
                *templateText, chain.empty() ? data->snippet : chain.back().snippet,
                data->name, Arcane::MaterialSurface::Sprite);
            if (!build.errors.empty())
                return Fail(id, "material source errors: " + build.errors.front());

            p.templ = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            p.jobs.resize(1);
            Arcane::ShaderCompileRequest req;
            req.debugName = data->name + ".thumb.sprite.hlsl";
            req.sourceUtf8 = build.hlsl;
            req.entry = Arcane::kPsEntry;
            req.profile = Arcane::kPsProfile;
            req.coalesceKey = ThumbStageKey(id, /*vertex=*/false);
            p.jobs[0].psJob = services.compiler->Submit(req, now);
            req.entry = Arcane::kVsEntry;
            req.profile = Arcane::kVsProfile;
            req.coalesceKey = ThumbStageKey(id, /*vertex=*/true);
            p.jobs[0].vsJob = services.compiler->Submit(std::move(req), now);
        }
        else   // Fullscreen -- the material's OWN pass chain, exactly as the
        {      // shader editor previews it (kSceneInput = the checkerboard).
            std::vector<Arcane::MaterialChainPassDesc> descs;
            descs.reserve(1 + base.passes.size());
            descs.push_back({ base.snippet, base.baseInputs });
            for (const Arcane::MaterialPass& pass : base.passes)
                descs.push_back({ pass.snippet, pass.inputs });

            Arcane::MaterialChainBuildResult build = Arcane::BuildMaterialChainSource(
                *templateText, descs, data->name, base.vertexSnippet, /*externalInput=*/true);
            if (!build.Ok())
            {
                for (const auto& pe : build.passErrors)
                    if (!pe.empty())
                        return Fail(id, "material source errors: " + pe.front());
                return Fail(id, "material source errors: " + build.errors.front());
            }

            p.templ = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            p.passInputs = std::move(build.passInputs);
            p.chainInputSlots = build.chainInputSlots;
            p.jobs.resize(build.hlsl.size());
            for (std::size_t i = 0; i < build.hlsl.size(); ++i)
            {
                Arcane::ShaderCompileRequest req;
                req.debugName = data->name + ".thumb.post.p" + std::to_string(i) + ".hlsl";
                req.sourceUtf8 = build.hlsl[i];
                req.entry = Arcane::kPsEntry;
                req.profile = Arcane::kPsProfile;
                req.coalesceKey = ThumbStageKey(id, /*vertex=*/false, i);
                p.jobs[i].psJob = services.compiler->Submit(req, now);
                req.entry = Arcane::kVsEntry;
                req.profile = Arcane::kVsProfile;
                req.coalesceKey = ThumbStageKey(id, /*vertex=*/true, i);
                p.jobs[i].vsJob = services.compiler->Submit(std::move(req), now);
            }
        }

        p.data = std::move(*data);
        p.parentChain = std::move(chain);
        pending.emplace(id, std::move(p));
    }

    void MaterialPreviewHarvester::OfferCompileResult(const Arcane::ShaderCompileResult& result)
    {
        // NEVER reports a consume: this is a passive observer BESIDE the
        // scene caches on the process's one drain site, not a competitor for
        // their results (its own jobs carry its own coalesce keys, so a
        // result can only ever match one of the two).
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

                const auto& target = im.services.backend == Arcane::GraphicsBackend::Vulkan
                                         ? result.spirv : result.dxil;
                if (!target.succeeded)
                {
                    // No last-good subtlety here, unlike the scene caches: a
                    // thumbnail that cannot compile simply has no picture, and
                    // whatever was already harvested stays in `entries`.
                    im.Fail(p.id, "shader compile failed");
                    return;   // `it` is dangling now -- Fail erased it
                }
                (isVs ? job.vsBytes : job.psBytes) = target.bytecode;
                if (!Impl::AllStagesLanded(p))
                    return;   // the other stages are still in flight
                // Publish must NOT erase the pending node itself (this scope
                // still holds `it` and `p` into it), so it reports a refusal
                // rather than calling Fail -- and the erase order below is
                // then unambiguous: read everything out of `p`, THEN drop it.
                const bool ok = im.Publish(p);
                const Arcane::Guid id = p.id;
                im.pending.erase(it);
                if (!ok)
                    im.Fail(id, "the preview batcher refused the material");
                return;
            }
        }
    }

    bool MaterialPreviewHarvester::Impl::AllStagesLanded(const Pending& p)
    {
        for (const PassJob& j : p.jobs)
            if (j.vsBytes.empty() || j.psBytes.empty())
                return false;
        return true;
    }

    bool MaterialPreviewHarvester::Impl::Publish(Pending& p)
    {
        Ready r;
        r.id = p.id;
        r.surface = p.surface;
        auto inst = LayerParams(p.templ, p.parentChain, p.data);

        if (p.surface == Arcane::MaterialSurface::Sprite)
        {
            if (!batch)
                return false;   // vehicle went away mid-flight
            Arcane::Material2DDesc desc;
            desc.templ = p.templ;
            desc.instance = inst;
            desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
                std::move(p.jobs[0].vsBytes));
            desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
                std::move(p.jobs[0].psBytes));
            // Registered into THIS object's own batcher, never the editor's
            // scene batcher: two owners of one recorder is how two frames'
            // content merges into one (ShaderEditorDocument's preview states
            // the same rule for the same reason). Slots are never reclaimed,
            // and are bounded by "materials thumbed in one session".
            r.spriteMaterial = batch->RegisterMaterial(std::move(desc));
            if (r.spriteMaterial == Arcane::Batcher2D::kInvalidMaterialId)
                return false;
        }
        else
        {
            std::vector<Arcane::PostChainPassDesc> passes;
            passes.reserve(p.jobs.size());
            for (std::size_t i = 0; i < p.jobs.size(); ++i)
            {
                Arcane::PostChainPassDesc bytes;
                bytes.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
                    std::move(p.jobs[i].vsBytes));
                bytes.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
                    std::move(p.jobs[i].psBytes));
                bytes.inputs = p.passInputs[i];
                passes.push_back(std::move(bytes));
            }
            r.post.templ = p.templ;
            r.post.instance = inst;
            r.post.chainInputSlots = p.chainInputSlots;
            r.post.passes = std::move(passes);
        }
        ready.push_back(std::move(r));
        return true;
    }

    // =====================================================================
    // The render half
    // =====================================================================
    bool MaterialPreviewHarvester::Impl::EnsureVehicle(Arcane::NriGraphContext& chrome)
    {
        if (ctx)
            return true;
        if (!services.hostConfig)
            return false;

        // NodeSet{} -- batch + post + tonemap + mesh and nothing else. A
        // thumbnail has no host chrome, no game HUD and nothing to pick.
        ctx = Arcane::NriGraphContext::CreateOffscreen(*services.hostConfig, chrome.Device(),
                                                       kThumbSize, kThumbSize);
        if (!ctx)
        {
            // Degraded, not fatal, and it degrades to what a missing device
            // already degrades to: kind icons instead of pictures. The refusal
            // is already logged + latched inside CreateOffscreen. Every queued
            // material is dropped so this cannot re-attempt every frame.
            ARC_WARN("[thumbs] the material-preview context could not be created "
                     "-- materials keep their kind icon this session");
            // Memoize the refusal across the WHOLE pipeline so this cannot be
            // re-attempted (and re-logged) on every single frame.
            for (const Arcane::Guid& g : inFlight)
                failed.insert(g);
            queue.clear();
            pending.clear();
            ready.clear();
            inFlight.clear();
            return false;
        }

        // The two injected seams, the same pair EditorApp::CreateGraphVehicles
        // and ShaderEditorDocument::EnsureGraphPreviewContext install: a
        // material's texture params are Guids and THIS device has to resolve
        // them itself.
        if (services.resolveAsset)
            ctx->SetAssetResolver(services.resolveAsset);
        if (services.pixelSupply)
            ctx->SetPixelSupply(services.pixelSupply);

        batch = Arcane::Batcher2D::Create();
        if (!batch)
        {
            ARC_WARN("[thumbs] the material-preview batcher could not be created");
            ctx.reset();
            return false;
        }

        // The mesh half: one sphere for the session (the mocks' sphere), and
        // the params-only material cache that colours it.
        sphere = Arcane::BuildUvSphere(0.5f, 24, 32);
        Arcane::MeshMaterialCache::Services ms;
        ms.resolveAsset = services.resolveAsset;
        ms.resolveAlbedoSlot = [this](const Arcane::Guid& g) -> std::uint32_t
        {
            // THIS context's own bindless table -- a slot from another
            // MeshNode's table names a different texture entirely.
            return ctx ? ctx->ResolveMeshAlbedoSlot(g) : 0xFFFFFFFFu;
        };
        meshMats = std::make_unique<Arcane::MeshMaterialCache>(std::move(ms));
        return true;
    }

    bool MaterialPreviewHarvester::Impl::Harvest(Ready& r)
    {
        // Pump has already made both of these true (it creates the vehicle
        // before it dequeues anything); re-read rather than assume.
        Arcane::NriGraphContext* chrome = Chrome();
        if (!chrome || !ctx || !batch)
            return false;

        Arcane::GlobalParams globals;
        globals.time = kThumbTime;
        globals.deltaTime = 0.0f;
        globals.viewportWidth  = static_cast<float>(kThumbSize);
        globals.viewportHeight = static_cast<float>(kThumbSize);

        // ===== THE PICTURE, PER SURFACE ==================================
        // The backdrop is the shader editor preview's checkerboard on every
        // surface -- it is what makes an alpha-cutout sprite material read as
        // a shape rather than as a hole, and on the FULLSCREEN arm it is
        // additionally what kSceneInput samples (one texture, same picture --
        // ShaderEditorDocument::RenderGraphPreview's own reasoning).
        //
        //   Sprite      -> the compiled material on a centred quad.
        //   Fullscreen  -> the material's whole pass chain over the canvas.
        //   Mesh        -> a lit UV sphere in the material's resolved
        //                  baseColor. THE REAL MESH PATH, not the brief's
        //                  sanctioned "same quad" fallback: MeshBuilder::
        //                  BuildUvSphere supplies the geometry, MeshNode is
        //                  built EAGERLY on every vehicle (it is not behind a
        //                  NodeSet flag, and a context whose MeshNode failed
        //                  to create does not exist at all), and
        //                  SceneCamera::PerspectiveProjection supplies the
        //                  lens -- so "trivially reachable" was true, and the
        //                  mocks show spheres.
        Arcane::Batcher2D& b = *batch;
        b.Begin(kThumbSize, kThumbSize);
        b.SetGlobals(globals);   // AFTER Begin -- matching every other call site

        const float extent = static_cast<float>(kThumbSize);
        const glm::vec4 light(0.16f, 0.16f, 0.19f, 1.0f);
        for (int y = 0; y * kCheckerCell < extent; ++y)
            for (int x = 0; x * kCheckerCell < extent; ++x)
                if ((x + y) & 1)
                    b.Rect(glm::vec2(x * kCheckerCell, y * kCheckerCell),
                           glm::vec2(kCheckerCell, kCheckerCell), light);

        if (r.surface == Arcane::MaterialSurface::Sprite &&
            r.spriteMaterial != Arcane::Batcher2D::kInvalidMaterialId)
        {
            const float s = 0.8f * extent;
            b.QuadMaterial(r.spriteMaterial,
                           glm::vec2((extent - s) * 0.5f, (extent - s) * 0.5f),
                           glm::vec2(s, s),
                           glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
        }

        Arcane::NriGraphContext::FrameDesc vp;
        vp.batch   = &b;
        vp.globals = &globals;
        vp.capture = true;   // the readback node -- the whole point of the frame

        std::vector<Arcane::MeshInstance> instances;
        Arcane::MeshSceneDesc meshScene;
        if (r.surface == Arcane::MaterialSurface::Fullscreen && !r.post.passes.empty())
        {
            vp.post = &r.post;
        }
        else if (r.surface == Arcane::MaterialSurface::Mesh && !sphere.vertices.empty())
        {
            Arcane::MeshInstance mi;
            mi.mesh = &sphere;
            mi.model = glm::mat4(1.0f);
            mi.baseColor = r.meshColor;
            mi.materialSlot = r.meshSlot;
            instances.push_back(mi);

            meshScene.instances = instances;
            // Right-handed, [0,1] depth -- SceneCamera's own convention, and
            // the only one MeshNode's shaders are built for. 2.0m back from a
            // 0.5m-radius sphere at 35 degrees fills ~78% of the tile.
            meshScene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f),
                                           glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            meshScene.projection = Arcane::PerspectiveProjection(35.0f, 1.0f, 0.05f, 10.0f);
            meshScene.lightDirection = glm::vec3(0.45f, 0.7f, 0.8f);   // TOWARD the light
            meshScene.lightColor = glm::vec3(1.0f);
            meshScene.ambient = glm::vec3(0.12f);
            vp.mesh = &meshScene;
        }

        if (ctx->RenderFrameOffscreen(vp) != Arcane::NriGraphContext::FrameOutcome::Presented)
        {
            // Skipped is impossible at a fixed 64px; Failed has already
            // latched + logged. Either way this material gets no picture, and
            // the vehicle is dropped rather than spending a frame's worth of
            // validation errors per thumbnail for the rest of the session --
            // the same posture ShaderEditorDocument takes for its preview.
            ARC_ERROR("[thumbs] the preview frame for material {} failed "
                      "-- dropping the preview vehicle", r.id.ToString());
            ctx.reset();
            batch.reset();
            meshMats.reset();
            return false;
        }

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        // TIGHT RGBA8, already swizzled out of the output's BGRA. A false is
        // NOT a run failure (ReadCapture's own contract) -- just no picture.
        if (!ctx->ReadCapture(w, h, rgba) || w == 0 || h == 0)
            return false;

        // ===== THE CACHE SWAP, AND WHAT A RE-HARVEST OWES ================
        Entry& e = entries[r.id];
        if (!e.thumbId.IsValid())
        {
            // A SYNTHETIC guid, the toolbar mark's own idiom: this image is
            // not a project asset, and the chrome cache's whole vocabulary is
            // Guids. Generated so it cannot collide with a real asset id.
            e.thumbId = Arcane::Guid::Generate();
            thumbToMaterial.emplace(e.thumbId, r.id);
        }
        if (e.texture != 0)
        {
            // RE-HARVEST. NriTextureCache::Invalidate destroys the texture and
            // the next Resolve creates a replacement -- and NRI does not
            // ref-count, so that replacement may land on the address the old
            // one just vacated. ImGuiNri caches per texture by RAW POINTER, so
            // a bit-identical id would report a cache HIT on a descriptor set
            // naming the DESTROYED texture (NriGraphContext.hpp, item (2)
            // under TWO CONTEXTS TWO LANES).
            //
            // BEFORE, and unconditionally rather than "if the pointer changed"
            // -- an unchanged pointer is precisely the case a recycled address
            // fakes. NOTHING RENDERS BETWEEN THE TWO CALLS: both run inside
            // one Pump, at phase 13, strictly after this frame's render and
            // strictly before the next one's.
            if (Arcane::ImGuiNriNode* hud = chrome->ImGuiHud())
                hud->InvalidateUserTextureNow(
                    reinterpret_cast<nri::Texture*>(static_cast<std::intptr_t>(e.texture)));
            chrome->InvalidateContentTexture(e.thumbId);
            e.texture = 0;
        }

        e.pixels.width = w;
        e.pixels.height = h;
        e.pixels.rgba = rgba;   // copied: the PNG writer below consumes the other one
        EnsureTexture(r.id, e);

        // ===== PERSIST (the UE lesson) ==================================
        // maxWidth 0: these bytes are ALREADY the thumbnail size, and the
        // writer's cap is a downscale, not a target.
        if (const std::filesystem::path png = ThumbPath(r.id); !png.empty())
            (void)Arcane::WriteThumbnailPngRgba(png, w, h, std::move(rgba), 0);

        return true;
    }

    std::uint64_t MaterialPreviewHarvester::Impl::EnsureTexture(const Arcane::Guid& material,
                                                                Entry& e) const
    {
        if (e.texture != 0)
            return e.texture;
        if (!e.pixels.Valid() || !e.thumbId.IsValid())
            return 0;
        Arcane::NriGraphContext* chrome = Chrome();
        Arcane::NriTextureCache* cache = chrome ? chrome->Textures() : nullptr;
        if (!cache)
            return 0;   // no device yet (a PrimeFromDisk during boot) -- retried
        // ColorSpace::Display is load-bearing, exactly as it is for the
        // toolbar mark: ImGui draws AFTER the tonemap, and an sRGB view under
        // it decodes a second time and reads dark. These bytes are already
        // display-referred -- they came out of the tonemap.
        nri::Texture* t = cache->Resolve(e.thumbId, Arcane::NriTextureCache::ColorSpace::Display);
        if (!t)
        {
            ARC_WARN("[thumbs] the chrome texture cache refused the preview for material {}",
                     material.ToString());
            return 0;
        }
        e.texture = static_cast<std::uint64_t>(reinterpret_cast<std::intptr_t>(t));
        return e.texture;
    }

    // =====================================================================
    // The pump
    // =====================================================================
    void MaterialPreviewHarvester::Pump(double now)
    {
        Impl& im = *m_impl;

        // ===== THE EARLY-OUT, and it is the whole performance contract ===
        // Nothing rendered and nothing to start means NOTHING is touched: no
        // device, no fence, no allocation, no cache lookup beyond these two
        // emptiness tests. `pending` deliberately does NOT keep this alive --
        // a compile in flight is the compiler's business and lands through
        // OfferCompileResult, not through a poll here.
        if (im.ready.empty() && im.queue.empty())
            return;

        // ===== THE VEHICLE, LAZILY, ONCE ================================
        // Not "per harvest" and not "at construction": the first material
        // that actually wants a picture is what pays for the offscreen
        // context, the batcher, the sphere and the mesh-material cache. A
        // null chrome (boot has not reached CreateGraphVehicles, or a run
        // that never built a vehicle) leaves EVERYTHING queued rather than
        // failing it -- nothing about a missing device says the material is
        // un-previewable.
        Arcane::NriGraphContext* chrome = im.Chrome();
        if (!chrome || !im.EnsureVehicle(*chrome))
            return;

        // ===== ONE HARVEST PER PUMP =====================================
        // ReadCapture idles the whole device, so this is capped at one --
        // the same "rare event pays for the DeviceWaitIdle" reasoning
        // EditorApp::CaptureGraphViewportPng states for the cover screenshot.
        // LIFO: the newest ready entry is the one whose Request came from the
        // most recent visible draw.
        if (!im.ready.empty())
        {
            Impl::Ready r = std::move(im.ready.back());
            im.ready.pop_back();
            im.inFlight.erase(r.id);
            if (!im.Harvest(r))
            {
                // Harvest already logged whatever went wrong. Memoize the
                // refusal so a broken material cannot idle the device once a
                // frame forever.
                im.failed.insert(r.id);
            }
            else
            {
                ARC_INFO("[thumbs] harvested a 64px preview for material {} ({} left)",
                         r.id.ToString(), PendingCount());
            }
            return;   // one device idle per frame, no matter what else is due
        }

        // ===== OTHERWISE START ONE (pure CPU: a JSON load + a DXC submit) =
        // It stays in `inFlight` across the move from `queue` to `pending` /
        // `ready`, so Request() cannot re-push it and nothing is ever lost
        // between the two containers.
        const Arcane::Guid id = im.queue.front();
        im.queue.pop_front();
        im.StartOne(id, now);
    }

    // =====================================================================
    // Readers
    // =====================================================================
    std::uint64_t MaterialPreviewHarvester::ThumbTextureId(const Arcane::Guid& material) const
    {
        Impl& im = *m_impl;   // the pimpl loophole: `const` here is the
                              // CALLER's contract (a query), not a promise
                              // that the lazy upload below cannot happen.
        const auto it = im.entries.find(material);
        if (it == im.entries.end())
            return 0;
        return im.EnsureTexture(material, it->second);
    }

    const Arcane::PixelData* MaterialPreviewHarvester::PixelsForThumb(
        const Arcane::Guid& id) const
    {
        const Impl& im = *m_impl;
        const auto m = im.thumbToMaterial.find(id);
        if (m == im.thumbToMaterial.end())
            return nullptr;
        const auto e = im.entries.find(m->second);
        if (e == im.entries.end() || !e->second.pixels.Valid())
            return nullptr;
        return &e->second.pixels;
    }
}
