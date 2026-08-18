#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>

#include <Arcane/Base/Log.hpp>

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
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }

            // The context, its own atlas, and the io the game context is
            // DEFINED by (see the header's CONTEXT DISCIPLINE). Leaves
            // whatever was current current again -- unlike ImGuiLayer::Init,
            // which is the process's PRIMARY context and is required to hold
            // currency; this is the SECOND one and must never steal it.
            bool Init()
            {
                IMGUI_CHECKVERSION();
                m_context = ImGui::CreateContext();   // own atlas; prior context stays current, so we pin m_context explicitly below
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
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
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

            ImDrawData* RenderToDrawData() override
            {
                // THE PIN IS THE POINT (see the header). ImGui::Render() ends
                // the frame itself, so the BeginFrame pairing holds here
                // regardless of what was current before this call.
                ImGui::SetCurrentContext(m_context);
                ImGui::Render();
                return ImGui::GetDrawData();
            }

            void EndFrameDiscard() override
            {
                ImGui::SetCurrentContext(m_context);
                ImGui::EndFrame();
            }

            bool WantCaptureMouse()    const override { return m_wantMouse; }
            bool WantCaptureKeyboard() const override { return m_wantKeyboard; }

        private:
            ImGuiContext* m_context = nullptr;
            Input         m_input;
            bool          m_wantMouse    = false;
            bool          m_wantKeyboard = false;
        };
    }

    std::unique_ptr<OffscreenImGuiLayer> OffscreenImGuiLayer::Create()
    {
        auto layer = std::make_unique<OffscreenImGuiLayerImpl>();
        if (!layer->Init())
            return nullptr;
        return layer;
    }
}
