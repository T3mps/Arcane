#pragma once

// BootPresenter: draws and presents exactly ONE loading frame per call, through
// the existing backbuffer + ImGuiLayer path rather than a bespoke render chain
// (homogenized-rendering mandate), so it inherits the editor theme for free.
//
// It cannot present before gpu_core completes -- that is what BootSplashWindow
// covers.
//
// Tick-cadence contract (see IBootPresenter in BootSequence.hpp): during a
// Worker-stage overlap this is called repeatedly at roughly display cadence
// instead of the main thread blocking silently. Present() therefore does
// exactly one frame of window-pump + ImGui + present per call and returns --
// it never loops or waits on anything unbounded.

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
        Fullscreen,   // boot: the whole backbuffer
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

    private:
        GpuContext&          m_gpu;
        BootPresenterMode    m_mode;
        bool                 m_showProgress = true;
        // Reserved: an earlier draft of this class gated Window::Show() on
        // "has this presenter ever produced a frame". That responsibility now
        // lives in the splash_ready stage body instead (shows the real window,
        // THEN closes the pre-device splash -- see docs/superpowers/specs/
        // 2026-07-29-async-boot-loading-screen-design.md), so this is not read
        // anywhere today. Left declared rather than dropped.
        bool                 m_shownWindow  = false;
        nvrhi::TextureHandle m_splashTexture;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
