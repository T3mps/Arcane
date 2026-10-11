#pragma once

#include <NRI.h>
#include <Arcane/Core/ModuleApi.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <cstdint>
#include <memory>

struct ImDrawData;

#if defined(ARC_CLIENT_EXPORTS)
#define ARC_RENDER_OVERLAY_API ARC_DLL_EXPORT
#else
#define ARC_RENDER_OVERLAY_API ARC_DLL_IMPORT
#endif

namespace Arcane
{
    class Graveyard;
    class NriGraphContext;

    class ARC_RENDER_OVERLAY_API RenderOverlayNode
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

    ARC_RENDER_OVERLAY_API void SetRenderOverlayHooks(RenderOverlayHooks hooks) noexcept;
    ARC_RENDER_OVERLAY_API RenderOverlayHooks GetRenderOverlayHooks() noexcept;
}
