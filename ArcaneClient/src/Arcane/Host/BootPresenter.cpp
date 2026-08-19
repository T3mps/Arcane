#include <Arcane/Host/BootPresenter.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Host/GpuContext.hpp>

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
        // ev.resized: used to be forwarded to m_gpu.OnResize (the NVRHI
        // swapchain resize). GpuContext has had no OnResize -- no NVRHI
        // swapchain to resize at all -- since NRI Phase 5a, Task 6, and this
        // whole function is unreachable regardless (see below), so the call
        // is gone rather than ported to an accessor that no longer compiles.

        // EVERYTHING BELOW THIS POINT IS DELETED, NOT PORTED (NRI Phase 5a,
        // Task 6). It was already dead code before this task: every caller
        // gated constructing a BootPresenter behind `!GraphFlavor()`/
        // `!GraphMode()` (RuntimeApp::StageFinalize, EditorApp's matching
        // stage, EditorAppProject's switch overlay), which has been
        // unconditionally false since the Movement 1 flip -- so this
        // function has had no live caller for several tasks already (see
        // NRI Phase 5a, Task 5's note here, which said as much about the
        // ImGui composite alone). What used to sit here was: an ImGui frame
        // drawing the progress bar/detail text into a Fullscreen-or-Overlay
        // panel, an NVRHI backbuffer acquire (m_gpu.Swap().BeginFrame()), a
        // command-list clear + submit (m_gpu.Cmd(), a GpuPassScope,
        // m_gpu.Device().Nvrhi()->executeCommandList) and a present
        // (m_gpu.Swap().Present()). GpuContext has built no NVRHI device,
        // swapchain or command list since this task collapsed it to one
        // flavor, so none of that compiles any more, and there is nothing
        // left to port it to: the frame graph's ImGuiNriNode is what
        // actually composites a progress overlay today, on a path this class
        // never touches.
        //
        // STILL NOT RETIRED after Task 11a, and here is the exact blocker.
        // 11a collapsed EditorApp::GraphMode(), which covered only ONE of the
        // three gates above -- EditorAppProject's switch overlay, which now
        // passes nullptr unconditionally (its `overlay` local is constructed
        // and unused, deliberately). The other two, RuntimeApp::StageFinalize
        // and EditorApp's matching stage, gate on GpuContext::GraphFlavor(),
        // a predicate 11a did not own and which still has live callers and
        // ArcaneTests assertions. So this class dies with the GraphFlavor()
        // collapse, not this one; until then it is kept as an intact, if
        // inert, IBootPresenter implementation rather than an ImGui frame
        // that gets Begin()'d and never Rendered.
        //
        // m_hasPresentedFrame is now WRITE-DEAD: the deleted tail was its
        // only assignment site (the real draw+present path used to set it
        // true), so HasPresentedFrame() permanently returns false. Left
        // that way deliberately rather than hand-set true here -- a caller
        // that somehow still reached this class and asked "did you actually
        // draw something" should get an honest no, not a lie that nothing
        // was ever drawn into. Fail-loud, not fail-silent, matching the rest
        // of this function's shape.
        return true;
    }
}
