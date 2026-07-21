#include <Arcane/ImGui/ImGuiLayer.hpp>

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
                // Order mirrors Create in reverse: uninstall the tap first so
                // no stray event reaches a half-torn-down context.
                m_window->SetNativeEventTap(nullptr, nullptr);
                m_renderer.Shutdown();
                ImGui_ImplSDL3_Shutdown();
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }

            bool Init(RenderDevice& device, ShaderLibrary& shaders)
            {
                IMGUI_CHECKVERSION();
                m_context = ImGui::CreateContext();
                if (!m_context)
                {
                    ARC_ERROR("ImGui::CreateContext failed");
                    return false;
                }

                if (!ImGui_ImplSDL3_InitForOther(m_window->SdlWindow()))
                {
                    ARC_ERROR("ImGui_ImplSDL3_InitForOther failed");
                    ImGui::DestroyContext(m_context);
                    m_context = nullptr;
                    return false;
                }

                if (!m_renderer.Init(device.Nvrhi(), shaders))
                {
                    ARC_ERROR("ImGuiNvrhiRenderer::Init failed");
                    ImGui_ImplSDL3_Shutdown();
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

            void Render(nvrhi::ICommandList* commandList,
                        nvrhi::IFramebuffer* target) override
            {
                ImGui::SetCurrentContext(m_context);
                ImGui::Render();
                m_renderer.RenderDrawData(ImGui::GetDrawData(), commandList,
                                          target);
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
            static void Tap(const void* event, void* /*user*/)
            {
                ImGui_ImplSDL3_ProcessEvent((const SDL_Event*)event);
            }

            Window*             m_window  = nullptr;
            ImGuiContext*       m_context = nullptr;
            ImGuiNvrhiRenderer  m_renderer;
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
}
