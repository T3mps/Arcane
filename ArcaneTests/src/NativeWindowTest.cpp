// NativeWindow (crash window plan 2, task 2): the thread-owned Win32 window
// the splash and the reporter present on. Behavioural, not visual: creation
// on its own thread, the presenter callbacks, the posted-message seam, and
// BOTH close paths (Close() and the user's Alt+F4).
#if defined(_WIN32)
#include <Arcane/Platform/NativeWindow.hpp>
#include <catch2/catch_test_macros.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace
{
    struct CountingPresenter final : Arcane::INativeWindowPresenter
    {
        std::atomic<int>      creates{0}, destroys{0}, paints{0}, sizes{0};
        std::atomic<unsigned> createThread{0};
        std::atomic<unsigned> lastUserMsg{0};
        std::atomic<std::uintptr_t> lastUserW{0};
        void OnCreate(void*) override { ++creates; createThread.store(GetCurrentThreadId()); }
        void OnPaint(void*, int, int, int, int) override { ++paints; }
        void OnSize(int, int) override { ++sizes; }
        bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t) override { lastUserMsg.store(msg); lastUserW.store(w); return true; }
        void OnDestroy() override { ++destroys; }
    };

    bool PollUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return pred();
    }

    HWND FindByClass(const wchar_t* cls)
    {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, cls, nullptr)) != nullptr)
        {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return h;
        }
        return nullptr;
    }
}

TEST_CASE("NativeWindow creates on its own thread, runs the presenter, and Close() destroys it once", "[platform]")
{
    CountingPresenter p;
    Arcane::NativeWindow w;
    CHECK_FALSE(w.IsOpen());
    CHECK_FALSE(w.WasEverOpen());
    CHECK(w.Dpi() == 96u);

    Arcane::NativeWindowDesc d;
    d.className = L"ArcaneNativeWindowTest";
    d.title     = L"NativeWindow test";
    w.Open(d, &p);
    REQUIRE(w.WaitUntilReady(5000));
    CHECK(w.IsOpen());
    CHECK(w.WasEverOpen());
    CHECK(w.Hwnd() != nullptr);
    CHECK(p.creates.load() == 1);
    CHECK(p.createThread.load() != GetCurrentThreadId());
    CHECK(w.Dpi() >= 96u);
    CHECK(PollUntil([&] { return p.paints.load() >= 1; }));

    w.PostUser(7, 42, 0);
    CHECK(PollUntil([&] { return p.lastUserMsg.load() == 7u && p.lastUserW.load() == 42u; }));

    w.SetTitle(L"renamed");
    CHECK(PollUntil([&]
    {
        wchar_t buf[64]{}; GetWindowTextW(static_cast<HWND>(w.Hwnd()), buf, 64);
        return std::wstring(buf) == L"renamed";
    }));

    w.Close();
    CHECK_FALSE(w.IsOpen());
    CHECK(w.WasEverOpen());
    CHECK(p.destroys.load() == 1);
    w.Close();   // idempotent
    CHECK(p.destroys.load() == 1);
}

TEST_CASE("NativeWindow: the user's Alt+F4 ends the window thread and Wait() returns", "[platform]")
{
    CountingPresenter p;
    Arcane::NativeWindow w;
    Arcane::NativeWindowDesc d;
    d.className = L"ArcaneNativeWindowTest";
    d.popup = false;   // an overlapped window has the system menu Alt+F4 drives
    w.Open(d, &p);
    REQUIRE(w.WaitUntilReady(5000));

    HWND h = FindByClass(L"ArcaneNativeWindowTest");
    REQUIRE(h != nullptr);
    PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);   // the exact message DefWindowProc synthesises from Alt+F4
    w.Wait();
    CHECK_FALSE(w.IsOpen());
    CHECK(w.WasEverOpen());
    CHECK(w.Hwnd() == nullptr);
    CHECK(p.destroys.load() == 1);
    w.Close();   // after an OS destroy: no join hang, no double destroy
    CHECK(p.destroys.load() == 1);
}

TEST_CASE("NativeWindow: Close() before creation completes and Close() on a never-opened window are both safe", "[platform]")
{
    {
        CountingPresenter p;
        Arcane::NativeWindow w;
        Arcane::NativeWindowDesc d;
        d.className = L"ArcaneNativeWindowTest";
        w.Open(d, &p);
        w.Close();   // no WaitUntilReady: Close must wait for the attempt itself
        CHECK_FALSE(w.IsOpen());
        CHECK(p.creates.load() == p.destroys.load());
    }
    {
        Arcane::NativeWindow w;
        w.Close();
        w.Wait();
        CHECK_FALSE(w.WasEverOpen());
    }
}
#endif
