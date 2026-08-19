#include <Arcane/Render/SpriteCache.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <utility>

namespace Arcane
{
    // THE KEEP-ALIVE MAP IS GONE (NRI Phase 5a, Task 7). A second map held a
    // reference to every resolved GPU texture, because SpriteEntry::texture is
    // a BARE pointer and the Assets facade's own LRU could evict the last
    // reference out from under it. Residency is NriTextureCache's job
    // exclusively now -- it keys off SpriteEntry::textureId, the device-free
    // half of the pair -- so there is no engine-side GPU texture for this cache
    // to pin. See Request() for why nothing fills `texture` any more.
    struct SpriteCache::Impl
    {
        Services services;
        std::unordered_map<Guid, SpriteEntry> table;
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

        // Success: the entry always carries the asset's own pivot. UV/size
        // resolve only when the texture Guid is valid AND the Assets facade
        // yields pixels for it -- a memoized prior load failure returns null
        // here too, same as an unset texture Guid. Either way
        // ComputeSpriteGeom(data, 0, 0) is the documented 1x1 m / full-UV
        // fallback (SpriteAsset.hpp:64-66, SpriteAsset.cpp:104-106), so the
        // untextured case needs no special-case geometry of its own.
        SpriteEntry entry;
        entry.pivot = data->pivot;
        // The DEVICE-FREE key (NRI Phase 3, Task 2), and since Task 7 the ONLY
        // key: it names the asset the graph path's NriTextureCache uploads onto
        // its own device. Set unconditionally, including when the image has no
        // decodable pixels -- the record must still say WHICH image it wanted.
        entry.textureId = data->texture;

        std::uint32_t texWidth = 0, texHeight = 0;
        if (data->texture.IsValid() && m_impl->services.assets)
        {
            // Geometry's dimensions come from PixelsFor -- device-free, so
            // this resolves with no render device bound, which is the only
            // configuration there is.
            //
            // THE DIMENSIONS ARE COPIED OUT IMMEDIATELY, and that ordering is
            // the contract, not a style choice (Assets.hpp: "valid only until
            // evicted -- callers that need it to outlive the current call must
            // copy it, not hold the pointer"). PixelsFor returns a BARE pointer
            // into the facade's LRU-budgeted pixel cache and drops its own pin
            // before returning, so ANY later Assets call may free it and read
            // it back as garbage. This function used to make exactly such a
            // call -- a GetTexture whose trailing EnforceBudget() can evict the
            // globally least-recently-used entry across all four caches, the
            // pixel cache included -- and reading pixels->width after it was a
            // real use-after-free, fixed by copying first. That second call is
            // gone (NRI Phase 5a, Task 7), so the hazard is now structurally
            // absent rather than merely avoided; the copy stays anyway, because
            // it costs nothing and it is what keeps the next Assets call added
            // here from being a bug.
            const PixelData* pixels = m_impl->services.assets->PixelsFor(data->texture);
            if (pixels)
            {
                texWidth  = pixels->width;
                texHeight = pixels->height;
            }
        }

        // NOTHING RESOLVES entry.texture ANY MORE (NRI Phase 5a, Task 7). It
        // used to hold an Assets::GetTexture result, pinned alive by a
        // second map in Impl. Both are gone together: the pointer names an
        // object on an engine-owned NVRHI device, no such device is created in
        // any configuration, and GetTexture accordingly returns null for every
        // sprite (Assets.cpp: "No render device yet ... degrade to null"). The
        // field stays null, which is what it already was at runtime, and
        // `entry.textureId` above is what the graph path resolves through
        // NriTextureCache. The PixelsFor/GetTexture dimension cross-check went
        // with it -- there is no second dimension source left to disagree.
        const ResolvedSpriteGeom g = ComputeSpriteGeom(*data, texWidth, texHeight);
        entry.uvMin = g.uvMin;
        entry.uvMax = g.uvMax;
        entry.sizeMeters = g.sizeMeters;

        m_impl->table.emplace(id, entry);
    }

    // THE EVICT-BEFORE-RELEASE HOOK BELOW IS INERT, and stated rather than
    // quietly left to look live: `entry.texture` is never set any more (see
    // Request), so the guard never passes and Batcher2D::RemoveTexture is never
    // called from here. It is kept, rather than torn out with the map it
    // guarded, because RemoveTexture is a Batcher2D vtable slot and removing
    // that whole NVRHI surface is a plugin-ABI change this task is fenced out
    // of. When it goes, these two guards go with it.
    void SpriteCache::Invalidate(const Guid& id)
    {
        const auto it = m_impl->table.find(id);
        if (it != m_impl->table.end())
        {
            if (it->second.texture && m_impl->services.batcher)
                m_impl->services.batcher->RemoveTexture(it->second.texture);
            m_impl->table.erase(it);
        }
    }

    void SpriteCache::Clear()
    {
        if (m_impl->services.batcher)
            for (const auto& [id, entry] : m_impl->table)
                if (entry.texture)
                    m_impl->services.batcher->RemoveTexture(entry.texture);
        m_impl->table.clear();
    }
}
