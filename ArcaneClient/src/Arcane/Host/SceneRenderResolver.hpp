#pragma once

// SceneRenderResolver: the per-frame bridge between what a scene REFERENCES
// (asset Guids on components) and what the submission path can DRAW (resolved
// tables + a bound post chain). One call per frame, from every host.
//
// WHY THIS EXISTS. RenderSubmissionSystem (Scene/RenderSystems.hpp) never
// touches the Assets facade by design: it reads two pre-resolved lookup tables
// the host must publish every frame (SceneResources.hpp:4 states the contract).
// Until 2026-07-29 only the Arcane Editor did -- the resolution cache itself
// lived in ArcaneEditor/src/SpriteCache.hpp -- so the standalone runtime host
// structurally could not draw a textured sprite whatever the scene contained,
// and neither could it bind a sprite material or a post chain. That is the
// second capability in one evening that the editor grew and the standalone host
// never got (the first was the camera, now Scene/SceneCamera.hpp). The fix
// under the homogenized-rendering directive is not a second cache in the
// runtime: it is ONE engine-side implementation both hosts drive.
// Brief: docs/superpowers/plans/2026-07-29-sprite-resolution-lift.md
// Design: docs/superpowers/specs/2026-07-29-sprite-resolution-lift-design.md
//
// WHAT IT OWNS. The three caches that turn a Guid into something bindable --
// SpriteCache (.arcsprite -> texture/UVs/size/pivot), SpriteMaterialCache
// (.arcmat -> a registered Batcher2D material id) and PostChainCache (.arcmat ->
// a bound FullscreenMaterialChain). They share one compile drain site, which is
// why they belong to one owner: splitting them would mean a second Drain(), and
// ShaderCompiler::Drain is the ONE place a compile result may become an NVRHI
// shader (ShaderCompiler.hpp:9-13).
//
// WHAT IT DOES NOT OWN. The camera (a pure sweep -- Scene/SceneCamera.hpp), the
// batcher, the device, and the compile service: all injected. It never renders.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Render/Device.hpp>   // GraphicsBackend (by value in Services)

#include <functional>

namespace Arcane
{
    class Batcher2D;
    class FullscreenMaterialChain;
    class MaterialInstance;
    class Runtime;
    class ShaderCompiler;
    class ShaderSourceProvider;
    struct PostChainDesc;
    struct ShaderCompileResult;

    class ARCANE_API SceneRenderResolver
    {
    public:
        struct Services
        {
            // The runtime whose Registry is swept and whose SpriteTable /
            // SpriteMaterialTable resources are published. Required.
            Runtime* runtime = nullptr;

            // The frame batcher registered materials bind into and evicted
            // sprite textures are dropped from. Null (a headless host or a
            // test) disables material binding; sprite resolution still works.
            Batcher2D* batcher = nullptr;

            nvrhi::IDevice* device = nullptr;   // createShader, at the drain site
            GraphicsBackend backend{};

            // The app-shared compile service. Null, or an unavailable one (no
            // dxcompiler.dll), disables materials AND the post chain -- but NOT
            // sprite resolution, which is a synchronous JSON+texture load with
            // no compile step. That distinction is load-bearing: gating the
            // sprite sweep on the compiler is exactly the bug the sprite arc's
            // F1 fix removed (a DXC-less machine drew every sprite as an
            // indistinguishable untextured 1x1 quad, with no diagnostic).
            ShaderCompiler*       compiler = nullptr;
            ShaderSourceProvider* sources  = nullptr;

            // Offered every drained compile result BEFORE the caches; true means
            // consumed. This is how the process keeps exactly ONE drain site
            // while the editor still routes results to its open shader
            // documents. Hosts with no documents leave it unset.
            std::function<bool(const ShaderCompileResult&)> consumeFirst;
        };

        // This frame's facts. `now` is the compile service's clock (monotonic
        // seconds); viewport extent sizes the material globals.
        struct FrameInfo
        {
            double now = 0.0;
            double dt  = 0.0;
            float  viewportWidth  = 0.0f;
            float  viewportHeight = 0.0f;
        };

        explicit SceneRenderResolver(Services services);

        // Publishes nulls through the Runtime, because the registry holds
        // NON-OWNING pointers to this object's maps (SceneResources.hpp:96-99).
        // CONTRACT for every host: declare the resolver so it destructs BEFORE
        // the Runtime (whose registry would otherwise be left pointing at freed
        // maps) and before the render device (this holds nvrhi keep-alive
        // texture handles).
        ~SceneRenderResolver();

        SceneRenderResolver(const SceneRenderResolver&) = delete;
        SceneRenderResolver& operator=(const SceneRenderResolver&) = delete;

        // THE per-frame call. Sweep -> request -> pump -> latch -> publish, in
        // that order (see the .cpp for why each step sits where it does).
        //
        // Call it BEFORE the frame's Batcher2D::Begin and before
        // RunLoop::SubmitRender: the drain REGISTERS materials with the batcher
        // (pipeline + binding-set table mutation), which does not belong inside
        // a Begin/End recording, and the tables it publishes are what that
        // frame's submission reads.
        void Refresh(const FrameInfo& frame);

        // Engine-global material constants for THIS frame (Time/Delta/Viewport).
        // Feed Batcher2D::SetGlobals with it after Begin -- registered sprite
        // materials read zeros otherwise (built-ins ignore it).
        const GlobalParams& Globals() const;

        // The scene's bound post chain, or null for "no chain" (the plain
        // canvas->tonemap path). Re-read every frame AFTER Refresh: a drain may
        // swap the bound instance under an asset re-save.
        FullscreenMaterialChain* PostChain() const;
        const MaterialInstance*  PostInstance() const;

        // The SAME bound chain as bytecode + layout + values, for a recorder
        // that is not on this resolver's nvrhi device -- the `--nri-graph`
        // vehicle's post nodes (NRI Phase 2, Task 10; see PostChainDesc in
        // Render/PostChainCache.hpp). Null for "no chain", and latched beside
        // PostChain()/PostInstance() in the same Refresh step, so all three
        // describe one compile. Re-read every frame AFTER Refresh, for the
        // same reason the other two are.
        const PostChainDesc* PostDesc() const;

        // What the scene REFERENCES versus what is bound right now (NRI Phase 2).
        //
        // Materials bind asynchronously -- Refresh submits the compiles and a
        // LATER Refresh drains whatever the compile worker finished in the
        // meantime -- so "the scene declares a material" and "this frame can
        // draw it" are different facts, separated by WALL-CLOCK time. For an
        // interactive host that gap is invisible (a material appears a few
        // frames in and nobody minds). For the golden harness it is the whole
        // ballgame: a captured frame missing its materials is not a slower
        // frame, it is a picture of DIFFERENT CONTENT, and freezing that as a
        // baseline poisons every compare made against it afterwards.
        //
        // So this is the probe a host uses to answer "is what I am about to
        // capture actually the scene?" -- read it AFTER draining the compile
        // service to idle. Deliberately computed on demand (it re-walks the
        // SpriteRenderer and PostProcess views) rather than accumulated every
        // Refresh: only the golden path calls it, and no ordinary frame should
        // pay for it. Both halves read the LIVE registry against the LIVE
        // caches, so the answer never depends on whether a Refresh has run --
        // a resolver that has never swept simply reports everything unbound.
        //
        // `spriteReferenced` counts sprite COMPONENTS carrying a valid material
        // Guid, not distinct Guids -- "every referencing sprite can draw its
        // material" is the property a host actually wants, and it stays correct
        // when two sprites share one material.
        struct MaterialCensus
        {
            int  spriteReferenced = 0;       // SpriteRenderers with a valid material Guid
            int  spriteBound      = 0;       // ...whose material is registered with the batcher
            bool postReferenced   = false;   // a PostProcess assignment was found
            bool postBound        = false;   // ...and its chain is bound AND ready
        };
        MaterialCensus Materials() const;

        // A .arcsprite was re-saved: evict, then re-resolve SYNCHRONOUSLY, so no
        // frame can render the placeholder in the gap. (SpriteCache::Request has
        // no async step, so this costs one JSON load. The material caches get
        // the same no-gap property from their last-good scheme instead.)
        void InvalidateSprite(const Guid& id);

        // A .arcmat was re-saved: re-resolve on the next Refresh. Hits BOTH the
        // sprite-material and post caches -- one Guid cannot be known to be
        // only one of the two, and a wrong guess silently strands the other.
        void InvalidateMaterial(const Guid& id);

        // Project switch: forget everything. A Guid resolves through the CURRENT
        // project's registry, so a cached entry may resolve to something else
        // entirely (or nothing) once the project changes.
        void Clear();

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
