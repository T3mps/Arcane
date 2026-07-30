#pragma once

// A plain OS window shown at process start, BEFORE any graphics device exists,
// so something is on screen within ~100ms of the click. Torn down once the real
// window is up (see the ordering note on Close()).
//
// Windows-only; a no-op elsewhere. It must NEVER be able to fail boot -- every
// failure path degrades to "no splash" silently.
//
// Task 8c (2026-07-30 correction, "the splash carries the loading UI, not the
// editor window"): this is now where BootProgress is actually RENDERED for the
// whole boot -- see BootSplashPresenter below. UE's shape, which we copy:
// engine init/module-plugin-load/startup-map-load all run with the splash up
// and report into it (UnrealEdGlobals.cpp:167-194, FeedbackContextEditor.cpp:
// 668-669), and the main window is not revealed until loading is finished
// (UnrealEdGlobals.cpp:215-236: "Hide the splash screen now that everything
// is ready to go" -> Hide() -> "Do final set up on the editor frame and show
// it" -> CreateDefaultMainFrame).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Host/BootSequence.hpp>   // IBootPresenter / BootProgress -- BootSplashPresenter below

#include <memory>
#include <string>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::unique_ptr<Impl> on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API BootSplashWindow
    {
    public:
        // Never throws. IsOpen() reflects ONLY whether the OS window itself was
        // created -- a missing, unreadable, or corrupt image never affects it.
        // (An earlier revision of this comment claimed a bad image made
        // IsOpen() false; that was never actually true, and became doubly
        // wrong once Task 8c started reading imagePath at all -- the image is
        // loaded best-effort, AFTER the window exists, and a failure there
        // just leaves the class background brush as the whole splash. See
        // SetStatusText/SetProgress below for the same always-degrade,
        // never-fail contract on the live status/progress updates.)
        explicit BootSplashWindow(const char* imagePath) noexcept;
        ~BootSplashWindow();

        BootSplashWindow(const BootSplashWindow&)            = delete;
        BootSplashWindow& operator=(const BootSplashWindow&) = delete;

        // ORDERING MATTERS: call this AFTER the real window is shown, never
        // before. Closing first leaves a frame with neither window on screen,
        // which is the flicker this whole component exists to avoid. UE's
        // editor gets this right the same way (UnrealEdGlobals.cpp:215-236,
        // cited above): Hide() the splash only after CreateDefaultMainFrame.
        void Close() noexcept;

        [[nodiscard]] bool IsOpen() const noexcept;

        // Update the one status line drawn over the splash image (bottom-left,
        // UE's convention for startup progress -- WindowsPlatformSplash.cpp:
        // 99-116). Safe to call before the window exists, after Close(), or
        // when construction failed -- every case is a silent no-op. Cheap and
        // non-blocking: stores the text, then schedules a repaint on the
        // splash's OWN thread; never paints from the caller's thread
        // (IBootPresenter's tick-cadence contract -- BootSequence.hpp).
        void SetStatusText(std::string text) noexcept;

        // Drive the Windows taskbar progress overlay (ITaskbarList3::
        // SetProgressValue), matching WindowsPlatformSplash.cpp:769-781.
        // `fraction01` is clamped to [0,1]. Same safety/cost contract as
        // SetStatusText. Every COM/taskbar failure -- init, CoCreateInstance,
        // HrInit -- degrades silently to "no taskbar progress"; never affects
        // boot.
        void SetProgress(float fraction01) noexcept;

    public:
        // Defined in BootSplashWindow.cpp, Windows-only. Public (not private)
        // so the free WndProc function there -- which must stay a plain
        // function pointer to satisfy WNDPROC, so it cannot be a member --
        // can name it for GWLP_USERDATA's reinterpret_cast without a `friend`
        // declaration pulling HWND/WNDPROC into this platform-neutral header.
        // m_impl below stays private; nothing outside the .cpp can complete
        // Impl's definition or reach m_impl, so this costs no real encapsulation.
        struct Impl;

    private:
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // BootSplashPresenter: the pre-device IBootPresenter. Forwards every
    // BootProgress update to the splash window's SetStatusText/SetProgress
    // instead of drawing a swapchain frame (Task 8c). Wired as BootSequence::
    // Run's ONE presenter for the WHOLE boot, from runtime_create through
    // finalize -- the swapchain-backed BootPresenter (BootPresenter.hpp) is
    // used exactly once, explicitly, by the LAST stage's own body
    // (EditorApp::StageSplashReady / RuntimeApp::StageFinalize), never
    // through this per-stage pump. See the design doc's Task 8c correction
    // (docs/superpowers/specs/2026-07-29-async-boot-loading-screen-design.md)
    // for the full "who draws what, when" picture.
    //
    // Header-only and deliberately NOT ARCANE_API: it derives from the
    // exported IBootPresenter interface but is compiled straight into each
    // host's own TU (EditorApp.cpp / RuntimeApp.cpp), replacing the
    // per-host LazyBootPresenter nested class those files used to hand-roll
    // for the same "one stable IBootPresenter& for the whole Run() call"
    // reason -- this is that same shape, shared instead of duplicated.
    class BootSplashPresenter final : public IBootPresenter
    {
    public:
        // `splash` may be null (a host constructed without one, or a
        // defensive/test path) -- every call below then degrades to "do
        // nothing", matching BootSplashWindow's own never-fail-boot contract.
        explicit BootSplashPresenter(BootSplashWindow* splash) noexcept : m_splash(splash) {}

        // Always returns true: the pre-device splash has no close affordance
        // that should abort boot (WS_POPUP, no system menu, no titlebar).
        // Quit-during-boot is unchanged -- still detected once the real
        // BootPresenter exists, at the reveal stage's own explicit Present()
        // call (see StageSplashReady/StageFinalize).
        bool Present(const BootProgress& progress) override
        {
            if (m_splash)
            {
                m_splash->SetStatusText(!progress.detail.empty() ? progress.detail : progress.stageId);
                m_splash->SetProgress(progress.fraction);
            }
            return true;
        }

    private:
        BootSplashWindow* m_splash;
    };
}
