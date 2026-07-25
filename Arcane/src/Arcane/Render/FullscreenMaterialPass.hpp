#pragma once

// FullscreenMaterialPass: renders a compiled MATERIAL as a fullscreen triangle
// into a linear-HDR framebuffer (the OffscreenCanvas canvas via DrawPass, or
// any canvas fb) -- the material-system counterpart of TonemapPass, and the
// heart of the shader-editor preview. Binding surface mirrors what
// GenerateMaterialBindings emits: volatile CB b0 (material params, packed by
// MaterialInstance::PackCB), volatile CB b1 (GlobalParams), Texture2D t0..N-1
// (the instance's texture table via the Assets GUID seam, white 1x1 fallback),
// one sampler s0. Shaders arrive from the caller -- created at the compile
// service's Drain site (the only place NVRHI object creation happens) -- and
// SetMaterial swaps them atomically: on compile failure the caller simply
// doesn't call SetMaterial, so the LAST-GOOD material keeps rendering.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <span>

namespace Arcane
{
    class Assets;

    class ARCANE_API FullscreenMaterialPass
    {
    public:
        // Null (with ARC_ERROR) when the sampler/fallback-texture setup fails.
        static std::unique_ptr<FullscreenMaterialPass> Create(nvrhi::IDevice* device);

        virtual ~FullscreenMaterialPass() = default;

        // Bind a compiled material: the template (layout) + the vs/ps compiled
        // from its stitched source. Rebuilds the binding layout, buffers, and
        // pipeline cache. False (pass keeps its previous material) on null args.
        // `chainInputs` (pass chains): the source was stitched with that many
        // reserved upstream textures, so the layout additionally exposes SRVs
        // at slots TextureCount()..TextureCount()+chainInputs-1.
        virtual bool SetMaterial(std::shared_ptr<const MaterialTemplate> templ,
                                 nvrhi::ShaderHandle vs, nvrhi::ShaderHandle ps,
                                 std::uint32_t chainInputs) = 0;
        bool SetMaterial(std::shared_ptr<const MaterialTemplate> templ,
                         nvrhi::ShaderHandle vs, nvrhi::ShaderHandle ps)
        { return SetMaterial(std::move(templ), std::move(vs), std::move(ps), 0); }

        // True once SetMaterial has accepted a material.
        virtual bool Ready() const = 0;

        // Record the fullscreen pass into an OPEN command list targeting a
        // LINEAR framebuffer. Packs the instance's params into b0 and `globals`
        // into b1, binds the instance's textures (resolved through `assets` by
        // Guid; null/missing -> the white fallback). The instance's template
        // must be the one SetMaterial bound (no-op + warn otherwise).
        // `chainInputs` supplies the upstream pass outputs for a chain-mode
        // material, in slot order (fewer than bound / null entries -> a 1x1
        // black fallback; ignored for non-chain materials).
        // CONTRACT: texture params must already be loaded (call
        // assets->GetTexture per guid BEFORE opening the list) -- a first-time
        // load here executes an upload list while the caller's list is open,
        // which loses the upload and samples an empty texture.
        virtual void Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target,
                            const MaterialInstance& instance,
                            const GlobalParams& globals,
                            Assets* assets,
                            std::span<nvrhi::ITexture* const> chainInputs) = 0;
        void Render(nvrhi::ICommandList* commandList,
                    nvrhi::IFramebuffer* target,
                    const MaterialInstance& instance,
                    const GlobalParams& globals,
                    Assets* assets)
        { Render(commandList, target, instance, globals, assets, {}); }
    };
}
