#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/ImGui/ImGuiNvrhi.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <imgui.h>

#include <cfloat>

namespace Arcane
{
    namespace
    {
        class OffscreenImGuiLayerImpl final : public OffscreenImGuiLayer
        {
        public:
            ~OffscreenImGuiLayerImpl() override
            {
                if (!m_context)
                    return;
                ImGuiContext* prev = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(m_context);
                m_renderer.Shutdown();
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }

            bool Init(RenderDevice& device, ShaderLibrary& shaders)
            {
                IMGUI_CHECKVERSION();
                m_context = ImGui::CreateContext();   // own atlas; sets itself current
                if (!m_context)
                {
                    ARC_ERROR("OffscreenImGuiLayer: ImGui::CreateContext failed");
                    return false;
                }
                ImGuiContext* prev = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(m_context);
                ImGuiIO& io = ImGui::GetIO();
                io.BackendPlatformName = "arcane_offscreen";
                io.IniFilename = nullptr;             // no imgui.ini for the game context
                io.DisplaySize = ImVec2(1.0f, 1.0f);  // non-zero so a stray NewFrame is safe
                const bool ok = m_renderer.Init(device.Nvrhi(), shaders);   // backend flags on THIS ctx
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
                if (!ok)
                {
                    ARC_ERROR("OffscreenImGuiLayer: ImGuiNvrhiRenderer::Init failed");
                    ImGui::DestroyContext(m_context);
                    m_context = nullptr;
                    return false;
                }
                return true;
            }

            void* Context() const override { return m_context; }
            void  SetInput(const Input& in) override { m_input = in; }

            void BeginFrame() override
            {
                ImGui::SetCurrentContext(m_context);
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(m_input.displaySize.x <= 0.0f ? 1.0f : m_input.displaySize.x,
                                        m_input.displaySize.y <= 0.0f ? 1.0f : m_input.displaySize.y);
                io.DeltaTime = m_input.deltaTime > 0.0f ? m_input.deltaTime : 1.0f / 60.0f;
                if (m_input.hasInput)
                {
                    io.AddMousePosEvent(m_input.mousePos.x, m_input.mousePos.y);
                    for (int i = 0; i < 5; ++i) io.AddMouseButtonEvent(i, m_input.mouseDown[i]);
                    if (m_input.wheel != 0.0f) io.AddMouseWheelEvent(0.0f, m_input.wheel);
                }
                else
                {
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);   // cursor off-target
                    for (int i = 0; i < 5; ++i) io.AddMouseButtonEvent(i, false);
                }
                ImGui::NewFrame();
                // WantCapture* is finalized at NewFrame (from last frame's hover/active).
                m_wantMouse    = io.WantCaptureMouse;
                m_wantKeyboard = io.WantCaptureKeyboard;
            }

            void Render(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* target) override
            {
                ImGui::SetCurrentContext(m_context);
                ImGui::Render();
                m_renderer.RenderDrawData(ImGui::GetDrawData(), cmd, target);
            }

            bool WantCaptureMouse()    const override { return m_wantMouse; }
            bool WantCaptureKeyboard() const override { return m_wantKeyboard; }

        private:
            ImGuiContext*      m_context = nullptr;
            ImGuiNvrhiRenderer m_renderer;
            Input              m_input;
            bool               m_wantMouse    = false;
            bool               m_wantKeyboard = false;
        };
    }

    std::unique_ptr<OffscreenImGuiLayer> OffscreenImGuiLayer::Create(RenderDevice& device,
                                                                    ShaderLibrary& shaders)
    {
        auto layer = std::make_unique<OffscreenImGuiLayerImpl>();
        if (!layer->Init(device, shaders))
            return nullptr;
        return layer;
    }
}
