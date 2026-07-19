#pragma once
// GrimoireApp: the editor application. Constructed in main from a LoomConfig
// (reused as the host config); Run() drives Init -> the frame loop -> Shutdown.
// Member declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN
// CONTRACT (destruct reverse: plugin Unload while the DLL is still mapped ->
// runtime -> render stack in GpuContext -> window last). Mirrors Loom.
#include <cstdint>
#include <memory>
#include <optional>

#include <LoomConfig.hpp>
#include <GpuContext.hpp>
#include <FramePerf.hpp>
#include "ConsoleBuffer.hpp"
#include "PlayMode.hpp"
#include "SelectionContext.hpp"
#include "ViewportInput.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>

#include <spdlog/sinks/callback_sink.h>

namespace Astra { class TypeContext; }

namespace Grimoire
{
    class GrimoireApp
    {
    public:
        explicit GrimoireApp(LoomConfig cfg);
        int Run();   // Init() -> MainLoop() -> Shutdown(); process exit code

    private:
        bool Init();
        void MainLoop();
        void Shutdown();
        void InstallConsoleSink();   // attach a callback sink on Arcane::Log::Engine() -> m_console

        LoomConfig                        m_config;
        std::unique_ptr<GpuContext>       m_gpu;                    // destructs LAST
        Astra::TypeContext*               m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
        std::optional<Arcane::Runtime>    m_runtime;                // destructs before m_gpu
        std::optional<Arcane::PluginHost> m_plugin;                 // destructs before m_runtime
        FramePerf                         m_perf;
        std::uint64_t                     m_frameCount = 0;
        ConsoleBuffer                     m_console{512};
        // Handle to the callback sink pushed onto Arcane::Log::Engine() in
        // InstallConsoleSink(); erased at the top of Shutdown() so the sink cannot
        // fire (and Push into a destroyed m_console) during member teardown -- e.g.
        // ~GpuContext's Vulkan device destruction logs validation messages.
        std::shared_ptr<spdlog::sinks::callback_sink_mt> m_consoleSink;

        // Play-in-editor (Task 8): Edit|Play state machine. Play() snapshots the
        // registry + unpauses the RunLoop; Stop() restores the snapshot + re-pauses.
        // Grimoire boots in Edit (see Init: the RunLoop is paused right after the
        // plugin loads).
        Grimoire::PlaySession m_play;

        // The one selected-entity source of truth, shared by the Hierarchy panel
        // (and, later, the Inspector + viewport pick -- see SelectionContext.hpp).
        Grimoire::SelectionContext m_selection;

        // Scene-in-a-panel viewport: the same canvas->batcher->tonemap path Loom
        // drives the backbuffer with, rendered into a panel texture instead. Resized
        // to the Viewport panel's content region each frame; input into the plugin
        // is gated on m_viewportActive and remapped through m_viewportRect (see
        // ViewportInput.hpp + MainLoop's input block).
        std::unique_ptr<Arcane::OffscreenCanvas> m_viewport;
        Grimoire::ViewportRect                   m_viewportRect{};
        bool                                      m_viewportActive = false;

        // Deferred resize: the Viewport panel's content-region size measured LAST
        // frame, applied at the START of THIS frame (before m_viewport->Draw). This
        // mirrors the m_viewportRect/m_viewportActive one-frame lag above -- it avoids
        // a same-frame use-after-free where OffscreenCanvas::Resize (called right after
        // ImGui::Image bakes the current texture pointer into this frame's draw list)
        // synchronously frees that very texture before ImGui replays the draw list at
        // Render time. See MainLoop for the full sequencing rationale.
        std::uint32_t m_pendingViewportW = 0;
        std::uint32_t m_pendingViewportH = 0;
    };
}
