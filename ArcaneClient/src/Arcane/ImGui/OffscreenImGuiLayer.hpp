#pragma once

// A self-contained Dear ImGui context that renders into a caller-provided
// framebuffer with MANUALLY INJECTED input (no OS window / SDL3 backend). For
// hosting a SECOND ImGui layer inside an offscreen render target -- e.g. a
// game/plugin's debug UI composited into an editor viewport, separate from the
// editor's own ImGui. Owns its ImGuiContext + ImGuiNvrhiRenderer + font atlas;
// editor- and game-agnostic. Sibling of ImGuiLayer (which owns the OS-window,
// SDL3-backed context).
//
// TWO FLAVORS SINCE NRI PHASE 3, TASK 9, and they differ ONLY in who renders
// the draw data -- the split ImGuiLayer already carries, for the same reason
// and with the same shape:
//
//   Create()          the NVRHI flavor. Context + ImGuiNvrhiRenderer.
//                     Render(cmd, fb) draws into an offscreen framebuffer.
//   CreateForGraph()  the GRAPH flavor. Context and NO renderer at all -- the
//                     frame graph's ImGuiNriNode is the renderer, and it
//                     consumes the ImDrawData RenderToDrawData() hands back.
//                     Render(cmd, fb) is unreachable there and says so.
//
// WHAT THE FLAVORS SHARE IS THE POINT, not an implementation detail: the
// CONTEXT DISCIPLINE. io.IniFilename is null on both (a game/plugin HUD must
// never persist into the host's layout file -- in the editor that file is the
// PER-PROJECT layout, and per-id ini state silently overrides authored UI on
// every later boot), the atlas is this context's own, and every entry point
// pins m_context with ImGui::SetCurrentContext before touching ImGui state.
// The graph flavor exists so that discipline survives on a host with no NVRHI
// device; dropping the layer there would hand the plugin the HOST's context
// and with it the host's ini file.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <memory>

struct ImDrawData;

namespace Arcane
{
    class RenderDevice;
    class ShaderLibrary;

    class ARCANE_API OffscreenImGuiLayer
    {
    public:
        static std::unique_ptr<OffscreenImGuiLayer> Create(RenderDevice& device,
                                                           ShaderLibrary& shaders);

        // The GRAPH flavor: no device, no ShaderLibrary, no NVRHI renderer --
        // just the context, its own atlas and the same injected-input surface.
        // The graph's ImGuiNriNode draws RenderToDrawData()'s output instead.
        //
        // ONE OBLIGATION THE NVRHI FLAVOR DOES NOT CARRY: whichever ImGuiNri
        // will be handed this context's draw data must ADOPT it
        // (ImGuiNriNode::AdoptImGuiContext) before the first frame. That
        // backend installs ImGuiBackendFlags_RendererHasTextures on whatever
        // context is current when IT is created, which is the HOST's, not this
        // one's -- and a draw list built without that flag carries no atlas for
        // the node to upload. See ImGuiNri::AdoptContext.
        static std::unique_ptr<OffscreenImGuiLayer> CreateForGraph();

        virtual ~OffscreenImGuiLayer() = default;

        // The underlying ImGuiContext* (as void*, keeping this header imgui-free
        // like Runtime.hpp). Pass to a plugin via Runtime::SetImGui / EngineContext.
        virtual void* Context() const = 0;

        // Injected IO for the NEXT BeginFrame. Coordinates are target-local px.
        struct Input
        {
            glm::vec2 displaySize{0.0f, 0.0f};   // offscreen target size (px)
            glm::vec2 mousePos{0.0f, 0.0f};      // target-local px
            bool      mouseDown[5] = {};         // LMB,RMB,MMB,X1,X2
            float     wheel        = 0.0f;
            float     deltaTime    = 1.0f / 60.0f;
            bool      hasInput     = false;      // false => cursor off-target, no buttons
        };
        virtual void SetInput(const Input&) = 0;

        // SetCurrentContext(this) + inject IO + ImGui::NewFrame(). Caller then
        // issues ImGui draw calls (or a plugin's DrawUI). Caches WantCapture* for
        // this frame. Pair every BeginFrame with EXACTLY ONE of Render /
        // RenderToDrawData / EndFrameDiscard -- a double-Begin trips ImGui's own
        // asserts, and an unpaired one asserts on the NEXT frame.
        virtual void BeginFrame() = 0;

        // SetCurrentContext(this) + ImGui::Render() + RenderDrawData into `target`
        // on the OPEN command list (display-referred target; ImGui blends over it).
        // NVRHI flavor only: the graph flavor built no renderer and refuses.
        virtual void Render(nvrhi::ICommandList*, nvrhi::IFramebuffer* target) = 0;

        // Ends this frame and hands back ITS draw data, recording nothing --
        // for a renderer that is not this layer's (the graph path's
        // ImGuiNriNode, via NriGraphContext::FrameDesc::gameUi). The returned
        // pointer stays valid until the next BeginFrame, which is what lets
        // that node copy the geometry at record time.
        virtual ImDrawData* RenderToDrawData() = 0;

        // Ends this frame and DROPS it: the balancing move for a frame that was
        // begun and must not be drawn.
        virtual void EndFrameDiscard() = 0;

        // Valid after BeginFrame; reflects whether THIS context's UI wants the
        // pointer/keys this frame (i.e. the cursor is over its widgets).
        virtual bool WantCaptureMouse() const = 0;
        virtual bool WantCaptureKeyboard() const = 0;
    };
}
