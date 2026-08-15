#include <Arcane/Render/SpriteCache.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <nvrhi/nvrhi.h>

#include <utility>

namespace Arcane
{
    struct SpriteCache::Impl
    {
        Services services;
        std::unordered_map<Guid, SpriteEntry>          table;
        std::unordered_map<Guid, nvrhi::TextureHandle> handles;   // keep-alive refs
    };

    SpriteCache::SpriteCache(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
    }

    SpriteCache::~SpriteCache() { delete m_impl; }

    const std::unordered_map<Guid, SpriteEntry>& SpriteCache::Table() const
    {
        return m_impl->table;
    }

    void SpriteCache::Request(const Guid& id)
    {
        if (!id.IsValid() || m_impl->table.contains(id))
            return;

        // Negative-result caching: emplace a DEFAULT SpriteEntry (1x1 m,
        // untextured -- SceneResources.hpp:85-92's field defaults) directly
        // into the PUBLISHED table -- unlike SpriteMaterialCache::Request,
        // which keeps a failed material OUT of its published table via a
        // separate `failed` set (SpriteMaterialCache.cpp:47,95-101) so the
        // sprite just falls back to the plain (unmaterialed) pipeline. A
        // sprite has no such fallback -- the component's whole point is to
        // draw a specific asset -- so a broken one renders as the VISIBLE
        // placeholder instead of vanishing. Either structure gets the same
        // never-re-parsed-once-known effect via the table.contains(id) guard
        // above.
        auto cacheDefault = [&](const char* why)
        {
            ARC_WARN("SpriteCache: sprite {} unavailable -- {}", id.ToString(), why);
            m_impl->table.emplace(id, SpriteEntry{});
        };

        if (!m_impl->services.resolveAsset)
            return cacheDefault("no asset resolver installed");
        const auto path = m_impl->services.resolveAsset(id);
        if (!path)
            return cacheDefault("not in the asset registry");
        const auto data = LoadSpriteAsset(*path);
        if (!data)
            return cacheDefault("asset failed to load");

        // Success: the entry always carries the asset's own pivot. Texture/
        // UV/size resolve only when the texture Guid is valid AND the Assets
        // facade yields something -- a memoized prior load failure returns
        // null here too, same as an unset texture Guid. Either way
        // ComputeSpriteGeom(data, 0, 0) is the documented 1x1 m / full-UV
        // fallback (SpriteAsset.hpp:64-66, SpriteAsset.cpp:104-106), so the
        // untextured case needs no special-case geometry of its own.
        SpriteEntry entry;
        entry.pivot = data->pivot;
        // The DEVICE-FREE key (NRI Phase 3, Task 2), set whether or not the
        // GPU texture below resolves: it is the asset the graph path's
        // NriTextureCache uploads onto its own device, and in graph mode
        // `entry.texture` is null for every sprite because Assets holds no
        // NVRHI device at all.
        entry.textureId = data->texture;

        nvrhi::TextureHandle tex;
        const PixelData* pixels = nullptr;
        if (data->texture.IsValid() && m_impl->services.assets)
        {
            // Geometry's dimensions come from PixelsFor -- device-free, so
            // this resolves even with no render device bound (null-device
            // legal). GetTexture routes its own upload through the SAME
            // retained pixels (Assets.cpp), so this is a decode cache hit
            // whichever of the two is called first.
            pixels = m_impl->services.assets->PixelsFor(data->texture);
            tex = m_impl->services.assets->GetTexture(AssetId::FromGuid(data->texture));
        }

        std::uint32_t texWidth = 0, texHeight = 0;
        if (pixels)
        {
            texWidth = pixels->width;
            texHeight = pixels->height;
        }
        if (tex)
        {
            // The live GPU texture's desc describes the SAME decode PixelsFor
            // just supplied (GetTexture is a consumer of it) -- the two must
            // always agree when both exist. A mismatch here would mean the
            // NVRHI upload path and the device-free pixel supply have drifted
            // apart, which the "identical GPU result" contract forbids.
            const auto& desc = tex->getDesc();
            ARC_ASSERT(pixels != nullptr && texWidth == desc.width && texHeight == desc.height,
                       "SpriteCache: PixelsFor and GetTexture disagree on texture dimensions");
        }
        const ResolvedSpriteGeom g = ComputeSpriteGeom(*data, texWidth, texHeight);
        entry.uvMin = g.uvMin;
        entry.uvMax = g.uvMax;
        entry.sizeMeters = g.sizeMeters;

        if (tex)
        {
            entry.texture = tex.Get();
            m_impl->handles.emplace(id, std::move(tex));   // keep-alive: outlives Assets' own LRU eviction
        }

        m_impl->table.emplace(id, entry);
    }

    void SpriteCache::Invalidate(const Guid& id)
    {
        const auto it = m_impl->table.find(id);
        if (it != m_impl->table.end())
        {
            // Evict BEFORE release (Batcher2D.hpp:181-191): the cached
            // texture->binding-set entry pins the texture alive, and a stale
            // entry would be served for a DIFFERENT texture if the allocator
            // reuses the freed address (ABA). This is that hook's first-ever
            // caller.
            if (it->second.texture && m_impl->services.batcher)
                m_impl->services.batcher->RemoveTexture(it->second.texture);
            m_impl->table.erase(it);
        }
        m_impl->handles.erase(id);
    }

    void SpriteCache::Clear()
    {
        // Same evict-before-release order as Invalidate (Batcher2D.hpp:
        // 181-191): drop every live texture's cached binding-set entry FIRST,
        // while handles still holds the keep-alive ref, then clear both maps.
        if (m_impl->services.batcher)
            for (const auto& [id, entry] : m_impl->table)
                if (entry.texture)
                    m_impl->services.batcher->RemoveTexture(entry.texture);
        m_impl->table.clear();
        m_impl->handles.clear();
    }
}
