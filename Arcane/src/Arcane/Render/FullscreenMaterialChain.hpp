#pragma once

// FullscreenMaterialChain: runs a fullscreen pass DAG -- N material passes
// sharing ONE merged template/instance (BuildMaterialChainSource's contract),
// each reading up to kMaxPassInputs upstream pass outputs through the reserved
// InputTexture(N) set. Execution order is pass order (the builder guarantees
// inputs only reference earlier passes), and EVERY pass owns a persistent
// RGBA16F intermediate sized to the target -- which is what makes per-pass
// thumbnails (PassOutput) free. The `viewIndex` pass additionally renders
// into the TARGET itself, so "view any pass" costs one extra draw of that one
// pass -- no blit.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
            // Upstream pass indices feeding InputTexture(N), already validated
            // by BuildMaterialChainSource (earlier passes only).
            std::vector<std::uint32_t> inputs;
        };

        // Bind a compiled chain ATOMICALLY: the merged template + one entry per
        // pass, in chain order, plus the UNIFORM input-slot count every pass's
        // layout carries (MaterialChainBuildResult::chainInputSlots).
        // All-or-nothing -- on any failure the previous chain keeps rendering
        // (chain-level last-good, matching SetMaterial's contract).
        virtual bool SetChain(std::shared_ptr<const MaterialTemplate> templ,
                              std::span<const PassShaders> passes,
                              std::uint32_t inputSlots) = 0;

        virtual bool Ready() const = 0;
        virtual std::size_t PassCount() const = 0;

        // Pass i's rendered output (its persistent intermediate) -- the node
        // thumbnail source. Null until the first Render after SetChain, or for
        // an out-of-range index. The handle stays valid between Renders.
        virtual nvrhi::ITexture* PassOutput(std::size_t pass) const = 0;

        // Record the chain into an OPEN command list; `target` must be a LINEAR
        // framebuffer (the OffscreenCanvas canvas via DrawPass). The instance
        // must be over the template SetChain bound; the one packed CB and
        // texture table serve every pass. Every pass renders its intermediate
        // (thumbnails stay live); the `viewIndex` pass (clamped; SIZE_MAX = the
        // last) also renders into the target. Texture params follow the same
        // pre-load contract as FullscreenMaterialPass::Render.
        virtual void Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target,
                            const MaterialInstance& instance,
                            const GlobalParams& globals,
                            Assets* assets,
                            std::size_t viewIndex = static_cast<std::size_t>(-1)) = 0;
    };
}
