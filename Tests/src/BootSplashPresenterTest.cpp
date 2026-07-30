// BootSplashPresenter: the armed quit-detection wired to BootSplashWindow
// (2026-07-30 review round 2, finding 2). Exercises it against a REAL
// BootSplashWindow rather than a mock -- Windows-only, both classes are
// already documented no-ops elsewhere (see BootSplashWindow.hpp), and this
// tests BEHAVIOUR (Present()'s return value, IBootPresenter's documented
// quit contract) rather than visual appearance, which is what the design
// doc's "Splash appearance itself is desk-verify only" note actually scopes
// out -- so it is safe and meaningful to automate.
//
// Proves exactly what the review asked to be proved: the happy path
// (Disarm() before Close() never falsely reports a quit -- every successful
// boot depends on this) and the feature itself (closing the splash WITHOUT
// Disarm() reports a quit, matching the human's ruling that a splash close
// should abort boot).

#if defined(_WIN32)

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/BootSequence.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>

#include <chrono>
#include <thread>

namespace
{
    // BootSplashWindow's own window creation is asynchronous (its own
    // thread) -- poll IsOpen() with a bounded timeout rather than assuming
    // it is already true the instant the constructor returns (that race is
    // exactly what BootSplashPresenter::Present's "arm only once observed
    // open" logic exists to survive; this helper just gets the OTHER tests
    // in this file past it so they can exercise the interesting states).
    // A timeout means window creation failed or is pathologically slow on
    // this machine -- REQUIRE reports that plainly rather than hanging.
    bool WaitUntilOpen(Arcane::BootSplashWindow& splash,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (splash.IsOpen()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return splash.IsOpen();
    }
}

TEST_CASE("BootSplashPresenter never reports a quit before the splash is ever observed open", "[boot]")
{
    // Window creation is asynchronous; the very first Present() call (right
    // after runtime_create completes in a real boot) can easily land before
    // CreateWindowExW has even run, and creation can fail outright
    // (IsOpen() false forever) -- neither may ever be misread as a quit, or
    // BootSplashWindow's own never-fail-boot contract breaks. Deterministic
    // regardless of the actual creation race: nothing has closed the splash
    // in this test yet, so Present() can only see "not armed yet" or
    // "armed, still open" -- both return true.
    Arcane::BootSplashWindow splash("");   // no image -- irrelevant to this test
    Arcane::BootSplashPresenter presenter(&splash);

    Arcane::BootProgress p;
    p.fraction = 0.0f;
    CHECK(presenter.Present(p));

    splash.Close();   // clean teardown regardless of how far creation got
}

TEST_CASE("BootSplashPresenter: Disarm() before Close() never reports a quit", "[boot]")
{
    // The happy-path proof: every successful boot calls Disarm() then
    // Close() in exactly this order (EditorApp::StageSplashReady /
    // RuntimeApp::StageFinalize) -- if this ordering or the arm/disarm logic
    // ever regressed, the editor would silently exit at 100% instead of
    // entering MainLoop, which is why this is proved here rather than only
    // reasoned about.
    Arcane::BootSplashWindow splash("");
    REQUIRE(WaitUntilOpen(splash));

    Arcane::BootSplashPresenter presenter(&splash);
    Arcane::BootProgress p;
    p.fraction = 0.5f;
    CHECK(presenter.Present(p));   // arms (splash observed open)

    presenter.Disarm();
    splash.Close();

    p.fraction = 1.0f;
    CHECK(presenter.Present(p));   // must NOT report a quit
}

TEST_CASE("BootSplashPresenter: closing the splash without Disarm() reports a quit", "[boot]")
{
    // The feature itself: the human ruled that closing the splash (its only
    // close affordance is Alt+F4 -- see BootSplashWindow.cpp's WS_EX_APPWINDOW
    // comment) should abort boot. Proves IBootPresenter's documented quit
    // contract (BootSequence.hpp) actually fires when nobody told the
    // presenter to expect the close.
    Arcane::BootSplashWindow splash("");
    REQUIRE(WaitUntilOpen(splash));

    Arcane::BootSplashPresenter presenter(&splash);
    Arcane::BootProgress p;
    p.fraction = 0.5f;
    CHECK(presenter.Present(p));   // arms

    splash.Close();   // no Disarm() -- simulates the user closing it mid-boot

    p.fraction = 0.75f;
    CHECK_FALSE(presenter.Present(p));   // must report a quit
}

#endif
