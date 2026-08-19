#pragma once

// SpriteCache: resolves .arcsprite Guids referenced by SpriteRenderer::sprite
// into Arcane::SpriteEntry records for the scene's SpriteTable. Mirrors
// SpriteMaterialCache's host integration (SpriteMaterialCache.hpp) minus
// compilation -- there is no async compile step here, so Request() resolves
// synchronously in one call: load the .arcsprite JSON (LoadSpriteAsset), read
// the source image's pixel dimensions through Assets::PixelsFor (a cached,
// DEVICE-FREE facade), and compute UVs/size via ComputeSpriteGeom against
// them. The resolved record names its texture by Guid only -- residency on the
// GPU is NriTextureCache's job, not this cache's. A failed resolve
// (unresolvable Guid, missing file, bad JSON) caches a DEFAULT SpriteEntry
// (1x1 m untextured placeholder) directly in the PUBLISHED table --
// deliberately different from SpriteMaterialCache::Request, which keeps
// failures OUT of its published table in a separate `failed` set
// (SpriteMaterialCache.cpp:47,95-101) so a failed material just leaves the
// sprite on the plain pipeline. A sprite has no such implicit fallback: the
// whole point of the component is to draw a specific asset, so a broken one
// must still be VISIBLE as the placeholder rather than silently vanishing.
// Both schemes share the same never-re-parsed-once-known effect (a
// table.contains(id) guard either way), just achieved with different
// structures.
//
// ENGINE-SIDE, not editor-side (sprite-resolution lift, 2026-07-29): it lived
// in ArcaneEditor for the whole sprite arc, which is exactly why the
// standalone runtime could not draw a textured sprite -- see
// docs/superpowers/plans/2026-07-29-sprite-resolution-lift.md. Both hosts now
// drive ONE implementation through SceneRenderResolver (Arcane/Host).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // Arcane::SpriteEntry (full type: Table()'s value type)

#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Arcane
{
    class Assets;
    class Batcher2D;

    class ARCANE_API SpriteCache
    {
    public:
        // Guid-shaped, matching SpriteMaterialCache/PostChainCache exactly, so
        // one host service can hand all three caches the same resolver (the
        // AssetId round-trip happens inside).
        using ResolveAssetFn =
            std::function<std::optional<std::filesystem::path>(const Guid&)>;

        struct Services
        {
            Assets*        assets  = nullptr;   // PixelsFor(Guid) -- image dimensions
            Batcher2D*     batcher = nullptr;   // RemoveTexture on eviction (inert; see the .cpp)
            ResolveAssetFn resolveAsset;        // Guid -> path (project registry)
        };

        explicit SpriteCache(Services services);
        ~SpriteCache();
        SpriteCache(const SpriteCache&) = delete;
        SpriteCache& operator=(const SpriteCache&) = delete;

        // Ensure `id` is resolved (or known-failed) -- idempotent, a no-op once
        // known (same per-frame sweep contract as SpriteMaterialCache::Request,
        // SpriteMaterialCache.hpp:62-65). Call per frame per referenced sprite Guid.
        void Request(const Guid& id);

        // Asset re-saved / removed: drop the table entry so the next Request
        // re-resolves from disk. The keep-alive map this also used to clear is
        // gone (NRI Phase 5a, Task 7 -- see SpriteCache.cpp), and the
        // Batcher2D::RemoveTexture eviction it performed first is now inert
        // because no SpriteEntry carries a texture object any more.
        void Invalidate(const Guid& id);

        // Forget everything (project switch): a Guid resolves through the
        // CURRENT project's registry, so a cached entry from the outgoing
        // project may resolve to something else entirely (or nothing) once the
        // project changes -- same rationale as SpriteMaterialCache::Clear()
        // (SpriteMaterialCache.hpp:79-81).
        void Clear();

        // Guid -> resolved record (the SpriteTable payload). Stable storage
        // between mutating calls.
        const std::unordered_map<Guid, SpriteEntry>& Table() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
