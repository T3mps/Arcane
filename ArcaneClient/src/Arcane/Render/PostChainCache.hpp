#pragma once

// PostChainCache (post arc, slice 2): resolves SAVED .arcmat material assets
// into a bound scene post chain, published as PostChainDesc bytes for the
// `--nri-graph` recorder's PostChainNode (Render/Nri/nodes/FullscreenNodes.*).
// It is SpriteMaterialCache's twin -- same Services, same Request / Invalidate
// / ConsumeResult / Clear lifecycle, same SAVED-asset + parent-chain
// resolution -- but the build is BuildMaterialChainSource in POST mode
// (kSceneInput entries are valid: the material may read the scene color),
// every pass's stages compile through the app-shared ShaderCompiler, and a
// completed set binds atomically, so a failed RE-compile keeps the previous
// bytes published (last-good). Unresolved / failed materials simply never
// expose a chain.
//
// NRI Phase 5a, Task 4: this cache used to ALSO build an NVRHI
// FullscreenMaterialChain (Chain()/Instance() fed OffscreenCanvas::SetPostChain
// on the editor's NVRHI arm and ArcaneRuntime's RenderNvrhi). Both callers are
// gone -- the graph recorder is the only render path left -- so that half was
// deleted; Desc()/Instance() below are what remains, and what every caller
// already used on the graph arm.
//
// The host sweep (slice 3) Request()s the assigned material every frame and
// reads Desc()/Instance() AFTER the drain site runs -- a consumed result may
// swap the bound instance, so the pointers must be re-fetched each frame,
// never cached across a drain.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>

#include <nvrhi/nvrhi.h>   // nvrhi::IDevice* in Services -- reached through Render/Device.hpp until Task 8b deleted it

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
    // THE COMPILED CHAIN AS BYTES (NRI Phase 2, Task 10) -- the twin of
    // Material2DDesc::vsBytes/psBytes (Batcher2D.hpp, Task 9), and it exists
    // for the identical reason: a SECOND graphics device in the same process
    // cannot use an nvrhi::ShaderHandle, only the bytecode behind it. The
    // graph vehicle's PostChainNode builds its own NRI pipelines from exactly
    // these bytes, so both recorders run the SAME stitched shaders rather
    // than two independently compiled ones.
    //
    // ONE target, not both: the producer already picks dxil-or-spirv by the
    // process's GraphicsBackend and the graph vehicle runs on that same
    // backend (RuntimeApp builds both from one HostConfig).
    //
    // NRI Phase 5a, Task 4: this is now the ONLY compiled form PostChainCache
    // publishes -- the NVRHI FullscreenMaterialChain it used to build BESIDE
    // these bytes is gone, along with both its callers.
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
            Assets*               assets = nullptr;    // texture warm-up at bind
            // Gates the texture warm-up below (Assets::GetTexture at bind
            // time). Always null since NRI Phase 5a, Task 2b's flip -- both
            // hosts have passed nullptr since the graph recorder became the
            // only render path, and the graph makes its own textures resident
            // through NriTextureCache instead. NRI Phase 5a, Task 4 deleted
            // the NVRHI chain this field used to also gate (see Bind()'s own
            // history note); the field itself is kept rather than deleted:
            // it mirrors SpriteMaterialCache::Services::device, and
            // collapsing device-gated plumbing repo-wide is Task 11's job,
            // not this one's.
            nvrhi::IDevice*       device = nullptr;
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
        // instance); re-fetch every frame before handing it to a caller. Bind
        // time also warms the instance's texture params through Assets when
        // `Services::device` is set (see its own comment -- always null today).
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
