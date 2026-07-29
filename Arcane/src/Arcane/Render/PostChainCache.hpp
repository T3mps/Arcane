#pragma once

// PostChainCache (post arc, slice 2): resolves SAVED .arcmat material assets
// into bound FullscreenMaterialChains for the scene post-processing hook
// (OffscreenCanvas::SetPostChain; the ArcaneRuntime host's inline site). It is
// SpriteMaterialCache's twin -- same Services, same Request / Invalidate /
// ConsumeResult / Clear lifecycle, same SAVED-asset + parent-chain resolution
// -- but the build is BuildMaterialChainSource in POST mode (kSceneInput
// entries are valid: the material may read the scene color), every pass's
// stages compile through the app-shared ShaderCompiler, and a completed set
// binds through the chain's atomic SetChain, so a failed RE-compile keeps the
// previous chain rendering (last-good). Unresolved / failed materials simply
// never expose a chain (the host's no-chain path is byte-identical to today).
//
// The host sweep (slice 3) Request()s the assigned material every frame and
// hands Chain()/Instance() to the canvas hook AFTER the drain site runs --
// a consumed result may swap the bound instance, so the pointers must be
// re-fetched each frame, never cached across a drain.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/Device.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace Arcane
{
    class Assets;
    class FullscreenMaterialChain;
    class MaterialInstance;
    class ShaderCompiler;
    class ShaderSourceProvider;
    struct ShaderCompileResult;

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
            nvrhi::IDevice*       device = nullptr;    // createShader (drain site)
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
        // stays rendering (last-good) until the fresh compile lands through
        // SetChain.
        void Invalidate(const Guid& id);

        // Offer a drained compile result; true when it belonged to this cache.
        bool ConsumeResult(const ShaderCompileResult& result);

        // The bound chain / merged instance for `id` -- null until the first
        // successful bind. Valid until the next mutating call (a drain may
        // swap the instance); re-fetch every frame before handing them to the
        // canvas hook. Bind time also warms the instance's texture params
        // through Assets, honoring FullscreenMaterialPass::Render's pre-load
        // contract for hosts that Draw() right after the drain.
        FullscreenMaterialChain* Chain(const Guid& id) const;
        const MaterialInstance*  Instance(const Guid& id) const;

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
