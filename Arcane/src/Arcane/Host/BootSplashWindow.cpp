#include <Arcane/Host/BootSplashWindow.hpp>

#include <Arcane/Base/Log.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#endif

namespace Arcane
{
#if defined(_WIN32)
    struct BootSplashWindow::Impl
    {
        std::thread       thread;
        std::atomic<HWND> hwnd{nullptr};
        std::atomic<bool> open{false};

        // Set exactly once, on the splash thread, right after window creation
        // has been ATTEMPTED (whether it succeeded or failed) -- BEFORE the
        // thread ever blocks in the message loop below. Close() waits on this
        // instead of reading hwnd directly: without it, a Close() that lands in
        // the tiny window between CreateWindowExW succeeding and hwnd being
        // stored would see hwnd == nullptr, skip posting WM_CLOSE, and then
        // block forever in thread.join() while the splash thread sits in
        // GetMessageW with nobody left to ever post it a quit message -- a
        // permanent hang, which is worse than "no splash" and exactly the kind
        // of failure this class must never produce.
        std::atomic<bool> ready{false};
        std::string       imagePath;
    };

    namespace
    {
        LRESULT CALLBACK SplashProc(HWND h, UINT msg, WPARAM w, LPARAM l)
        {
            if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
            return DefWindowProcW(h, msg, w, l);
        }
    }

    BootSplashWindow::BootSplashWindow(const char* imagePath) noexcept
        : m_impl(nullptr)
    {
        try
        {
            m_impl = std::make_unique<Impl>();
            m_impl->imagePath = imagePath ? imagePath : "";
            m_impl->thread = std::thread([impl = m_impl.get()]
            {
                WNDCLASSEXW wc{};
                wc.cbSize        = sizeof(wc);
                wc.lpfnWndProc   = SplashProc;
                wc.hInstance     = GetModuleHandleW(nullptr);
                wc.hbrBackground = CreateSolidBrush(RGB(13, 13, 15));
                wc.lpszClassName = L"ArcaneBootSplash";
                RegisterClassExW(&wc);

                constexpr int kW = 480, kH = 270;
                const int x = (GetSystemMetrics(SM_CXSCREEN) - kW) / 2;
                const int y = (GetSystemMetrics(SM_CYSCREEN) - kH) / 2;

                HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                         wc.lpszClassName, L"Arcane",
                                         WS_POPUP, x, y, kW, kH,
                                         nullptr, nullptr, wc.hInstance, nullptr);
                if (!h)
                {
                    // RegisterClassExW's return value is deliberately not checked
                    // above: if registration truly failed, CreateWindowExW fails
                    // right here for the same reason, so this one check already
                    // covers both -- no splash, boot continues.
                    impl->ready.store(true);
                    impl->ready.notify_all();
                    return;
                }
                impl->hwnd.store(h);
                impl->open.store(true);
                impl->ready.store(true);
                impl->ready.notify_all();
                ShowWindow(h, SW_SHOW);
                UpdateWindow(h);

                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                impl->open.store(false);
            });
        }
        catch (...)
        {
            // Never fail boot for a splash.
            m_impl.reset();
        }
    }

    void BootSplashWindow::Close() noexcept
    {
        if (!m_impl) return;
        // Wait for window creation to have been attempted before touching hwnd
        // -- see the Impl::ready comment above for why this is required, not
        // optional.
        m_impl->ready.wait(false);
        if (HWND h = m_impl->hwnd.exchange(nullptr))
            PostMessageW(h, WM_CLOSE, 0, 0);
        if (m_impl->thread.joinable())
            m_impl->thread.join();
        m_impl->open.store(false);
    }

    bool BootSplashWindow::IsOpen() const noexcept
    {
        return m_impl && m_impl->open.load();
    }

    BootSplashWindow::~BootSplashWindow() { Close(); }
#else
    struct BootSplashWindow::Impl {};
    BootSplashWindow::BootSplashWindow(const char*) noexcept : m_impl(nullptr) {}
    BootSplashWindow::~BootSplashWindow() = default;
    void BootSplashWindow::Close() noexcept {}
    bool BootSplashWindow::IsOpen() const noexcept { return false; }
#endif
}
