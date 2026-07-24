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

#include <memory>

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
        virtual bool SetMaterial(std::shared_ptr<const MaterialTemplate> templ,
                                 nvrhi::ShaderHandle vs, nvrhi::ShaderHandle ps) = 0;

        // True once SetMaterial has accepted a material.
        virtual bool Ready() const = 0;

        // Record the fullscreen pass into an OPEN command list targeting a
        // LINEAR framebuffer. Packs the instance's params into b0 and `globals`
        // into b1, binds the instance's textures (resolved through `assets` by
        // Guid; null/missing -> the white fallback). The instance's template
        // must be the one SetMaterial bound (no-op + warn otherwise).
        virtual void Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target,
                            const MaterialInstance& instance,
                            const GlobalParams& globals,
                            Assets* assets) = 0;
    };
}
