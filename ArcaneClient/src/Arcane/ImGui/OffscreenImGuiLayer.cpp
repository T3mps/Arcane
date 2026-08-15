#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>

#include <Arcane/Base/Assert.hpp>
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
                // GUARDED: the graph flavor never ran ImGuiNvrhiRenderer::Init,
                // so there is nothing for Shutdown to tear down and its
                // internals have no device to reach through.
                if (m_hasNvrhiRenderer)
                    m_renderer.Shutdown();
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
                ImGui::DestroyContext(m_context);
                m_context = nullptr;
            }

            // The half both flavors share: the context, its own atlas, and the
            // io the game context is DEFINED by (see the header's CONTEXT
            // DISCIPLINE). Leaves whatever was current current again -- unlike
            // ImGuiLayer::InitCommon, which is the process's PRIMARY context and
            // is required to hold currency; this is the SECOND one and must
            // never steal it.
            bool InitCommon()
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

            bool Init(RenderDevice& device, ShaderLibrary& shaders)
            {
                if (!InitCommon())
                    return false;

                ImGuiContext* prev = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(m_context);
                const bool ok = m_renderer.Init(device.Nvrhi(), shaders);   // backend flags on THIS ctx
                ImGui::SetCurrentContext(prev == m_context ? nullptr : prev);
                if (!ok)
                {
                    ARC_ERROR("OffscreenImGuiLayer: ImGuiNvrhiRenderer::Init failed");
                    ImGui::DestroyContext(m_context);
                    m_context = nullptr;
                    return false;
                }
                m_hasNvrhiRenderer = true;
                return true;
            }

            // The GRAPH flavor. NOTHING is installed on the context here beyond
            // InitCommon's io -- in particular NOT the renderer backend flags,
            // which belong to whichever ImGuiNri will draw this context's lists
            // and are installed by ImGuiNriNode::AdoptImGuiContext once that
            // vehicle exists (the header states the obligation, and
            // ImGuiNri::AdoptContext states why it cannot be inferred).
            bool InitForGraph() { return InitCommon(); }

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
                // Recon 6's rule for a member that was never constructed, and
                // the same fallback ImGuiLayer::Render carries: assert loudly,
                // then STILL end the frame, so a mistaken call does not also
                // break the "every BeginFrame is paired exactly once" contract
                // and turn a wrong picture into an assert on the next frame.
                ARC_ASSERT(m_hasNvrhiRenderer,
                           "OffscreenImGuiLayer::Render on the GRAPH flavor -- that flavor builds no "
                           "NVRHI renderer; the graph's ImGuiNriNode draws RenderToDrawData()'s output");
                if (!m_hasNvrhiRenderer)
                {
                    (void)RenderToDrawData();
                    return;
                }
                ImGui::SetCurrentContext(m_context);
                ImGui::Render();
                m_renderer.RenderDrawData(ImGui::GetDrawData(), cmd, target);
            }

            ImDrawData* RenderToDrawData() override
            {
                // THE PIN IS THE POINT (see the header). ImGui::Render() ends
                // the frame itself, so the BeginFrame pairing holds here exactly
                // as it does in Render above.
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
            ImGuiContext*      m_context = nullptr;
            ImGuiNvrhiRenderer m_renderer;
            Input              m_input;
            bool               m_wantMouse    = false;
            bool               m_wantKeyboard = false;
            // Which flavor this is -- see the header. Governs the destructor's
            // Shutdown and Render's refusal, and nothing else: every other entry
            // point is context-only and identical on both.
            bool               m_hasNvrhiRenderer = false;
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

    std::unique_ptr<OffscreenImGuiLayer> OffscreenImGuiLayer::CreateForGraph()
    {
        auto layer = std::make_unique<OffscreenImGuiLayerImpl>();
        if (!layer->InitForGraph())
            return nullptr;
        return layer;
    }
}
