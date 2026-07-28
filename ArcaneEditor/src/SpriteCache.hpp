#pragma once

// SpriteCache (sprite-asset arc, Task 3): resolves .arcsprite Guids
// referenced by SpriteRenderer::sprite into Arcane::SpriteEntry records for
// the runtime's SpriteTable. Mirrors SpriteMaterialCache's host integration
// (Arcane/Render/SpriteMaterialCache.hpp/.cpp) minus compilation -- there is
// no async compile step here, so Request() resolves synchronously in one
// call: load the .arcsprite JSON (LoadSpriteAsset), resolve its texture Guid
// through Assets::GetTexture (a cached facade, synchronous -- Assets.hpp:
// 79-81), and compute UVs/size via ComputeSpriteGeom against the texture's
// actual pixel dimensions. A failed resolve (unresolvable Guid, missing
// file, bad JSON) caches a DEFAULT SpriteEntry (1x1 m untextured
// placeholder) directly in the PUBLISHED m_table -- deliberately different
// from SpriteMaterialCache::Request, which keeps failures OUT of its
// published table in a separate `failed` set (SpriteMaterialCache.cpp:
// 47,95-101) so a failed material just leaves the sprite on the plain
// pipeline. A sprite has no such implicit fallback: the whole point of the
// component is to draw a specific asset, so a broken one must still be
// VISIBLE as the placeholder rather than silently vanishing. Both schemes
// share the same never-re-parsed-once-known effect (m_table.contains(id)
// guards Request either way), just achieved with different structures.

#include <Arcane/Guid.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // Arcane::SpriteEntry (full type: Table()'s value type)

#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Arcane
{
    class Assets;
    class Batcher2D;
    class AssetId;
}

namespace Arcane::Editor
{
    class SpriteCache
    {
    public:
        struct Services
        {
            Arcane::Assets*    assets  = nullptr;              // GetTexture(AssetId)
            Arcane::Batcher2D* batcher = nullptr;               // RemoveTexture on eviction
            std::function<std::optional<std::filesystem::path>(const Arcane::AssetId&)> resolveAsset;
        };

        explicit SpriteCache(Services services);

        // Ensure `id` is resolved (or known-failed) -- idempotent, a no-op
        // once known (same per-frame sweep contract as
        // SpriteMaterialCache::Request, SpriteMaterialCache.hpp:62-65). Call
        // per frame per referenced sprite Guid.
        void Request(const Arcane::Guid& id);

        // Asset re-saved / removed: evict its Batcher2D texture binding-set
        // entry FIRST -- Batcher2D.hpp:181-191 is explicit that RemoveTexture
        // must run BEFORE the texture is released, since the cached
        // binding-set entry pins the texture alive (leak) and a stale entry
        // would be served for a DIFFERENT texture if the allocator reuses the
        // freed address (ABA) -- then drop the table + keep-alive entries so
        // the next Request re-resolves from disk.
        void Invalidate(const Arcane::Guid& id);

        // Forget everything (project switch): a Guid resolves through the
        // CURRENT project's registry, so a cached entry from the outgoing
        // project may resolve to something else entirely (or nothing) once
        // the project changes -- same rationale as
        // SpriteMaterialCache::Clear() (SpriteMaterialCache.hpp:79-81).
        // Evicts every live texture from the batcher first (Invalidate's
        // order, Batcher2D.hpp:181-191) before dropping both maps.
        void Clear();

        const std::unordered_map<Arcane::Guid, Arcane::SpriteEntry>& Table() const { return m_table; }

    private:
        Services m_services;
        std::unordered_map<Arcane::Guid, Arcane::SpriteEntry>  m_table;
        std::unordered_map<Arcane::Guid, nvrhi::TextureHandle> m_handles;  // keep-alive refs
    };
}
