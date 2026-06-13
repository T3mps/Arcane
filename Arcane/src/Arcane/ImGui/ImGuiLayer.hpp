#pragma once

// ImGui module facade: owns the context + both backends (upstream
// imgui_impl_sdl3 for platform/input via Window's native event tap;
// first-party NVRHI renderer). Renders POST-tonemap into the
// display-referred backbuffer framebuffer the caller provides.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace Arcane
{
    class Window;
    class RenderDevice;
    class ShaderLibrary;

    class ARCANE_API ImGuiLayer
    {
    public:
        // Window must outlive the layer (the layer taps its events).
        static std::unique_ptr<ImGuiLayer> Create(Window& window,
                                                  RenderDevice& device,
                                                  ShaderLibrary& shaders);
        virtual ~ImGuiLayer() = default;

        // sdl3 NewFrame + ImGui::NewFrame. Caller MUST pair every BeginFrame
        // with exactly one Render; a double-Begin trips ImGui's own asserts.
        virtual void BeginFrame() = 0;
        // ImGui::Render + draw into target (an OPEN command list).
        virtual void Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target) = 0;

        // ImGui capture state for the input layer: pass these into
        // InputDevices::Sample so kbm actions release while ImGui owns the
        // keyboard/mouse (text fields, drags). Valid between frames.
        virtual bool WantCaptureKeyboard() const = 0;
        virtual bool WantCaptureMouse() const = 0;
    };
}
