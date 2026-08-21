// NriTextureCache -- see the header for what this owns, where the pixels come
// from, and why these textures are deliberately NOT graph resources.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "NriTextureCache.hpp"

// The Render/Nri headers FIRST among the Arcane ones, exactly as Batch2DNode
// orders them: NriDevice.hpp pulls Extensions/NRIDeviceCreation.h, whose
// nri::Message enumerator is literally named ERROR, and Arcane/Base/Log.hpp
// reaches <windows.h> through spdlog, which #defines it away via wingdi.h.
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>

#include <Arcane/Assets/ImageIo.hpp>   // PixelData -- the device-free decode (Task 1)
#include <Arcane/Base/Log.hpp>

#undef ERROR

#include <string>

namespace Arcane
{
    std::unique_ptr<NriTextureCache> NriTextureCache::Create(NriDevice& device)
    {
        std::unique_ptr<NriTextureCache> cache(new NriTextureCache());
        cache->m_device = &device;

        // Resolved ONCE, here, rather than per upload: nriGetInterface is a
        // table copy, and doing it inside Resolve would repeat it for every
        // image in the project.
        if (!ARC_NRI_CHECK(nriGetInterface(device.Device(), NRI_INTERFACE(nri::HelperInterface),
                                            &cache->m_helper)))
        {
            ARC_ERROR("[nri-graph] NriTextureCache: HelperInterface unavailable -- no image can be "
                      "made resident on the graph device");
            return nullptr;
        }
        return cache;
    }

    NriTextureCache::~NriTextureCache()
    {
        if (!m_device || m_textures.empty())
            return;

        ARC_WARN("[nri-graph] NriTextureCache destroyed with {} live NRI object set(s) -- its owner "
                 "never called Release(). Destroying directly behind a DeviceWaitIdle.",
                 m_textures.size());
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (auto& [key, resident] : m_textures)
        {
            if (resident.view)    core.DestroyDescriptor(resident.view);
            if (resident.texture) core.DestroyTexture(resident.texture);
        }
        m_textures.clear();
    }

    void NriTextureCache::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view can never outlive its texture.
        for (auto& [key, resident] : m_textures)
        {
            if (resident.view)
                graveyard.Bury(fence, [core, d = resident.view] { core->DestroyDescriptor(d); });
            if (resident.texture)
                graveyard.Bury(fence, [core, t = resident.texture] { core->DestroyTexture(t); });
        }
        m_textures.clear();
        // The warn latch stays SET across a release: a run that could not
        // resolve an image before a resize should not re-announce it after one.
    }

    std::size_t NriTextureCache::ResidentCount() const noexcept
    {
        // Counted on the VIEW, for the same reason Resolve's hit is gated on
        // it: counting TEXTURES over-reports by one for every entry whose
        // upload or view creation failed after the texture itself was created
        // -- entries that are memoized FAILURES, not residents.
        std::size_t count = 0;
        for (const auto& [key, resident] : m_textures)
            if (resident.view)
                ++count;
        return count;
    }

    nri::Descriptor* NriTextureCache::View(const Guid& id, ColorSpace space) const
    {
        const auto found = m_textures.find(Key{ id, space });
        return found != m_textures.end() ? found->second.view : nullptr;
    }

    nri::Texture* NriTextureCache::Resolve(const Guid& id, ColorSpace space)
    {
        // The ordinary untextured case -- every Rect, Line, Circle, glyph and
        // colored quad. Not a miss, not a warning, not an entry.
        if (!id.IsValid())
            return nullptr;

        const Key key{ id, space };
        const auto cached = m_textures.find(key);
        if (cached != m_textures.end())
        {
            // THE VIEW, NOT THE TEXTURE, IS WHAT "RESIDENT" MEANS (whole-branch
            // review, M2). This used to return `texture` with the comment "null
            // for a load this cache already failed", which was false for two of
            // the four failure paths below: an upload failure and a view-create
            // failure both leave `texture` NON-null (it was created before they
            // ran), so a memoized failure was served as a hit and the caller
            // then found View() null. The cache's product is a bindable view --
            // a texture with no view is nothing anyone can use -- so that is
            // what the hit is gated on. The texture object itself stays owned
            // and is destroyed by Release()/ClearImmediate like any other, so
            // this leaks nothing; it just stops lying about what is resident.
            return cached->second.view ? cached->second.texture : nullptr;
        }

        // Inserted BEFORE any early return, so a failure is attempted once
        // rather than re-resolved every frame.
        Resident& resident = m_textures[key];

        // THE ONE-SHOT MISS WARN -- moved here from Batch2DNode, and it now
        // covers the SPRITE's own texture as well as a material's declared
        // params, because both reach residency through this one cache.
        const auto reportMiss = [&](const std::string& why)
        {
            if (m_warnedMiss)
                return;
            m_warnedMiss = true;
            ARC_WARN("[nri-graph] NriTextureCache: image {} is not resident on the graph device -- "
                     "{}; the white texel is bound in its place (further occurrences are silent)",
                     id.ToString(), why);
        };

        const PixelData* pixels = m_supply ? m_supply(id) : nullptr;
        if (!pixels)
        {
            reportMiss(m_supply ? "the asset resolved to nothing, or its image could not be decoded"
                                : "no pixel supply is installed on this vehicle");
            return nullptr;
        }
        if (!pixels->Valid())
        {
            reportMiss("the decoded pixel buffer does not match its own dimensions");
            return nullptr;
        }
        // nri::Dim_t is 16-bit and the cast below would silently WRAP -- the
        // same refusal RenderGraph::RealizePool makes for a transient's extent.
        if (pixels->width > 0xFFFFu || pixels->height > 0xFFFFu)
        {
            reportMiss("the image is " + std::to_string(pixels->width) + "x"
                       + std::to_string(pixels->height)
                       + ", which nri::Dim_t (16-bit) cannot express");
            return nullptr;
        }

        const nri::CoreInterface& core = m_device->Core();

        nri::TextureDesc textureDesc = {};
        textureDesc.type      = nri::TextureType::TEXTURE_2D;
        textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
        // THE SPACE DECIDES THE FORMAT (see ColorSpace):
        //   Srgb    -> RGBA8_SRGB. The canvas is LINEAR and the hardware does
        //              the decode, so a UNORM view here renders the same asset
        //              visibly brighter.
        //   Display -> RGBA8_UNORM. The sampled texel composites directly into
        //              a display-referred target (ImGui draws post-tonemap),
        //              and an SRGB view there decodes a second time and reads
        //              dark.
        textureDesc.format    = space == ColorSpace::Display ? nri::Format::RGBA8_UNORM
                                                             : nri::Format::RGBA8_SRGB;
        textureDesc.width     = (nri::Dim_t)pixels->width;
        textureDesc.height    = (nri::Dim_t)pixels->height;
        textureDesc.depth     = 1;
        textureDesc.mipNum    = 1;   // mip 0 only
        textureDesc.layerNum  = 1;
        textureDesc.sampleNum = 1;
        if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                        0.0f, textureDesc, resident.texture))
            || !resident.texture)
        {
            reportMiss("CreateCommittedTexture failed");
            resident.texture = nullptr;
            return nullptr;
        }
        core.SetDebugName(resident.texture,
                          ("asset " + id.ToString()
                           + (space == ColorSpace::Display ? " (display)" : "")).c_str());

        // Through NRI's OWN helper: UploadData submits and waits internally,
        // which is why every caller must reach this at DECLARATION time and
        // never from inside a node's open command buffer. It is also what keeps
        // this file free of any CmdBarrier at all, so the graph's
        // no-hand-barriers rule reads the same from outside as it does inside.
        nri::TextureSubresourceUploadDesc subresource = {};
        subresource.slices     = pixels->rgba.data();
        subresource.sliceNum   = 1;
        subresource.rowPitch   = pixels->width * 4;
        subresource.slicePitch = pixels->width * pixels->height * 4;

        nri::TextureUploadDesc upload = {};
        upload.subresources = &subresource;
        upload.texture      = resident.texture;
        upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                nri::StageBits::FRAGMENT_SHADER };
        upload.planes       = nri::PlaneBits::ALL;
        if (!ARC_NRI_CHECK(m_helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
        {
            reportMiss("the texture upload failed");
            return nullptr;
        }

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = resident.texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        viewDesc.format   = textureDesc.format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, resident.view)) || !resident.view)
        {
            reportMiss("the shader-resource view could not be created");
            resident.view = nullptr;
            return nullptr;
        }

        ARC_INFO("[nri-graph] NriTextureCache: image {} ({}x{}, {}) is resident on the graph device",
                 id.ToString(), pixels->width, pixels->height,
                 space == ColorSpace::Display ? "display-referred" : "sRGB");
        return resident.texture;
    }
}
