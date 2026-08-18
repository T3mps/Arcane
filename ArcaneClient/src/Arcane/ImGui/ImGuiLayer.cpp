#include <Arcane/ImGui/ImGuiLayer.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Window.hpp>

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
                // Order mirrors Init in reverse: uninstall the tap first so no
                // stray event reaches a half-torn-down context.
                m_window->SetNativeEventTap(nullptr, nullptr);
                ImGui_ImplSDL3_Shutdown();
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }

            // The context, the SDL3 platform backend, and the event tap --
            // ONE FLAVOR (NRI Phase 5a, Task 5). The tap used to be withheld
            // on one of two flavors (a time-boxed stance from NRI Phase 3
            // Task 6 step 3, closed at D3 exit 2026-08-18); that whole axis
            // of variation is gone now, along with the flavor it could rot
            // on unseen. See the Phase 3 milestone record for what leaving it
            // one checkpoint too long cost: an entire desk checkpoint's worth
            // of unclickable editor, found by one human click after 1015
            // green test cases, 12 golden compares and the NVRHI floor all
            // missed it, because no scripted host run drives real input.
            bool Init()
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

                m_window->SetNativeEventTap(&ImGuiLayerImpl::Tap, this);
                return true;
            }

            void BeginFrame() override
            {
                // Once a second (offscreen) context exists, BeginFrame must not
                // run on whatever context another layer last left current.
                ImGui::SetCurrentContext(m_context);
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
            }

            ImDrawData* RenderToDrawData() override
            {
                // THE PIN IS THE POINT (see the header). ImGui::Render() ends
                // the frame itself, so the BeginFrame/RenderToDrawData pairing
                // holds here regardless of what was current before this call.
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

            Window*       m_window  = nullptr;
            ImGuiContext* m_context = nullptr;
        };
    }

    std::unique_ptr<ImGuiLayer> ImGuiLayer::Create(Window& window)
    {
        auto layer = std::make_unique<ImGuiLayerImpl>(window);
        if (!layer->Init())
            return nullptr;
        return layer;
    }
}
