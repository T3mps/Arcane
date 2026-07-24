#pragma once

// FullscreenMaterialChain: runs a fullscreen pass chain -- N material passes
// sharing ONE merged template/instance (BuildMaterialChainSource's contract)
// -- by ping-ponging two RGBA16F intermediates sized to the target. Each pass
// reads the previous pass's output through the reserved InputTexture (pass 0
// reads black). `viewIndex` truncates the chain so the editor can view ANY
// intermediate: passes 0..k-1 render into the ping-pong and pass k renders
// into the TARGET itself -- no extra blit, no wasted passes.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <memory>
#include <span>

namespace Arcane
{
    class Assets;

    class ARCANE_API FullscreenMaterialChain
    {
    public:
        // Null (with ARC_ERROR) on a null device.
        static std::unique_ptr<FullscreenMaterialChain> Create(nvrhi::IDevice* device);

        virtual ~FullscreenMaterialChain() = default;

        struct PassShaders
        {
            nvrhi::ShaderHandle vs;
            nvrhi::ShaderHandle ps;
        };

        // Bind a compiled chain ATOMICALLY: the merged template + one (vs, ps)
        // per pass, in chain order. All-or-nothing -- on any failure the
        // previous chain keeps rendering (chain-level last-good, matching
        // FullscreenMaterialPass::SetMaterial's contract).
        virtual bool SetChain(std::shared_ptr<const MaterialTemplate> templ,
                              std::span<const PassShaders> passes) = 0;

        virtual bool Ready() const = 0;
        virtual std::size_t PassCount() const = 0;

        // Record the chain into an OPEN command list; `target` must be a LINEAR
        // framebuffer (the OffscreenCanvas canvas via DrawPass). The instance
        // must be over the template SetChain bound; the one packed CB and
        // texture table serve every pass. `viewIndex` clamps to the last pass
        // (pass SIZE_MAX = the full chain). Texture params follow the same
        // pre-load contract as FullscreenMaterialPass::Render.
        virtual void Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target,
                            const MaterialInstance& instance,
                            const GlobalParams& globals,
                            Assets* assets,
                            std::size_t viewIndex = static_cast<std::size_t>(-1)) = 0;
    };
}
