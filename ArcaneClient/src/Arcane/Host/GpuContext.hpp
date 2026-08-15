#pragma once

// GpuContext: the host-owned platform/render/input stack, extracted out of
// main.cpp so the boot sequence + the (load-bearing) teardown order live in one
// place. Create() runs the EXACT ordered boot (window -> device -> swapchain ->
// shaders -> canvas -> batcher -> tonemap -> imgui -> inputDevices -> input ->
// commandList); on any fallible step it logs ARC_ERROR + returns nullptr, and
// the partially-built unique_ptr<GpuContext> unwinds (RAII) in reverse member
// declaration order -- the same order the destructor would use at shutdown.
//
// MEMBER DECLARATION ORDER IS THE TEARDOWN CONTRACT -- window first (destructs
// LAST: imgui/input hold SDL window refs); render resources outlive
// runtime/plugin (which a host owns + declares after a GpuContext); commandList +
// framebuffers LAST (release their NVRHI handles before device). Do NOT reorder.
//
// =====================================================================
// TWO FLAVORS, ONE CLASS (NRI Phase 3, Task 6 -- plan reconciliation 6)
// =====================================================================
// CreateForGraph() builds the SAME object with its NVRHI half absent: no
// RenderDevice, no Swapchain, no ShaderLibrary, no Canvas, no TonemapPass, no
// command list, no GpuFrameProgress. What it DOES build is everything the
// frame's data-supply side and the host's platform surface need -- the window
// (which the NRI swapchain then binds: ONE window per process, DXGI permits
// one flip-model swapchain per HWND), a DEVICE-LESS Batcher2D, the graph
// flavor of ImGuiLayer, and the input stack.
//
// The NVRHI accessors below are then UNREACHABLE, and they say so: each one
// ARC_ASSERTs (fatal in Debug) and, in an optimized build, hands back null
// rather than dereferencing a null member. That shape rather than a second
// class because all ~130 call sites already live on paths that are mode-gated
// (the NVRHI render arm; the editor's NVRHI phases) -- a fork would duplicate
// the boot order and the teardown contract, which are the two things this file
// exists to keep singular. The split is cleaned up at Phase 5, when the NVRHI
// members die for real.
//
// GraphFlavor() is the ONE predicate every host branch keys off. Prefer it to
// re-reading HostConfig::nriGraph: what matters at a call site is whether the
// members exist, not which flag caused that.

#include <Arcane/Base/Api.hpp>
#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/Swapchain.hpp>
#include <Arcane/Render/TonemapPass.hpp>

#include <Arcane/Host/HostConfig.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace Arcane
{
    class ARCANE_API GpuContext
    {
    public:
        // Runs the ordered boot into the members. Returns null (with ARC_ERROR on
        // the failing step) when any stage fails -- the partial unwinds via RAII in
        // reverse member order, the same order ~GpuContext uses at shutdown.
        static std::unique_ptr<GpuContext> Create(const HostConfig& cfg);

        // The GRAPH flavor (NRI Phase 3, Task 6): window + device-less
        // Batcher2D + graph ImGuiLayer + input, and NOTHING NVRHI. The caller
        // then builds an NriGraphContext over Win() -- that object owns the
        // only graphics device in the process.
        //
        // The window is created HIDDEN, exactly as Create() does, and the
        // caller reveals it (Window::Show) once the graph vehicle exists.
        static std::unique_ptr<GpuContext> CreateForGraph(const HostConfig& cfg);

        // Which flavor this is. True == the NVRHI members were never
        // constructed and every accessor marked "NVRHI half" below is
        // unreachable.
        [[nodiscard]] bool GraphFlavor() const noexcept { return m_graphFlavor; }

        // Resize the presentation surface + scene target. Mirrors main's old inline
        // resize: drop the cached backbuffer framebuffers, resize the swapchain to
        // the event extent, then resize the canvas to the swapchain's ACTUAL extent
        // (which may differ from the requested size on Vulkan/Win32).
        //
        // NVRHI half. On the graph flavor the presentation surface belongs to
        // NriGraphContext and NriGraphContext::Resize is the whole of it.
        void OnResize(std::uint32_t w, std::uint32_t h);

        // Lazily builds + caches the backbuffer framebuffer for `backbuffer`.
        // NVRHI half.
        nvrhi::FramebufferHandle& FramebufferFor(nvrhi::ITexture* backbuffer);

        // The linear post buffer for the scene post chain (post arc): built lazily
        // at the first posted frame and resized here to track the canvas, so a
        // chainless run never allocates it. Null on creation failure (the caller
        // falls back to tonemapping the canvas directly). NVRHI half.
        Canvas* EnsurePost();

        // Accessors return references to the owned objects (or the bare nvrhi
        // command-list handle, which is already pointer-like). The ones marked
        // "NVRHI half" are unreachable on the graph flavor -- ARC_ASSERT, then
        // null in an optimized build.
        Window&        Win()       { return m_window; }
        RenderDevice&  Device()    { AssertNvrhi("Device");  return *m_device; }        // NVRHI half
        Swapchain&     Swap()      { AssertNvrhi("Swap");    return *m_swapchain; }     // NVRHI half
        ShaderLibrary& Shaders()   { AssertNvrhi("Shaders"); return *m_shaders; }       // NVRHI half
        Canvas&        Cnv()       { AssertNvrhi("Cnv");     return *m_canvas; }        // NVRHI half
        Batcher2D&     Batch()     { return *m_batcher; }
        TonemapPass&   Tone()      { AssertNvrhi("Tone");    return *m_tonemap; }       // NVRHI half
        ImGuiLayer&    Imgui()     { return *m_imgui; }
        InputDevices&  InDevices() { return *m_inputDevices; }
        InputActions&  Input()     { return *m_input; }
        nvrhi::ICommandList*   Cmd()       { AssertNvrhi("Cmd"); return m_commandList; } // NVRHI half

        // GPU-progress heartbeat source (GPU crash diagnostics arc, Task 7).
        // Hosts call FrameProgress().EndFrame() once per frame, after the
        // frame's last submit. Lives here because it is per-device state both
        // hosts need and neither should own a private copy of.
        //
        // NVRHI half: the graph path's heartbeat is published from
        // NriGraphContext::RenderFrame through NriDiagnostics::PublishHeartbeat
        // (the pacing fence's completed value), which is the 1:1 replacement.
        GpuFrameProgress& FrameProgress() { AssertNvrhi("FrameProgress"); return *m_frameProgress; }

    private:
        GpuContext() = default;

        // The one gate behind every "NVRHI half" accessor. Out-of-line (the
        // .cpp) so this header does not have to pull in the assert seam.
        void AssertNvrhi(const char* accessor) const;

        // --- TEARDOWN CONTRACT: declaration order == reverse destruction order ---
        // Do NOT reorder. See the file-header comment for the why of each position.

        // Window first: it destructs LAST (imgui + inputDevices tap the SDL window).
        // On the graph flavor it also outlives the borrowing NriGraphContext,
        // which the host guarantees by declaring that object AFTER its GpuContext.
        Window m_window;

        // Render stack: outlives the runtime/plugin (declared after a GpuContext in
        // the host) so PluginHost teardown runs while these are still alive.
        // Every one of these except m_batcher/m_imgui stays NULL on the graph
        // flavor.
        std::unique_ptr<RenderDevice>  m_device;
        std::unique_ptr<Swapchain>     m_swapchain;
        std::unique_ptr<ShaderLibrary> m_shaders;
        std::unique_ptr<Canvas>        m_canvas;
        std::unique_ptr<Canvas>        m_post;      // lazy -- see EnsurePost
        std::unique_ptr<Batcher2D>     m_batcher;
        std::unique_ptr<TonemapPass>   m_tonemap;
        std::unique_ptr<ImGuiLayer>    m_imgui;
        std::unique_ptr<InputDevices>  m_inputDevices;
        std::unique_ptr<InputActions>  m_input;

        // commandList + framebuffers LAST: they hold NVRHI handles that must release
        // before m_device (whose teardown uses the device's native handles).
        // frameProgress joins them for the same reason -- it owns a chain of
        // nvrhi::EventQueryHandles created off m_device.
        nvrhi::CommandListHandle m_commandList;
        std::unique_ptr<GpuFrameProgress> m_frameProgress;
        std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> m_framebuffers;
        // What FramebufferFor hands back on the graph flavor in an optimized
        // build: a permanently null handle, so the reference it must return by
        // signature names something real. Never written.
        nvrhi::FramebufferHandle m_nullFramebuffer;

        bool m_graphFlavor = false;
    };
}
