#pragma once

// GpuContext: the host-owned platform/render/input stack, extracted out of
// main.cpp so the boot sequence + the (load-bearing) teardown order live in one
// place. Create() runs the ordered boot (window -> device-less batcher ->
// imgui -> inputDevices -> input); on any fallible step it logs ARC_ERROR +
// returns nullptr, and the partially-built unique_ptr<GpuContext> unwinds
// (RAII) in reverse member declaration order -- the same order the destructor
// would use at shutdown.
//
// MEMBER DECLARATION ORDER IS THE TEARDOWN CONTRACT -- window first (destructs
// LAST: imgui/input hold SDL window refs); the render/input stack outlives
// runtime/plugin (which a host owns + declares after a GpuContext). Do NOT
// reorder.
//
// =====================================================================
// ONE FLAVOR (NRI Phase 5a, Task 6)
// =====================================================================
// This class briefly carried two flavors (NRI Phase 3, Task 6): a default
// NVRHI half (RenderDevice/Swapchain/ShaderLibrary/Canvas/TonemapPass/command
// list/GpuFrameProgress) and a device-less "graph" half (this one). The NVRHI
// factory had zero callers anywhere in the tree by the time this task landed
// -- every host went through CreateForGraph -- so the split promised to
// resolve "at Phase 5" is discharged here: the NVRHI half is deleted outright
// rather than ported, and CreateForGraph is renamed to Create, the one
// factory left.
//
// Create() builds the window (which the caller's NRI render vehicle then
// binds: ONE window per process, DXGI permits one flip-model swapchain per
// HWND), a DEVICE-LESS Batcher2D, ImGuiLayer, and the input stack. Nothing
// else -- there is no RenderDevice, Swapchain, ShaderLibrary, Canvas or
// command list anywhere in this object.
//
// THERE IS NO FLAVOR PREDICATE ANY MORE. This class carried a GraphFlavor()
// accessor over a m_graphFlavor member through the whole NRI port; Create()
// set it true before its first fallible step and the constructor is private,
// so it had answered true unconditionally since the Phase 5a flip. It was
// retired in the follow-on collapse that also deleted BootPresenter (its two
// `!GraphFlavor()` gates were that class's only remaining construction
// sites), because retiring it MOVES the test gate -- ArcaneTests asserted on
// it twice -- which the phase's own tasks were forbidden to do.

#include <Arcane/Base/Api.hpp>
#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>

#include <Arcane/Host/HostConfig.hpp>

#include <memory>

namespace Arcane
{
    class ARCANE_API GpuContext
    {
    public:
        // Runs the ordered boot into the members. Returns null (with ARC_ERROR on
        // the failing step) when any stage fails -- the partial unwinds via RAII in
        // reverse member order, the same order ~GpuContext uses at shutdown.
        //
        // The window is created HIDDEN; the caller reveals it (Window::Show)
        // once the render vehicle that owns this window's only swapchain
        // exists.
        static std::unique_ptr<GpuContext> Create(const HostConfig& cfg);

        Window&        Win()       { return m_window; }
        Batcher2D&     Batch()     { return *m_batcher; }
        ImGuiLayer&    Imgui()     { return *m_imgui; }
        InputDevices&  InDevices() { return *m_inputDevices; }
        InputActions&  Input()     { return *m_input; }

    private:
        GpuContext() = default;

        // --- TEARDOWN CONTRACT: declaration order == reverse destruction order ---
        // Do NOT reorder. See the file-header comment for the why of each position.

        // Window first: it destructs LAST (imgui + inputDevices tap the SDL window).
        // It also outlives the borrowing render vehicle, which the host guarantees
        // by declaring that object AFTER its GpuContext.
        Window m_window;

        std::unique_ptr<Batcher2D>     m_batcher;
        std::unique_ptr<ImGuiLayer>    m_imgui;
        std::unique_ptr<InputDevices>  m_inputDevices;
        std::unique_ptr<InputActions>  m_input;
    };
}
