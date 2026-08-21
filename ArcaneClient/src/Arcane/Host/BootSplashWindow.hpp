#pragma once

// A plain OS window shown at process start, BEFORE any graphics device exists,
// so something is on screen within ~100ms of the click. Torn down once the real
// window is up (see the ordering note on Close()).
//
// Windows-only; a no-op elsewhere. It must NEVER be able to fail boot -- every
// failure path degrades to "no splash" silently.
//
// THE SPLASH CARRIES THE LOADING UI, not the editor window: this is where
// BootProgress is RENDERED for the whole boot -- see BootSplashPresenter
// below. UE's shape, which we copy:
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
        // The image is loaded best-effort, AFTER the window exists, and a
        // failure there just leaves the class background brush as the whole
        // splash. See SetStatusText/SetProgress below for the same
        // always-degrade, never-fail contract on the live status/progress
        // updates.
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

        // "An OS window for this splash existed at some point" -- monotonic,
        // set the instant CreateWindowExW succeeds and NEVER cleared, so it
        // stays true after Close() and after the user destroys the window.
        // Distinct from IsOpen() ("it exists right now") and load-bearing
        // precisely because of the difference: the pair
        // (!IsOpen() && WasEverOpen()) is the ONLY unambiguous "it was up and
        // now it is gone" signal, and unlike an observer's own call history it
        // does not require anyone to have been LOOKING while the window was
        // up. It is false forever when window creation failed outright, which
        // is what keeps that documented degrade ("no splash") from ever
        // reading as a close. See BootSplashPresenter::Present below, the one
        // consumer, and Task 8d's root cause in BootSplashWindow.cpp
        // (Impl::everOpen).
        [[nodiscard]] bool WasEverOpen() const noexcept;

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

        // Gate what BootSplashPresenter::Present (below) forwards to this
        // splash: false means Present() leaves the splash on branding alone --
        // no status text, no taskbar percentage -- matching the spec default
        // for a player who never asked to watch asset scanning (ProjectManifest
        // ::SplashConfig::showProgress, spec sec 6). Defaults to TRUE, matching
        // this class's behaviour before this method existed (every existing
        // caller -- the editor, and every BootSplashPresenter test -- keeps
        // seeing status text/progress with zero code change). The runtime host
        // is the one caller that wants FALSE, and sets it explicitly (see
        // RuntimeApp::Run, before BootSequence::Run begins) BEFORE flipping it
        // per-project once project_open's manifest is known (ProjectBoot.cpp's
        // RuntimeStages override). Safe to call from any thread, before the
        // window exists, or after Close() -- same always-degrade contract as
        // every other setter here; on non-Windows this is a harmless no-op
        // (ShowProgress() below always reports true there, but every consumer
        // of it -- SetStatusText/SetProgress -- is already a no-op on that
        // platform, so the value is unobservable).
        void SetShowProgress(bool show) noexcept;

        // The flag SetShowProgress last set (or its default). Read by
        // BootSplashPresenter::Present; exposed publicly mainly so it is
        // directly testable without reaching into the presenter.
        [[nodiscard]] bool ShowProgress() const noexcept;

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
    // rather than drawing a frame. Wired as BootSequence::Run's ONE presenter
    // for the WHOLE boot, from runtime_create through finalize -- and the ONLY
    // presenter there is. Both hosts reveal their window from the render
    // vehicle's creation instead.
    //
    // Header-only and deliberately NOT ARCANE_API: it derives from the
    // exported IBootPresenter interface but is compiled straight into each
    // host's own TU (EditorApp.cpp / RuntimeApp.cpp), which is what gives each
    // one stable IBootPresenter& for the whole Run() call.
    class BootSplashPresenter final : public IBootPresenter
    {
    public:
        // `splash` may be null (a host constructed without one, or a
        // defensive/test path) -- every call below then degrades to "do
        // nothing", matching BootSplashWindow's own never-fail-boot contract.
        explicit BootSplashPresenter(BootSplashWindow* splash) noexcept : m_splash(splash) {}

        // Called by the reveal stage (StageSplashReady / StageFinalize)
        // immediately BEFORE it closes the splash itself -- see Present()'s
        // comment for why this specific ordering, not merely "somewhere in
        // that stage", is required. Sticky: the host closes the splash exactly
        // once, at the end of boot, and there is no legitimate re-arm.
        void Disarm() noexcept { m_disarmed = true; }

        // Quit-during-boot (2026-07-30 review round 2): if the splash was
        // ever open and is no longer, and nobody called Disarm() first, the
        // user closed it (its only close affordance is Alt+F4 -- WS_POPUP
        // has no titlebar/system-menu, but SW_SHOW still activates/focuses
        // it, so Alt+F4's WM_CLOSE reaches it) -- request an abort exactly
        // the way IBootPresenter documents (BootSequence.hpp:65):
        // BootSequence::Run already converts a false return into
        // BootResult::quitRequested + a clean exit, no new plumbing needed.
        //
        // "Ever been open" comes from the SPLASH (WasEverOpen(), a monotonic
        // latch set on the splash's own thread the instant its window is
        // created), never from this presenter's own call history. That
        // distinction IS Task 8d's bug, and it is not a subtle race:
        //
        // NOTE for anyone reusing WasEverOpen(): the `!IsOpen() &&
        // WasEverOpen()` test below is short-circuiting BY REQUIREMENT, not
        // by style. The latch is stored just AFTER open, so there is a brief
        // window at creation in which open is true and everOpen is not yet --
        // reading WasEverOpen() without an IsOpen() guard in front of it can
        // sample that gap. See BootSplashWindow.cpp's store site.
        //
        //   This class used to carry an `m_armed` flag, set the first time
        //   Present() happened to observe IsOpen() == true, and treated a
        //   later false as the close. That silently required a Present() call
        //   to LAND while the window was up. It routinely does not.
        //   BootSequence calls the presenter only after a stage COMPLETES
        //   (BootSequence.cpp:214) or while a Worker stage overlaps an idle
        //   main thread (:250) -- so nothing at all is presented until the
        //   FIRST stage finishes. Measured on the real editor (2026-07-30):
        //   splash window up at t=0, first present() at t=+1.0s. Close it in
        //   that second -- the one stretch where the splash is the only thing
        //   on screen, i.e. exactly when a human reaches for Alt+F4 -- and
        //   every later call saw only "not open, never armed" and returned
        //   true. The boot ran to completion and the editor appeared, which
        //   is precisely the desk-check report.
        //
        // Both reasons the old flag existed still hold, and WasEverOpen()
        // satisfies them by construction rather than by observation:
        //   1. Window creation is asynchronous (BootSplashWindow's own
        //      thread), so an early Present() can land before CreateWindowExW
        //      has run. WasEverOpen() is false then -- not a quit. Correct.
        //   2. If window creation fails outright, WasEverOpen() is false
        //      FOREVER, so "no splash" (a documented, must-never-fail-boot
        //      degrade) can never read as "boot aborted". Correct.
        // What it ADDS is case 3: the window was created and is now gone,
        // which is true whether or not anybody was watching at the time.
        bool Present(const BootProgress& progress) override
        {
            if (!m_splash) return true;
            // showProgress gates ONLY what gets reported (status text + taskbar
            // percentage) -- never the quit-detection below, which must keep
            // working whether or not progress is being shown.
            if (m_splash->ShowProgress())
            {
                m_splash->SetStatusText(!progress.detail.empty() ? progress.detail : progress.stageId);
                m_splash->SetProgress(progress.fraction);
            }

            // was open, now is not, and nobody Disarm()'d us: a real quit.
            if (!m_disarmed && !m_splash->IsOpen() && m_splash->WasEverOpen())
                return false;
            return true;
        }

    private:
        BootSplashWindow* m_splash;
        // Was `m_armed` (defaulting to false, meaning "no evidence yet"). Now
        // the inverse and only the INTENTIONAL close: the evidence lives in
        // the splash. Defaulting to false is what makes the fix work at the
        // head of boot -- there is nothing left to have missed.
        bool m_disarmed = false;
    };
}
