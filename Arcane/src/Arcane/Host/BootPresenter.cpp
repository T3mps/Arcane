#include <Arcane/Host/BootPresenter.hpp>

#include <Arcane/Host/GpuContext.hpp>

#include <imgui.h>

namespace Arcane
{
    BootPresenter::BootPresenter(GpuContext& gpu, BootPresenterMode mode)
        : m_gpu(gpu), m_mode(mode) {}

    BootPresenter::~BootPresenter() = default;

    bool BootPresenter::Present(const BootProgress& progress)
    {
        const WindowEvents ev = m_gpu.Win().PumpEvents();
        if (ev.quitRequested) return false;
        if (ev.resized) m_gpu.OnResize(ev.width, ev.height);

        m_gpu.Imgui().BeginFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (m_mode == BootPresenterMode::Fullscreen)
        {
            ImGui::SetNextWindowPos(vp->Pos);
            ImGui::SetNextWindowSize(vp->Size);
        }
        else
        {
            // Overlay: a small modal panel centered over whatever the last
            // editor frame drew. The backbuffer is NOT cleared below in this
            // mode, so that frame stays on screen behind the panel.
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                           vp->Pos.y + vp->Size.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(420.0f, 120.0f));
        }
        ImGui::Begin("##bootsplash", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);

        // Always drawn -- the mid-arc SetShowProgress toggle this used to read
        // (BootPresenter::SetShowProgress) had zero call sites (2026-07-31
        // review, Important 3; removed along with the unreachable Fullscreen
        // logo block that used to sit above this, which depended on a
        // SetSplashTexture that was equally dead). Both of BootPresenter's own
        // uses want the bar every time: Fullscreen's single reveal frame IS the
        // whole boot's one chance to show progress at all, and Overlay's brief
        // project-switch panel is short enough that hiding the bar would not
        // save anything. A caller wanting to suppress the bar entirely (e.g.
        // ArcaneRuntime's silent-splash manifest flag) does so through
        // BootSplashWindow::SetShowProgress instead -- a different class, for
        // the pre-device splash, not this one.
        ImGui::ProgressBar(progress.fraction, ImVec2(-1.0f, 0.0f));
        if (!progress.detail.empty())
            ImGui::TextDisabled("%s", progress.detail.c_str());
        else if (!progress.stageId.empty())
            ImGui::TextDisabled("%s", progress.stageId.c_str());

        ImGui::End();

        nvrhi::ITexture* backbuffer = m_gpu.Swap().BeginFrame();
        if (!backbuffer)
        {
            // No backbuffer this call (zero-size window mid-resize, surface
            // out of date). Balance the BeginFrame above with a bare EndFrame
            // so ImGui's double-Begin assert does not fire on the next call,
            // and report "still going" -- only a real quit aborts the boot.
            ImGui::EndFrame();
            return true;
        }

        nvrhi::ICommandList* cmd = m_gpu.Cmd();
        cmd->open();
        if (m_mode == BootPresenterMode::Fullscreen)
        {
            // Fullscreen owns the whole backbuffer -- clear it first. Overlay
            // must NOT clear: it draws over the last editor frame, which is
            // still sitting in this backbuffer image from an earlier Present.
            cmd->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                   nvrhi::Color(0.06f, 0.06f, 0.08f, 1.0f));
        }
        nvrhi::FramebufferHandle& fb = m_gpu.FramebufferFor(backbuffer);
        m_gpu.Imgui().Render(cmd, fb);
        cmd->close();
        m_gpu.Device().Nvrhi()->executeCommandList(cmd);
        m_gpu.Swap().Present();

        m_hasPresentedFrame = true;   // real content is now in the swapchain -- see HasPresentedFrame()
        return true;
    }
}
