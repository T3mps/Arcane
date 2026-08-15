#pragma once

// ImGui module facade: owns the context + the platform backend (upstream
// imgui_impl_sdl3, via Window's native event tap) and, in the NVRHI flavor,
// the first-party NVRHI renderer. Renders POST-tonemap into the
// display-referred backbuffer framebuffer the caller provides.
//
// TWO FLAVORS SINCE NRI PHASE 3, TASK 6 (the one-device landing), and they
// differ ONLY in who renders the draw data:
//
//   Create()          the NVRHI flavor. Context + SDL3 platform backend + the
//                     event tap + ImGuiNvrhiRenderer. Render(cmd, fb) draws.
//   CreateForGraph()  the GRAPH flavor. Context + SDL3 platform backend and NO
//                     renderer at all -- the frame graph's ImGuiNriNode is the
//                     renderer, and it consumes the ImDrawData
//                     RenderToDrawData() below hands back. Render(cmd, fb) is
//                     unreachable there and says so (recon 6's rule for a
//                     member that was never constructed).
//
// EVERY entry point pins its own context with ImGui::SetCurrentContext before
// touching ImGui state. That pin is the whole reason the graph render arm must
// route its ImGui::Render()/EndFrame() through this facade instead of calling
// them bare: a second (offscreen) context -- an editor document, a plugin that
// adopted its own -- may be current, and a bare call would end the wrong
// frame. Closing that hole is the Phase-2 carry Task 6 discharges.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <memory>

struct ImDrawData;

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

        // The GRAPH flavor: no device, no ShaderLibrary, no NVRHI renderer.
        // Window must outlive the layer (the platform backend reads its size
        // every BeginFrame -- that is what makes io.DisplaySize the HOST
        // window's extent, i.e. the same surface the graph swapchain binds).
        //
        // DELIBERATELY INSTALLS NO EVENT TAP -- see the .cpp. The HUD stays
        // non-interactive on the graph path until after desk checkpoint D3b.
        static std::unique_ptr<ImGuiLayer> CreateForGraph(Window& window);

        virtual ~ImGuiLayer() = default;

        // sdl3 NewFrame + ImGui::NewFrame. Caller MUST pair every BeginFrame
        // with exactly one Render / RenderToDrawData / EndFrameDiscard; a
        // double-Begin trips ImGui's own asserts.
        virtual void BeginFrame() = 0;
        // ImGui::Render + draw into target (an OPEN command list). NVRHI
        // flavor only: the graph flavor built no renderer and refuses.
        virtual void Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target) = 0;

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
