#pragma once

// NriTextureCache -- ONE Guid -> nri::Texture residency cache for the whole
// graph path (NRI Phase 3, Task 2).
//
// PROMOTED, not invented: this is Batch2DNode::EnsureTexture's private cache,
// lifted out whole. Phase 2 gave that node a per-Guid upload so a registered
// material's DECLARED texture params (t1..) could be sampled for real, and
// left two things open which this class closes together:
//
//   1. THE TEXTURE GAP. A Batch2DDrawSpan named an `nvrhi::ITexture*` -- an
//      object on the ENGINE's device, which the graph's device cannot sample
//      -- so the SPRITE's own texture (t0) was the white texel on every span
//      and a textured sprite rendered as its vertex tint alone. Since Task 2 a
//      span also carries the image's ASSET Guid (Batch2DDrawSpan::textureId),
//      which is device-independent, and THIS is what turns that Guid into
//      something t0 can bind.
//   2. THE POST TEXTURE GAP. PostChainNode bound the white texel for a post
//      material's declared params for want of exactly this machinery, which
//      "lives in Batch2DNode::EnsureTexture" (its own comment). Both nodes now
//      share one cache, so a sprite and a post pass that name the same image
//      get ONE upload rather than two.
//
// WHERE THE PIXELS COME FROM, and why this class does not decode. The bytes
// arrive through an injected `PixelSupplyFn` -- in production
// `Assets::PixelsFor(Guid)` (Phase 3, Task 1), the engine's retained
// decode-once pixel cache, which is DEVICE-FREE by charter. So the same decode
// serves the NVRHI upload and this one, and this class owns no file I/O, no
// stb call and no knowledge of the project registry. It is the same injection
// shape (and the same reasoning) as NriGraphContext::AssetResolveFn: a RENDER
// object must not grow a Runtime.
//
// THE SRGB RULE is Assets::GetTexture's, mirrored deliberately rather than
// re-decided: that path uploads `nvrhi::Format::SRGBA8_UNORM` (Assets.cpp's
// GetTexture), the canvas is LINEAR, and the hardware does the decode -- so a
// UNORM view here would render the same asset visibly brighter than the NVRHI
// path and turn a golden compare into a hunt. RGBA8_SRGB is that format's NRI
// spelling.
//
// FAILURES ARE MEMOIZED, exactly once each: an entry is inserted BEFORE the
// first early return, with null members, so an unresolvable or undecodable
// image is attempted once rather than re-attempted every frame by a sprite
// that is still on screen. The ONE-SHOT "not resident" WARN lives here too and
// fires only on a Resolve that MISSES -- a frame full of colored quads (every
// span carrying a nil Guid) says nothing at all.
//
// NO BARRIERS, and none needed. Uploads go through NRI's own
// HelperInterface::UploadData, which submits and waits internally and leaves
// the texture in SHADER_RESOURCE -- so these textures are NOT graph resources
// (they are persistent, never written by a node, and their state never
// changes), which is why nothing about them appears in a compiled frame. They
// must therefore be resolved at DECLARATION time, never from inside a node's
// exec fn: UploadData submitting while the frame's command buffer is open is
// exactly the shape the graph's no-hand-barriers rule exists to prevent.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h>, via spdlog, #defines ERROR through wingdi.h).
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    struct PixelData;

    class ARCANE_API NriTextureCache
    {
    public:
        // Guid -> decoded RGBA8 pixels, or null when the id names nothing this
        // process can decode. `Assets::PixelsFor` in production; the returned
        // pointer is READ INSIDE the call and never stored, so the supply owns
        // the buffer's lifetime and may evict it afterwards.
        using PixelSupplyFn = std::function<const PixelData*(const Guid&)>;

        // Resolves the HelperInterface once and takes a borrowed reference to
        // the device, which must outlive this object (the vehicle owns both).
        // Null, already logged, if the helper is unavailable.
        static std::unique_ptr<NriTextureCache> Create(NriDevice& device);

        // SAFETY NET, NOT THE PATH -- the same shape as ~Batch2DNode. The
        // sanctioned release is Release() at a fence the owner knows; if this
        // still holds objects it destroys them directly behind a
        // DeviceWaitIdle and says so at WARN, because there is no fence value
        // to bury against here and burying at 0 would violate Graveyard's
        // nondecreasing rule on a device the graph has been burying against
        // all run.
        ~NriTextureCache();

        NriTextureCache(const NriTextureCache&)            = delete;
        NriTextureCache& operator=(const NriTextureCache&) = delete;

        // Installed once by the frame driver, right after Create. Without it
        // every Resolve misses (loudly, once) and every texture slot falls back
        // to its node's white texel. Copied, not borrowed.
        void SetPixelSupply(PixelSupplyFn supply) { m_supply = std::move(supply); }

        // The texture for `id` on this device, uploading it on first sight.
        // Null -- meaning "bind the white texel" -- for a nil Guid (the
        // ordinary untextured case, silent), for an id the supply has no
        // pixels for, for an image nri::Dim_t cannot express, and for an NRI
        // refusal. Every null but the nil-Guid one is reported once per cache
        // and then memoized.
        //
        // CALL AT DECLARATION TIME ONLY -- see NO BARRIERS above.
        [[nodiscard]] nri::Texture* Resolve(const Guid& id);

        // The SHADER_RESOURCE view over Resolve(id), created with it. Null
        // under exactly the same conditions, and it does NOT trigger a
        // resolve: a caller that wants residency asks Resolve first (which is
        // what the nodes' Prepare passes do), so nothing uploads from a lookup.
        [[nodiscard]] nri::Descriptor* View(const Guid& id) const;

        // Buries every NRI object this cache owns at `fence` and empties it --
        // views before the textures they view, so the graveyard's in-order
        // reaping can never destroy a texture a live descriptor still names.
        // Idempotent. The caller picks the fence for the same reason
        // NriPipelineCache::Clear does: only it knows which timeline the
        // cache's users submitted on.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // How many images are actually RESIDENT (uploaded and viewable).
        // Memoized failures are not counted -- they hold no GPU object. Public
        // because upload-once is otherwise unobservable from outside, and it
        // is the property that makes this a cache rather than a loader.
        [[nodiscard]] std::size_t ResidentCount() const noexcept;

    private:
        NriTextureCache() = default;

        // One image made resident here. A FAILED load is kept with null
        // members: attempted once, not once per frame.
        struct Resident
        {
            nri::Texture*    texture = nullptr;
            nri::Descriptor* view    = nullptr;
        };

        NriDevice*          m_device = nullptr;
        nri::HelperInterface m_helper{};
        PixelSupplyFn       m_supply;
        std::unordered_map<Guid, Resident> m_textures;
        // THE ONE-SHOT MISS WARN (moved here from Batch2DNode): one line per
        // run, not one per span per frame.
        bool m_warnedMiss = false;
    };
}
