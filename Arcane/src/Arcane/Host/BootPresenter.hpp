#pragma once

// BootPresenter: draws and presents exactly ONE loading frame per call, through
// the existing backbuffer + ImGuiLayer path rather than a bespoke render chain
// (homogenized-rendering mandate), so it inherits the editor theme for free.
//
// It cannot present before gpu_core completes -- that is what BootSplashWindow
// covers.
//
// Task 8c (2026-07-30 correction, "the splash carries the loading UI, not the
// editor window"): in Fullscreen mode this is no longer BootSequence's
// per-stage presenter -- the pre-device splash (BootSplashPresenter,
// BootSplashWindow.hpp) is, for the WHOLE boot. Fullscreen mode is now used
// exactly ONCE, explicitly, by the last boot stage (EditorApp::
// StageSplashReady / RuntimeApp::StageFinalize) to draw the single frame that
// must exist in the swapchain before Window::Show() reveals it -- never a
// live per-stage loading screen. Overlay mode (the in-editor project switch)
// is unaffected: it still drives BootSequence's presenter directly for that
// short-lived, already-visible-window flow.
//
// Tick-cadence contract (see IBootPresenter in BootSequence.hpp): during a
// Worker-stage overlap this is called repeatedly at roughly display cadence
// instead of the main thread blocking silently. Present() therefore does
// exactly one frame of window-pump + ImGui + present per call and returns --
// it never loops or waits on anything unbounded. (Fullscreen mode's one boot
// use above only ever calls Present() once, so this contract matters to it
// only in the trivial sense; Overlay mode is where it is exercised for real.)

#include <Arcane/Base/Api.hpp>
#include <Arcane/Host/BootSequence.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

namespace Arcane
{
    // Forward-declared, not included: GpuContext.hpp pulls in Window/Device/
    // Swapchain/ShaderLibrary/Canvas/Batcher2D/TonemapPass/ImGuiLayer/Input --
    // heavy for a header every boot-stage caller sees. BootPresenter.cpp
    // includes the real header.
    class GpuContext;

    enum class BootPresenterMode : std::uint8_t
    {
        Fullscreen,   // boot's single reveal frame (Task 8c): the whole backbuffer
        Overlay,      // project switch: a modal panel over the last editor frame
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // nvrhi::TextureHandle on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API BootPresenter final : public IBootPresenter
    {
    public:
        BootPresenter(GpuContext& gpu, BootPresenterMode mode);
        ~BootPresenter() override;

        // Returns false when the window was closed -- BootSequence turns that
        // into BootResult::quitRequested.
        bool Present(const BootProgress& progress) override;

        // Splash logo, drawn centered above the bar in Fullscreen mode only --
        // Overlay's small fixed-size modal panel stays bar+label (see
        // Present()). Refcounted: this is the "splash texture" the design spec
        // means when it says BootPresenter "owns nothing but the splash
        // texture" -- its one piece of real state besides mode/flags. Null
        // (the default) draws no image, just the bar/label; the caller owns
        // loading the texture and may rebind or clear it between Present
        // calls (e.g. once gpu_core's stage body has decoded it).
        void SetSplashTexture(nvrhi::TextureHandle texture) noexcept { m_splashTexture = texture; }

        void SetShowProgress(bool show) noexcept { m_showProgress = show; }

        // True once Present() has actually drawn+presented a frame at least
        // once -- as opposed to returning true-but-drew-nothing on the
        // no-backbuffer branch (BootPresenter.cpp: BeginFrame() can yield a
        // null backbuffer transiently, e.g. a zero-size window mid-resize or
        // a surface out of date; Present() reports "still going" rather than
        // failing the whole boot over it). 2026-07-30 review round 2,
        // finding 2's second half: the reveal stages (StageSplashReady /
        // StageFinalize) use this to retry instead of revealing a window
        // nothing was ever drawn into. Purely additive -- every OTHER caller
        // of Present() (the general per-stage pump, Overlay mode) already
        // ignored this field entirely (see its history below) and is
        // unaffected by reading it.
        [[nodiscard]] bool HasPresentedFrame() const noexcept { return m_hasPresentedFrame; }

    private:
        GpuContext&          m_gpu;
        BootPresenterMode    m_mode;
        bool                 m_showProgress = true;
        // Was "m_shownWindow", reserved and unread, until this task gave it
        // the exact job its own old comment described: "has this presenter
        // ever produced a frame". Set true at the end of the real draw+
        // present path in Present(), never on the early no-backbuffer return.
        bool                 m_hasPresentedFrame = false;
        nvrhi::TextureHandle m_splashTexture;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
