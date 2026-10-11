#pragma once

#include <NRI.h>
#include <Arcane/Client/Api.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <cstdint>
#include <memory>

struct ImDrawData;

namespace Arcane
{
    class Graveyard;
    class NriGraphContext;

    class ARC_CLIENT_API RenderOverlayNode
    {
    public:
        virtual ~RenderOverlayNode() = default;
        virtual void Release(Graveyard&, std::uint64_t) = 0;
        virtual void AdoptImGuiContext(void*) = 0;
        virtual void PrepareFrame(ImDrawData*, Graveyard&, std::uint64_t) = 0;
        virtual bool InvalidateUserTexture(nri::Texture*, Graveyard&, std::uint64_t) = 0;
        virtual bool InvalidateUserTextureNow(nri::Texture*) = 0;
        virtual void Record(RenderGraphNodeContext&, ImDrawData*, RgTexture) = 0;
    };

    struct RenderOverlayHooks
    {
        std::unique_ptr<RenderOverlayNode> (*create)(NriGraphContext&) = nullptr;
        void (*addNode)(RenderGraph&, NriGraphContext*, RgTexture, bool gameUi) = nullptr;
    };

    ARC_CLIENT_API void SetRenderOverlayHooks(RenderOverlayHooks hooks) noexcept;
    ARC_CLIENT_API RenderOverlayHooks GetRenderOverlayHooks() noexcept;
}
