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
// runtime/plugin (which Loom owns + declares after a GpuContext); commandList +
// framebuffers LAST (release their NVRHI handles before device). Do NOT reorder.

#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/Swapchain.hpp>
#include <Arcane/Render/TonemapPass.hpp>

#include <LoomConfig.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

class GpuContext
{
public:
    // Runs the ordered boot into the members. Returns null (with ARC_ERROR on
    // the failing step) when any stage fails -- the partial unwinds via RAII in
    // reverse member order, the same order ~GpuContext uses at shutdown.
    static std::unique_ptr<GpuContext> Create(const LoomConfig& cfg);

    // Resize the presentation surface + scene target. Mirrors main's old inline
    // resize: drop the cached backbuffer framebuffers, resize the swapchain to
    // the event extent, then resize the canvas to the swapchain's ACTUAL extent
    // (which may differ from the requested size on Vulkan/Win32).
    void OnResize(std::uint32_t w, std::uint32_t h);

    // Lazily builds + caches the backbuffer framebuffer for `backbuffer`.
    nvrhi::FramebufferHandle& FramebufferFor(nvrhi::ITexture* backbuffer);

    // Accessors return references to the owned objects (or the bare nvrhi
    // command-list handle, which is already pointer-like).
    Arcane::Window&        Win()       { return m_window; }
    Arcane::RenderDevice&  Device()    { return *m_device; }
    Arcane::Swapchain&     Swap()      { return *m_swapchain; }
    Arcane::ShaderLibrary& Shaders()   { return *m_shaders; }
    Arcane::Canvas&        Cnv()       { return *m_canvas; }
    Arcane::Batcher2D&     Batch()     { return *m_batcher; }
    Arcane::TonemapPass&   Tone()      { return *m_tonemap; }
    Arcane::ImGuiLayer&    Imgui()     { return *m_imgui; }
    Arcane::InputDevices&  InDevices() { return *m_inputDevices; }
    Arcane::InputActions&  Input()     { return *m_input; }
    nvrhi::ICommandList*   Cmd()       { return m_commandList; }

private:
    GpuContext() = default;

    // --- TEARDOWN CONTRACT: declaration order == reverse destruction order ---
    // Do NOT reorder. See the file-header comment for the why of each position.

    // Window first: it destructs LAST (imgui + inputDevices tap the SDL window).
    Arcane::Window m_window;

    // Render stack: outlives the runtime/plugin (declared after a GpuContext in
    // Loom) so PluginHost teardown runs while these are still alive.
    std::unique_ptr<Arcane::RenderDevice>  m_device;
    std::unique_ptr<Arcane::Swapchain>     m_swapchain;
    std::unique_ptr<Arcane::ShaderLibrary> m_shaders;
    std::unique_ptr<Arcane::Canvas>        m_canvas;
    std::unique_ptr<Arcane::Batcher2D>     m_batcher;
    std::unique_ptr<Arcane::TonemapPass>   m_tonemap;
    std::unique_ptr<Arcane::ImGuiLayer>    m_imgui;
    std::unique_ptr<Arcane::InputDevices>  m_inputDevices;
    std::unique_ptr<Arcane::InputActions>  m_input;

    // commandList + framebuffers LAST: they hold NVRHI handles that must release
    // before m_device (whose teardown uses the device's native handles).
    nvrhi::CommandListHandle m_commandList;
    std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> m_framebuffers;
};
