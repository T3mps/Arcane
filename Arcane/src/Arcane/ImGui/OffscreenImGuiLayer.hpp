#pragma once

// A self-contained Dear ImGui context that renders into a caller-provided
// framebuffer with MANUALLY INJECTED input (no OS window / SDL3 backend). For
// hosting a SECOND ImGui layer inside an offscreen render target -- e.g. a
// game/plugin's debug UI composited into an editor viewport, separate from the
// editor's own ImGui. Owns its ImGuiContext + ImGuiNvrhiRenderer + font atlas;
// editor- and game-agnostic. Sibling of ImGuiLayer (which owns the OS-window,
// SDL3-backed context).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <memory>

namespace Arcane
{
    class RenderDevice;
    class ShaderLibrary;

    class ARCANE_API OffscreenImGuiLayer
    {
    public:
        static std::unique_ptr<OffscreenImGuiLayer> Create(RenderDevice& device,
                                                           ShaderLibrary& shaders);
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
        // this frame. Pair every BeginFrame with a Render.
        virtual void BeginFrame() = 0;

        // SetCurrentContext(this) + ImGui::Render() + RenderDrawData into `target`
        // on the OPEN command list (display-referred target; ImGui blends over it).
        virtual void Render(nvrhi::ICommandList*, nvrhi::IFramebuffer* target) = 0;

        // Valid after BeginFrame; reflects whether THIS context's UI wants the
        // pointer/keys this frame (i.e. the cursor is over its widgets).
        virtual bool WantCaptureMouse() const = 0;
        virtual bool WantCaptureKeyboard() const = 0;
    };
}
