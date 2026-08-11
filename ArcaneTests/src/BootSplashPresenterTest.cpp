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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

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

    // ---- the USER close path, not BootSplashWindow::Close() -----------------
    //
    // The three unit tests below simulate "the user closed the splash" by
    // calling splash.Close(). That is NOT the path a human takes: Close()
    // stores open=false itself, from the CALLER's thread
    // (BootSplashWindow.cpp's Close body), so it proves the presenter's
    // arm/disarm logic and nothing about how the flag gets cleared when
    // Windows destroys the window out from under us. Alt+F4 instead runs
    // WM_SYSCOMMAND/SC_CLOSE -> DefWindowProcW -> WM_CLOSE -> DefWindowProcW
    // -> DestroyWindow -> WM_DESTROY -> PostQuitMessage -> the splash
    // thread's own message loop exits -- an entirely different code path
    // ending at a DIFFERENT store. These helpers exercise THAT one.

    // The splash's own HWND, found the way an external actor would: by window
    // class, restricted to this process (Catch2 runs test cases sequentially,
    // so only the splash under test exists, but the pid filter makes that an
    // invariant rather than an assumption).
    HWND FindSplashHwnd()
    {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, L"ArcaneBootSplash", nullptr)) != nullptr)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return h;
        }
        return nullptr;
    }

    // Close the splash exactly the way Alt+F4 does, then wait (bounded) for
    // the splash to stop reporting itself open. The wait is not papering over
    // a race the real bug depends on: in a real boot the close is followed by
    // SECONDS of further boot stages, so "did IsOpen() ever go false" is the
    // honest question, and a false return here says the OS destroy path never
    // clears the flag at all.
    bool UserCloseSplash(Arcane::BootSplashWindow& splash,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        HWND h = FindSplashHwnd();
        if (!h) return false;
        // SC_CLOSE, not WM_CLOSE: this is literally the message DefWindowProc
        // synthesises from Alt+F4's WM_SYSKEYDOWN, so the whole
        // SC_CLOSE -> WM_CLOSE -> DestroyWindow chain runs, not just its tail.
        PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!splash.IsOpen()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !splash.IsOpen();
    }

    Arcane::BootStage Stage(std::string id, std::vector<std::string> deps,
                            std::function<bool()> run,
                            Arcane::BootThread thread = Arcane::BootThread::Main)
    {
        Arcane::BootStage s;
        s.id        = std::move(id);
        s.dependsOn = std::move(deps);
        s.thread    = thread;
        s.policy    = Arcane::BootPolicy::Fatal;
        s.weight    = 1;
        s.run       = std::move(run);
        return s;
    }

    bool Ran(const std::vector<std::string>& order, const char* id)
    {
        return std::find(order.begin(), order.end(), id) != order.end();
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

TEST_CASE("BootSplashWindow::ShowProgress defaults true and round-trips through SetShowProgress", "[boot]")
{
    // Default true preserves this class's behaviour from before the flag
    // existed -- the editor and every OTHER test in this file need zero
    // changes because of it (spec sec 6: the editor always shows progress).
    Arcane::BootSplashWindow splash("");
    CHECK(splash.ShowProgress());

    splash.SetShowProgress(false);
    CHECK_FALSE(splash.ShowProgress());

    splash.SetShowProgress(true);
    CHECK(splash.ShowProgress());

    splash.Close();
}

TEST_CASE("BootSplashPresenter still reports a quit when showProgress is false", "[boot]")
{
    // The showProgress gate (BootSplashPresenter::Present) must affect ONLY
    // what gets reported (status text/taskbar percentage), never the
    // quit-detection below it in the same method -- proves the two are not
    // accidentally coupled (e.g. an early `if (show) { ...; return true; }`
    // shape would silently swallow Alt+F4 for any project that opted out of
    // progress display).
    Arcane::BootSplashWindow splash("");
    REQUIRE(WaitUntilOpen(splash));
    splash.SetShowProgress(false);

    Arcane::BootSplashPresenter presenter(&splash);
    Arcane::BootProgress p;
    p.fraction = 0.5f;
    CHECK(presenter.Present(p));   // arms (splash observed open)

    splash.Close();   // no Disarm() -- simulates the user closing it mid-boot

    p.fraction = 0.75f;
    CHECK_FALSE(presenter.Present(p));   // must still report a quit
}

// ---- integration: a real BootSequence::Run, closed the way a human does ----
//
// The three unit tests above prove BootSplashPresenter::Present() in
// isolation. They did NOT prove the WIRING, and a human desk-check
// (2026-07-30, Task 8d) found the wiring broken: Alt+F4 on the splash made it
// visibly disappear and the editor still booted to a fully usable state.
// These drive a REAL BootSequence::Run with a REAL BootSplashWindow +
// BootSplashPresenter and close the splash partway through, which is the
// coverage whose absence let that ship.
//
// Three cases, because the three differ in WHICH state the presenter is in
// when the close lands, and only the first was actually broken:
//   1. Closed BEFORE Present() has ever been called at all. This is the
//      reported defect. Measured on the real editor (2026-07-30): the splash
//      window is up ~1.0s before BootSequence's first present() call, because
//      that call only happens once the FIRST stage (runtime_create) COMPLETES
//      -- so this is not a narrow race, it is the single longest stretch of
//      the whole boot in which the splash is the only thing on screen, i.e.
//      exactly when a human reaches for Alt+F4.
//   2. Closed from a stage body mid-sequence, after Present() has seen the
//      splash open.
//   3. Closed during a Worker-stage overlap, where BootSequence pumps the
//      presenter itself (BootSequence.cpp:250) rather than calling it after a
//      completed stage (BootSequence.cpp:214) -- a separate call site, and
//      only one of them being right still boots an editor the user asked to
//      go away.

TEST_CASE("BootSequence aborts when the splash is closed before the first Present()", "[boot]")
{
    // THE REPORTED DEFECT (2026-07-30 desk-check, Task 8d). Deliberately
    // deterministic: the close completes before Run() is entered, so this
    // reproduces the head-of-boot blind spot with no timing dependence at all
    // -- there is no arrangement of stage durations under which this test is
    // merely lucky.
    Arcane::BootSplashWindow splash("");
    REQUIRE(WaitUntilOpen(splash));

    Arcane::BootSplashPresenter presenter(&splash);
    REQUIRE(UserCloseSplash(splash));   // Alt+F4, before ANY present() call

    std::vector<std::string> order;
    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("first",  {},         [&] { order.push_back("first");  return true; }));
    stages.push_back(Stage("second", {"first"},  [&] { order.push_back("second"); return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&presenter);

    CHECK(r.quitRequested);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(Ran(order, "second"));

    splash.Close();
}

TEST_CASE("BootSequence aborts when the user closes the splash during a main stage", "[boot]")
{
    Arcane::BootSplashWindow splash("");
    REQUIRE(WaitUntilOpen(splash));

    Arcane::BootSplashPresenter presenter(&splash);

    std::vector<std::string> order;
    bool wasOpenBeforeClose = false;
    bool sawClosed          = false;

    std::vector<Arcane::BootStage> stages;
    // "early" exists so the presenter gets its arming present() (Present()
    // arms only once it has OBSERVED IsOpen() == true) before anything closes
    // the splash -- the same job runtime_create does in a real boot.
    stages.push_back(Stage("early", {}, [&] { order.push_back("early"); return true; }));
    stages.push_back(Stage("close", {"early"}, [&]
    {
        order.push_back("close");
        wasOpenBeforeClose = splash.IsOpen();
        sawClosed          = UserCloseSplash(splash);
        return true;   // the STAGE succeeds -- the abort must come from the presenter
    }));
    stages.push_back(Stage("late", {"close"}, [&] { order.push_back("late"); return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&presenter);

    // Split deliberately, so a failure names WHICH link broke rather than just
    // "the boot did not abort":
    CHECK(wasOpenBeforeClose);            // the splash really was up when we closed it
    CHECK(sawClosed);                     // the OS destroy path really clears IsOpen()
    CHECK(r.quitRequested);               // ... and BootSequence turned that into a quit
    CHECK_FALSE(r.ok);
    CHECK_FALSE(Ran(order, "late"));      // ... and stopped, rather than finishing the boot

    splash.Close();   // idempotent; the window is already gone on the happy path
}

TEST_CASE("BootSequence aborts when the user closes the splash during a worker overlap", "[boot]")
{
    // The other present() call site: while a Worker stage runs and the main
    // thread has nothing ready, BootSequence pumps the presenter itself
    // (BootSequence.cpp:244-257). A close landing in THAT window -- which is
    // most of a real editor boot, since project_open overlaps gpu_core -- must
    // abort just the same.
    Arcane::BootSplashWindow splash("");
    REQUIRE(WaitUntilOpen(splash));

    Arcane::BootSplashPresenter presenter(&splash);

    std::atomic<bool> sawClosed{false};
    std::atomic<bool> ranAfter{false};

    std::vector<Arcane::BootStage> stages;
    stages.push_back(Stage("worker", {}, [&]
    {
        // Let the idle pump tick (8ms cadence) enough times to arm the
        // presenter before the close -- generous by ~an order of magnitude.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sawClosed.store(UserCloseSplash(splash));
        // Stay alive briefly afterwards so the pump gets calls that see the
        // CLOSED splash; without this the worker could finish first and the
        // abort would come from the post-stage call site instead, which is
        // the other test's job.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    }, Arcane::BootThread::Worker));
    stages.push_back(Stage("after", {"worker"}, [&] { ranAfter.store(true); return true; }));

    Arcane::BootSequence seq(std::move(stages));
    const Arcane::BootResult r = seq.Run(&presenter);

    CHECK(sawClosed.load());
    CHECK(r.quitRequested);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(ranAfter.load());

    splash.Close();
}

#endif
