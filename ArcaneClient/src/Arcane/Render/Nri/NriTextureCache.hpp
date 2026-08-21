#pragma once

// NriTextureCache -- ONE Guid -> nri::Texture residency cache for the whole
// render path.
//
// IT IS SHARED BY EVERY NODE THAT SAMPLES AN ASSET, and that sharing is the
// point: a sprite's own texture (t0, from Batch2DDrawSpan::textureId), a
// registered material's DECLARED params (t1..), and a post material's
// declared params all resolve through THIS cache -- so an image named by a
// sprite and by a post pass gets ONE upload rather than two.
//
// WHERE THE PIXELS COME FROM, and why this class does not decode. The bytes
// arrive through an injected `PixelSupplyFn` -- in production
// `Assets::PixelsFor(Guid)`, the engine's retained decode-once pixel cache,
// which is DEVICE-FREE by charter. So this class owns no file I/O, no stb call
// and no knowledge of the project registry. It is the same injection shape
// (and the same reasoning) as NriGraphContext::AssetResolveFn: a RENDER object
// must not grow a Runtime.
//
// THE SRGB RULE: scene content uploads RGBA8_SRGB, because the canvas is
// LINEAR and the hardware does the decode -- a UNORM view would render the
// same asset visibly brighter. See ColorSpace below for the other half.
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

        // ===== WHICH COLOUR SPACE THE VIEW SAMPLES IN ====================
        // TWO SPACES, because the two have DIFFERENT consumers and there is no
        // one format that serves both:
        //
        //   Srgb    -- the SCENE. Uploads RGBA8_SRGB, so the sampler decodes
        //              to linear for the linear canvas and the hardware does
        //              the work. THE DEFAULT, and what every existing caller
        //              (Batch2DNode's sprites, PostChainNode's declared
        //              params) gets without asking.
        //   Display -- ImGui. Uploads RGBA8_UNORM, so the sampled texel goes
        //              STRAIGHT to a display-referred target. ImGuiNri draws
        //              after the tonemap and its own font atlas is UNORM for
        //              this reason; an SRGB view under it decodes a second
        //              time and the image renders visibly dark.
        //
        // THE SAME ASSET MAY BE RESIDENT IN BOTH, and that is deliberate
        // rather than a leak: the entries are keyed on (Guid, space) and a
        // second space is a second upload. NRI has no typeless textures to
        // hang two views off, and an asset wanted in both spaces is rare (one
        // is scene content, the other is chrome).
        enum class ColorSpace : std::uint8_t { Srgb, Display };

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
        //
        // `space` defaults to Srgb, which is what every scene caller means and
        // is what keeps this signature's old behaviour byte for byte.
        [[nodiscard]] nri::Texture* Resolve(const Guid& id, ColorSpace space = ColorSpace::Srgb);

        // The SHADER_RESOURCE view over Resolve(id, space), created with it.
        // Null under exactly the same conditions, and it does NOT trigger a
        // resolve: a caller that wants residency asks Resolve first (which is
        // what the nodes' Prepare passes do), so nothing uploads from a lookup.
        [[nodiscard]] nri::Descriptor* View(const Guid& id,
                                            ColorSpace space = ColorSpace::Srgb) const;

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

        // (asset, colour space) -- see ColorSpace. The SPACE is part of the
        // identity because it decides the uploaded FORMAT, so two spaces are
        // two textures and a lookup that ignored it would hand a caller the
        // other one's.
        struct Key
        {
            Guid       id;
            ColorSpace space = ColorSpace::Srgb;
            bool operator==(const Key&) const noexcept = default;
        };
        struct KeyHash
        {
            std::size_t operator()(const Key& k) const noexcept
            {
                // The Guid's own hash, mixed with the space through the
                // 32-BIT golden-ratio constant (0x9E3779B9; the 64-bit one is
                // 0x9E3779B97F4A7C15) -- two spaces of one asset must not land
                // in the same bucket chain by construction.
                const std::size_t h = std::hash<Guid>{}(k.id);
                return h ^ (static_cast<std::size_t>(k.space) + 0x9E3779B9u + (h << 6) + (h >> 2));
            }
        };

        NriDevice*          m_device = nullptr;
        nri::HelperInterface m_helper{};
        PixelSupplyFn       m_supply;
        std::unordered_map<Key, Resident, KeyHash> m_textures;
        // THE ONE-SHOT MISS WARN (moved here from Batch2DNode): one line per
        // run, not one per span per frame.
        bool m_warnedMiss = false;
    };
}
