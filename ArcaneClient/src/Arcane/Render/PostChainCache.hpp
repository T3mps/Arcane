#pragma once

// PostChainCache (post arc, slice 2): resolves SAVED .arcmat material assets
// into a bound scene post chain, published as PostChainDesc bytes for the
// graph recorder's PostChainNode (Render/Nri/nodes/FullscreenNodes.*).
// It is SpriteMaterialCache's twin -- same Services, same Request / Invalidate
// / ConsumeResult / Clear lifecycle, same SAVED-asset + parent-chain
// resolution -- but the build is BuildMaterialChainSource in POST mode
// (kSceneInput entries are valid: the material may read the scene color),
// every pass's stages compile through the app-shared ShaderCompiler, and a
// completed set binds atomically, so a failed RE-compile keeps the previous
// bytes published (last-good). Unresolved / failed materials simply never
// expose a chain.
//
// The host sweep (slice 3) Request()s the assigned material every frame and
// reads Desc()/Instance() AFTER the drain site runs -- a consumed result may
// swap the bound instance, so the pointers must be re-fetched each frame,
// never cached across a drain.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>


#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Arcane
{
    class Assets;
    class MaterialInstance;
    class MaterialTemplate;
    class ShaderCompiler;
    class ShaderSourceProvider;
    struct ShaderCompileResult;

    // =====================================================================
    // THE COMPILED CHAIN AS BYTES -- the twin of Material2DDesc::vsBytes/
    // psBytes (Batcher2D.hpp), and it exists for the identical reason: a
    // device can build a pipeline from bytecode but cannot adopt a compiled
    // shader OBJECT it did not create. The graph's PostChainNode builds its
    // own NRI pipelines from exactly these bytes, so a recompile and a render
    // always agree about which shader ran.
    //
    // ONE target, not both: the producer already picks dxil-or-spirv by the
    // process's GraphicsBackend and the render vehicle runs on that same
    // backend (RuntimeApp builds both from one HostConfig).
    //
    // THE ONLY COMPILED FORM this cache publishes.
    // =====================================================================
    struct PostChainPassDesc
    {
        std::shared_ptr<const std::vector<std::uint8_t>> vsBytes;
        std::shared_ptr<const std::vector<std::uint8_t>> psBytes;
        // Chain indices feeding this pass's InputTexture(N) slots, in slot
        // order: an EARLIER pass index, or kSceneInput (MaterialSource.hpp)
        // for the external scene colour. Already validated by
        // BuildMaterialChainSource, so a real index is always < this pass's.
        std::vector<std::uint32_t> inputs;
    };

    struct PostChainDesc
    {
        // The ONE merged layout + values every pass shares
        // (BuildMaterialChainSource's contract), so one packed CB and one
        // texture table serve the whole chain.
        std::shared_ptr<const MaterialTemplate> templ;
        std::shared_ptr<const MaterialInstance> instance;
        // The UNIFORM InputTexture decl count every pass's source carries
        // (MaterialChainBuildResult::chainInputSlots) -- the binding layout's
        // shape, not any one pass's wiring.
        std::uint32_t chainInputSlots = 1;
        std::vector<PostChainPassDesc> passes;   // chain order; passes[0] is the base
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD
#endif
    class ARCANE_API PostChainCache
    {
    public:
        using ResolveAssetFn =
            std::function<std::optional<std::filesystem::path>(const Guid&)>;

        struct Services
        {
            ShaderCompiler*       compiler = nullptr;
            ShaderSourceProvider* sources = nullptr;   // fullscreen template text
            // `assets` is VESTIGIAL: there is no bind-time texture warm-up
            // for it to serve -- the graph makes the same Guids resident on
            // its own device through NriTextureCache, which is where
            // residency belongs. Left in place because Bind() may yet want a
            // device-free asset read; nothing reads it today.
            Assets*               assets = nullptr;    // (unused)
            GraphicsBackend       backend{};
            ResolveAssetFn        resolveAsset;        // Guid -> path (project registry)
        };

        explicit PostChainCache(Services services);
        ~PostChainCache();
        PostChainCache(const PostChainCache&) = delete;
        PostChainCache& operator=(const PostChainCache&) = delete;

        // Ensure a compile is in flight (or done/failed) for `id`. Call per
        // frame for the swept material -- no-op once known. `now` feeds the
        // compile service's debounce clock.
        void Request(const Guid& id, double now);

        // Asset re-saved: re-resolve on the next Request. The bound chain
        // stays rendering (last-good) until the fresh compile lands.
        void Invalidate(const Guid& id);

        // Offer a drained compile result; true when it belonged to this cache.
        bool ConsumeResult(const ShaderCompileResult& result);

        // The merged instance for `id` -- null until the first successful
        // bind. Valid until the next mutating call (a drain may swap the
        // instance); re-fetch every frame before handing it to a caller.
        // Bind time does NOT warm the instance's texture params: texture
        // residency for the chain is the graph recorder's job, on its own
        // device, through NriTextureCache.
        const MaterialInstance*  Instance(const Guid& id) const;

        // The bound chain expressed as BYTECODE + layout + values, for the
        // `--nri-graph` recorder (see PostChainDesc above). Null until the
        // first successful bind, and it tracks Instance() exactly: both are
        // set in the same breath and a failed RE-compile leaves both at
        // last-good. Same validity window as Instance(): until the next
        // mutating call.
        const PostChainDesc* Desc(const Guid& id) const;

        // Forget everything (project switch).
        void Clear();

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
