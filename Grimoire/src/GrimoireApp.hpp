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
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/PickBuffer.hpp>

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

        // Editor undo/redo history (Edit-mode; cleared on Play). Constructed in
        // Init once the runtime's registry exists; optional so it can be built
        // after m_runtime. Declared AFTER m_runtime/m_plugin so it destructs
        // BEFORE them -- its resolver lambda captures `&*m_runtime` (a raw
        // Runtime*, dereferenced fresh each call), so it must not outlive it.
        std::optional<Arcane::CommandStack> m_undo;

        // Ctrl+Z/Ctrl+Shift+Z/Ctrl+Y edge-tracking for the undo/redo keybinds
        // (InputSnapshot only reports held-state, not rising-edge; tracked here
        // across frames -- see MainLoop's input block).
        bool m_prevUndoKeyDown = false;
        bool m_prevRedoKeyDown = false;

        // Transform-gizmo state (Edit-mode; drives the Viewport handles). Mode/
        // space persist across frames and selection changes; m_gizmoHovered is
        // recomputed every frame the gizmo is interactable (cleared otherwise) so
        // Draw's highlight always matches this frame's cursor. m_gizmoDrag spans
        // the mouse-down..mouse-up gesture; `start` is the pre-drag GizmoTransform
        // so ApplyDrag recomputes from origin each frame (no accumulation drift).
        Arcane::GizmoMode  m_gizmoMode    = Arcane::GizmoMode::Translate;
        Arcane::GizmoSpace m_gizmoSpace   = Arcane::GizmoSpace::World;
        Arcane::GizmoAxis  m_gizmoHovered = Arcane::GizmoAxis::None;
        struct GizmoDrag
        {
            bool                   active = false;
            Arcane::GizmoAxis      axis   = Arcane::GizmoAxis::None;
            Arcane::GizmoTransform start;
            glm::vec2              mouseStartScreen{0.0f, 0.0f};
        } m_gizmoDrag;

        // W/E/R mode-key edge-tracking (same pattern as m_prevUndoKeyDown/
        // m_prevRedoKeyDown above).
        bool m_prevKeyW = false;
        bool m_prevKeyE = false;
        bool m_prevKeyR = false;
        // Left-mouse edge-tracking, shared by the gizmo press/release detection.
        bool m_prevLmbDown = false;
        // Set for the remainder of THIS frame when a gizmo drag starts or ends,
        // so the click-pick block (later in MainLoop) does not also treat the
        // same click as a selection change. Reset at the top of the input block
        // each frame.
        bool m_gizmoCapturedClick = false;

        // Scene-in-a-panel viewport: the same canvas->batcher->tonemap path Loom
        // drives the backbuffer with, rendered into a panel texture instead. Resized
        // to the Viewport panel's content region each frame; input into the plugin
        // is gated on m_viewportActive and remapped through m_viewportRect (see
        // ViewportInput.hpp + MainLoop's input block).
        std::unique_ptr<Arcane::OffscreenCanvas> m_viewport;
        Grimoire::ViewportRect                   m_viewportRect{};
        bool                                      m_viewportActive = false;

        // GPU hit-proxy picker, a sibling of m_viewport: created and resized at the
        // same size, it renders each pickable entity's silhouette into an R32_UINT
        // id buffer and reads back the pixel under a viewport click to select the
        // entity there (sprites + physics colliders; front-most wins). Replaces the
        // CPU sprite-OBB PickEntitiesAt. See PickBuffer.hpp.
        std::unique_ptr<Arcane::PickBuffer>       m_pick;

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
