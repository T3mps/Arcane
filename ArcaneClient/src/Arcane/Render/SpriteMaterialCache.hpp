#pragma once

// SpriteMaterialCache (shader-editor Slice 8): resolves SAVED .arcmat material
// assets into registered Batcher2D materials for in-scene sprites. The host
// (Arcane Editor today; the .arcproj runtime-host later) asks Request() for
// every material Guid its sprites reference; the cache loads the SAVED asset
// (never a document's working copy -- the Slice-8 invariant), resolves the
// instance parent chain, stitches the SPRITE template, and submits both stages
// through the app-shared ShaderCompiler. The host's ONE drain site offers every
// result via ConsumeResult; on a completed pair the material is (re)registered
// with the frame batcher and Table() maps the Guid to its id -- publish that
// through the SpriteMaterialTable registry resource. Unresolved/failed
// materials simply stay off the table (sprites draw through the plain
// pipeline), and a failed RE-compile keeps the previous registration bound
// (last-good, same contract as the editor preview).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>


#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Arcane
{
    class Assets;
    class Batcher2D;
    class ShaderCompiler;
    class ShaderSourceProvider;
    struct ShaderCompileResult;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD
#endif
    class ARCANE_API SpriteMaterialCache
    {
    public:
        using ResolveAssetFn =
            std::function<std::optional<std::filesystem::path>(const Guid&)>;

        struct Services
        {
            ShaderCompiler*       compiler = nullptr;
            ShaderSourceProvider* sources = nullptr;   // sprite template text
            // UNUSED: Bind() touches no device and no image, so nothing
            // reads this. Kept only because Bind() may yet want a device-free
            // asset read.
            Assets*               assets = nullptr;    // (unused)
            GraphicsBackend       backend{};
            ResolveAssetFn        resolveAsset;        // Guid -> path (project registry)
        };

        explicit SpriteMaterialCache(Services services);
        ~SpriteMaterialCache();
        SpriteMaterialCache(const SpriteMaterialCache&) = delete;
        SpriteMaterialCache& operator=(const SpriteMaterialCache&) = delete;

        // Ensure a compile is in flight (or done/failed) for `id`. Call per
        // frame per referenced material -- no-op once known. `now` feeds the
        // compile service's debounce clock.
        void Request(const Guid& id, double now);

        // Asset re-saved: re-resolve on the next Request. The existing
        // registration stays bound (last-good) until the fresh compile lands
        // through UpdateMaterial.
        void Invalidate(const Guid& id);

        // Offer a drained compile result; true when it belonged to this cache.
        bool ConsumeResult(const ShaderCompileResult& result, Batcher2D& batcher);

        // Guid -> registered Batcher2D material id (the SpriteMaterialTable
        // payload). Stable storage between mutating calls.
        const std::unordered_map<Guid, std::uint16_t>& Table() const;

        // Forget everything (project switch). Batcher slots are not reclaimed
        // (its table only grows) -- bounded by materials touched per session.
        void Clear();

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
