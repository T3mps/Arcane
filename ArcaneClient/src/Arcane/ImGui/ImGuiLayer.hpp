#pragma once

// ImGui module facade: owns the context + the platform backend (upstream
// imgui_impl_sdl3, via Window's native event tap). Renders POST-tonemap into
// the display-referred backbuffer framebuffer the caller provides.
//
// ONE FLAVOR: context + SDL3 platform backend + the event tap.
// RenderToDrawData() is the only renderer entry point -- this facade never
// draws its own output; the frame graph's ImGuiNriNode does, consuming the
// ImDrawData handed back below.
//
// EVERY entry point pins its own context with ImGui::SetCurrentContext before
// touching ImGui state. That pin is the whole reason a render path must
// route its ImGui::Render()/EndFrame() through this facade instead of calling
// them bare: a second (offscreen) context -- an editor document, a plugin that
// adopted its own -- may be current, and a bare call would end the wrong
// frame.

#include <Arcane/Base/Api.hpp>

#include <memory>

struct ImDrawData;

namespace Arcane
{
    class Window;

    class ARCANE_API ImGuiLayer
    {
    public:
        // Window must outlive the layer (the layer taps its events, and the
        // platform backend reads its size every BeginFrame -- that is what
        // makes io.DisplaySize the HOST window's extent, i.e. the same
        // surface the graph swapchain binds).
        static std::unique_ptr<ImGuiLayer> Create(Window& window);

        virtual ~ImGuiLayer() = default;

        // sdl3 NewFrame + ImGui::NewFrame. Caller MUST pair every BeginFrame
        // with exactly one RenderToDrawData / EndFrameDiscard; a double-Begin
        // trips ImGui's own asserts.
        virtual void BeginFrame() = 0;

        // Ends this frame and hands back ITS draw data, recording nothing --
        // for a renderer that is not this layer's (the graph path's
        // ImGuiNriNode). The returned pointer stays valid until the next
        // BeginFrame, which is what lets that node copy the geometry at
        // record time.
        virtual ImDrawData* RenderToDrawData() = 0;

        // Ends this frame and DROPS it: the balancing move for a frame that
        // was begun and must not be drawn (the two non-Full golden stages,
        // where host chrome would mask the pixels a stage golden compares).
        virtual void EndFrameDiscard() = 0;

        // ImGui capture state for the input layer: pass these into
        // InputDevices::Sample so kbm actions release while ImGui owns the
        // keyboard/mouse (text fields, drags). Valid between frames.
        virtual bool WantCaptureKeyboard() const = 0;
        virtual bool WantCaptureMouse() const = 0;
    };
}
