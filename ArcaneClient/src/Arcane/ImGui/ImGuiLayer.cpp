#include <Arcane/ImGui/ImGuiLayer.hpp>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/ImGui/ImGuiNvrhi.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include <SDL3/SDL.h>

namespace Arcane
{
    namespace
    {
        class ImGuiLayerImpl final : public ImGuiLayer
        {
        public:
            ImGuiLayerImpl(Window& window) : m_window(&window) {}

            ~ImGuiLayerImpl() override
            {
                if (!m_context)
                    return;
                // self-pin: teardown runs on the current context (two-context safety).
                ImGui::SetCurrentContext(m_context);
                // Order mirrors Create in reverse: uninstall the tap first so
                // no stray event reaches a half-torn-down context. (Nothing to
                // uninstall on the graph flavor -- it never installed one --
                // but clearing an already-clear tap is a no-op, and one
                // teardown order for both flavors is cheaper to keep correct
                // than a per-flavor exception.)
                m_window->SetNativeEventTap(nullptr, nullptr);
                // GATED, unlike the two calls around it: ImGuiNvrhiRenderer::
                // Shutdown walks ImGui::GetPlatformIO().Textures and marks
                // every entry it can reach Destroyed. On the graph flavor
                // those entries belong to ImGuiNri (the graph node's backend),
                // so running a renderer that was never Init'd would reach into
                // another backend's bookkeeping for no gain.
                if (m_hasNvrhiRenderer)
                    m_renderer.Shutdown();
                ImGui_ImplSDL3_Shutdown();
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }

            // The half both flavors share: the context and the SDL3 platform
            // backend. `installTap` and the renderer are what differ.
            bool InitCommon(bool installTap)
            {
                IMGUI_CHECKVERSION();
                m_context = ImGui::CreateContext();   // own atlas; a PRIOR context stays current, so we pin m_context explicitly below
                if (!m_context)
                {
                    ARC_ERROR("ImGui::CreateContext failed");
                    return false;
                }
                // THE PIN, and it is not decoration: ImGui::CreateContext
                // RESTORES whatever context was current if there was one
                // (imgui.cpp's CreateContext -- "Restore previous context if
                // any, else keep new one"). Without this line the
                // ImGui_ImplSDL3_InitForOther below installs the platform
                // backend on a FOREIGN context -- either tripping its own
                // "Already initialized a platform backend!" assert, or leaving
                // THIS layer's context with no platform backend at all, which
                // aborts on the first BeginFrame. Neither host can reach that
                // today (GpuContext is the first context creator in both), but
                // this class's contract is that EVERY entry point pins before
                // touching ImGui state, and Init is an entry point.
                //
                // UNLIKE OffscreenImGuiLayer::Init -- which pins the same way
                // and then RESTORES the previous context -- this one
                // deliberately LEAVES m_context current. It is the host's
                // PRIMARY context, and both hosts read
                // ImGui::GetCurrentContext() straight after building their
                // GpuContext to publish it across the plugin ABI
                // (RuntimeApp::StageRenderBridge). The offscreen layer is the
                // SECOND context in the process and must never steal currency;
                // this one is the first and is required to hold it.
                ImGui::SetCurrentContext(m_context);

                if (!ImGui_ImplSDL3_InitForOther(m_window->SdlWindow()))
                {
                    ARC_ERROR("ImGui_ImplSDL3_InitForOther failed");
                    ImGui::DestroyContext(m_context);
                    m_context = nullptr;
                    return false;
                }

                if (installTap)
                    m_window->SetNativeEventTap(&ImGuiLayerImpl::Tap, this);
                return true;
            }

            bool Init(RenderDevice& device, ShaderLibrary& shaders)
            {
                if (!InitCommon(/*installTap=*/true))
                    return false;

                if (!m_renderer.Init(device.Nvrhi(), shaders))
                {
                    ARC_ERROR("ImGuiNvrhiRenderer::Init failed");
                    m_window->SetNativeEventTap(nullptr, nullptr);
                    ImGui_ImplSDL3_Shutdown();
                    ImGui::DestroyContext(m_context);
                    m_context = nullptr;
                    return false;
                }
                m_hasNvrhiRenderer = true;
                return true;
            }

            bool InitForGraph()
            {
                // ===========================================================
                // THE EVENT TAP IS INSTALLED ON BOTH FLAVORS AGAIN (D3 exit,
                // 2026-08-18). The withholding below was a TIME-BOXED stance
                // from NRI Phase 3 Task 6 step 3, and its box has closed.
                // ===========================================================
                // WHY IT WAS WITHHELD: before the landing the HUD was
                // non-interactive on the graph path as a SIDE EFFECT of the
                // two-window topology -- the tap sat on the host window while
                // the user's events went to the vehicle's. One window makes
                // that accident impossible, so the stance had to be STATED to
                // be kept, and it was to be kept "until desk checkpoint D3b's
                // compares are done", because:
                //
                //   an interactive HUD can be DRAGGED, and ImGui persists
                //   window placement per exe dir in imgui.ini. The graph path
                //   and the NVRHI path share that file, so one drag on a graph
                //   run would silently move the HUD on the NVRHI path too --
                //   i.e. it would change the very `full` baseline D3b compares
                //   against. A HUD that renders identically and cannot be moved
                //   is exactly what a golden comparison wants.
                //
                // WHY IT IS OVER: D3b closed green 2026-08-18 (run 5) and D3c
                // before it; BOTH golden sets are frozen and committed (runtime
                // main-* @c131692f, editor editor-* @db648b4f). The condition
                // this stance named has been met, so the argument flips exactly
                // as the old comment said it would: InitCommon(true).
                //
                // WHAT IT COST TO LEAVE IT ONE CHECKPOINT TOO LONG, recorded so
                // the next time-boxed stance carries an expiry that is CHECKED:
                // the tap is the ONLY route by which mouse BUTTON events reach
                // this context (imgui_impl_sdl3 turns SDL_EVENT_MOUSE_BUTTON_*
                // into AddMouseButtonEvent; nothing polls button state), and
                // this is the editor's PRIMARY context -- its whole chrome, not
                // just the HUD. So a --nri-graph editor was completely
                // unclickable: menus, panels, viewport pick, gizmo, all of it.
                // No headless gate could see it (ArcaneTests never drives a
                // real cursor) and no golden could either (a golden run has no
                // input at all); it took the D3 drive checklist's first human
                // click to surface. That is precisely what that checklist is
                // for -- see the Phase 3 milestone record.
                return InitCommon(/*installTap=*/true);
            }

            void BeginFrame() override
            {
                // Once a second (offscreen) context exists, BeginFrame must not
                // run on whatever context another layer last left current.
                ImGui::SetCurrentContext(m_context);
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
            }

            void Render(nvrhi::ICommandList* commandList,
                        nvrhi::IFramebuffer* target) override
            {
                // Recon 6's rule for a member that was never constructed: assert
                // (fatal in Debug), and in an optimized build do nothing rather
                // than record through a renderer with no device. The frame is
                // still ENDED, so ImGuiLayer's "every BeginFrame is paired
                // exactly once" contract survives the mistake.
                ARC_ASSERT(m_hasNvrhiRenderer,
                           "ImGuiLayer::Render on the GRAPH flavor -- that flavor builds no NVRHI "
                           "renderer; the graph's ImGuiNriNode draws RenderToDrawData()'s output");
                if (!m_hasNvrhiRenderer)
                {
                    (void)RenderToDrawData();
                    return;
                }
                ImGui::SetCurrentContext(m_context);
                ImGui::Render();
                m_renderer.RenderDrawData(ImGui::GetDrawData(), commandList,
                                          target);
            }

            ImDrawData* RenderToDrawData() override
            {
                // THE PIN IS THE POINT (see the header). ImGui::Render() ends
                // the frame itself, so the BeginFrame/Render pairing holds here
                // exactly as it does above.
                ImGui::SetCurrentContext(m_context);
                ImGui::Render();
                return ImGui::GetDrawData();
            }

            void EndFrameDiscard() override
            {
                ImGui::SetCurrentContext(m_context);
                ImGui::EndFrame();
            }

            bool WantCaptureKeyboard() const override
            {
                // Queries GImGui's current-context IO; with a second (offscreen)
                // context possibly left current by another layer's BeginFrame/
                // Render, this must pin the editor context before reading.
                ImGui::SetCurrentContext(m_context);
                return ImGui::GetIO().WantCaptureKeyboard;
            }

            bool WantCaptureMouse() const override
            {
                ImGui::SetCurrentContext(m_context);
                return ImGui::GetIO().WantCaptureMouse;
            }

        private:
            // The native event tap: the platform backend consumes SDL events
            // for input. `event` is a const SDL_Event* (see Window.hpp).
            // Pin the editor context first: it alone owns the SDL3 platform
            // backend. A second (offscreen) context has no SDL3 backend, and a
            // plugin's Init (which adopts its context) or the game ImGui pass can
            // leave THAT context current when an OS event is pumped -- on which
            // ImGui_ImplSDL3_ProcessEvent would assert (bd == nullptr).
            static void Tap(const void* event, void* user)
            {
                auto* self = static_cast<ImGuiLayerImpl*>(user);
                ImGui::SetCurrentContext(self->m_context);
                ImGui_ImplSDL3_ProcessEvent((const SDL_Event*)event);
            }

            Window*             m_window  = nullptr;
            ImGuiContext*       m_context = nullptr;
            ImGuiNvrhiRenderer  m_renderer;
            // False on the graph flavor: m_renderer was never Init'd, so it
            // must neither draw nor be shut down.
            bool                m_hasNvrhiRenderer = false;
        };
    }

    std::unique_ptr<ImGuiLayer> ImGuiLayer::Create(Window& window,
                                                   RenderDevice& device,
                                                   ShaderLibrary& shaders)
    {
        auto layer = std::make_unique<ImGuiLayerImpl>(window);
        if (!layer->Init(device, shaders))
            return nullptr;
        return layer;
    }

    std::unique_ptr<ImGuiLayer> ImGuiLayer::CreateForGraph(Window& window)
    {
        auto layer = std::make_unique<ImGuiLayerImpl>(window);
        if (!layer->InitForGraph())
            return nullptr;
        return layer;
    }
}
