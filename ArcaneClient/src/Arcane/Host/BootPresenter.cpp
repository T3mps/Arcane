#include <Arcane/Host/BootPresenter.hpp>

#include <Arcane/Base/Log.hpp>
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
        if (ev.quitRequested)
        {
            // Say so. A consumed quit aborts the whole sequence and, in the
            // project-switch case, exits the editor -- silently, because the
            // event never reaches PumpFrameEvents. When that turns out to be
            // WRONG (a quit nobody asked for), this line is the only evidence
            // that it happened at all, and which stage it landed on.
            ARC_INFO("BootPresenter: quit consumed during stage '{}' ({}); aborting the sequence",
                     progress.stageId.empty() ? "<terminal>" : progress.stageId.c_str(),
                     m_mode == BootPresenterMode::Overlay ? "overlay" : "fullscreen");
            return false;
        }
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
        // F-8d: boot-time device removal is real, and this submit runs before
        // any frame loop exists -- so it gets a pass scope of its own rather
        // than being the one GPU-bearing site a crash report could not name.
        // The crash backend is armed at device creation, which precedes the
        // first Present() here, so the scope always has somewhere to write.
        {
            GpuPassScope pass(cmd, "pass:boot");
            if (m_mode == BootPresenterMode::Fullscreen)
            {
                // Fullscreen owns the whole backbuffer -- clear it first. Overlay
                // must NOT clear: it draws over the last editor frame, which is
                // still sitting in this backbuffer image from an earlier Present.
                cmd->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                       nvrhi::Color(0.06f, 0.06f, 0.08f, 1.0f));
            }
            // NRI Phase 5a, Task 5 deleted ImGuiLayer::Render(cmd, fb) along
            // with the NVRHI renderer it drove (RenderToDrawData() is the
            // only entry point left, and only the frame graph's ImGuiNriNode
            // draws its output). This whole function is dead code today --
            // every caller gates construction of the BootPresenter instance
            // behind `!GraphFlavor()`/`!GraphMode()` (RuntimeApp::
            // StageFinalize, EditorApp's matching stage, EditorAppProject's
            // switch overlay), which is unconditionally false since the
            // Movement 1 flip -- so there is nothing left to compose the
            // progress-bar frame into here; it is simply not called.
        }
        cmd->close();
        m_gpu.Device().Nvrhi()->executeCommandList(cmd);
        m_gpu.Swap().Present();

        m_hasPresentedFrame = true;   // real content is now in the swapchain -- see HasPresentedFrame()
        return true;
    }
}
