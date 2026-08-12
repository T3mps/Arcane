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

        // Resize the presentation surface + scene target. Mirrors main's old inline
        // resize: drop the cached backbuffer framebuffers, resize the swapchain to
        // the event extent, then resize the canvas to the swapchain's ACTUAL extent
        // (which may differ from the requested size on Vulkan/Win32).
        void OnResize(std::uint32_t w, std::uint32_t h);

        // Lazily builds + caches the backbuffer framebuffer for `backbuffer`.
        nvrhi::FramebufferHandle& FramebufferFor(nvrhi::ITexture* backbuffer);

        // The linear post buffer for the scene post chain (post arc): built lazily
        // at the first posted frame and resized here to track the canvas, so a
        // chainless run never allocates it. Null on creation failure (the caller
        // falls back to tonemapping the canvas directly).
        Canvas* EnsurePost();

        // Accessors return references to the owned objects (or the bare nvrhi
        // command-list handle, which is already pointer-like).
        Window&        Win()       { return m_window; }
        RenderDevice&  Device()    { return *m_device; }
        Swapchain&     Swap()      { return *m_swapchain; }
        ShaderLibrary& Shaders()   { return *m_shaders; }
        Canvas&        Cnv()       { return *m_canvas; }
        Batcher2D&     Batch()     { return *m_batcher; }
        TonemapPass&   Tone()      { return *m_tonemap; }
        ImGuiLayer&    Imgui()     { return *m_imgui; }
        InputDevices&  InDevices() { return *m_inputDevices; }
        InputActions&  Input()     { return *m_input; }
        nvrhi::ICommandList*   Cmd()       { return m_commandList; }

        // GPU-progress heartbeat source (GPU crash diagnostics arc, Task 7).
        // Hosts call FrameProgress().EndFrame() once per frame, after the
        // frame's last submit. Lives here because it is per-device state both
        // hosts need and neither should own a private copy of.
        GpuFrameProgress& FrameProgress() { return *m_frameProgress; }

    private:
        GpuContext() = default;

        // --- TEARDOWN CONTRACT: declaration order == reverse destruction order ---
        // Do NOT reorder. See the file-header comment for the why of each position.

        // Window first: it destructs LAST (imgui + inputDevices tap the SDL window).
        Window m_window;

        // Render stack: outlives the runtime/plugin (declared after a GpuContext in
        // the host) so PluginHost teardown runs while these are still alive.
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
    };
}
