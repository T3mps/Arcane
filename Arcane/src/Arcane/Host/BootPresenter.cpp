#include <Arcane/Host/BootPresenter.hpp>

#include <Arcane/Host/GpuContext.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>

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

        // Logo: Fullscreen only. Overlay's fixed 420x120 panel is sized for
        // the bar plus one label line -- a full logo would not fit, and
        // Overlay is a brief "resuming..." panel over live editor content,
        // not a branding moment (Fullscreen already owns first-boot
        // branding).
        if (m_mode == BootPresenterMode::Fullscreen && m_splashTexture)
        {
            const nvrhi::TextureDesc& d = m_splashTexture->getDesc();
            constexpr float kMaxLogo = 160.0f;
            if (d.width > 0 && d.height > 0)
            {
                const float scale = kMaxLogo / (float)std::max(d.width, d.height);
                const float logoW = (float)d.width  * scale;
                const float logoH = (float)d.height * scale;
                ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - logoW) * 0.5f);
                ImGui::Image((ImTextureID)(uintptr_t)m_splashTexture.Get(),
                             ImVec2(logoW, logoH));
                ImGui::Spacing();
            }
        }

        if (m_showProgress)
        {
            ImGui::ProgressBar(progress.fraction, ImVec2(-1.0f, 0.0f));
            if (!progress.detail.empty())
                ImGui::TextDisabled("%s", progress.detail.c_str());
            else if (!progress.stageId.empty())
                ImGui::TextDisabled("%s", progress.stageId.c_str());
        }

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
