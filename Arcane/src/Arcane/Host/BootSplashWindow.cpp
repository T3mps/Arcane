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
#include <objbase.h>     // CoInitializeEx/CoCreateInstance/CoUninitialize (WIN32_LEAN_AND_MEAN excludes these from windows.h)
#include <shobjidl.h>    // ITaskbarList3 / CLSID_TaskbarList (WindowsPlatformSplash.cpp:769-781's Windows counterpart)
#include <objidl.h>      // IStream, ahead of gdiplus.h (standard GDI+ include order)
#include <gdiplus.h>      // Gdiplus::Bitmap -- decodes the PNG splash image (pre-device: no Assets facade exists yet)
#include <wrl/client.h>   // Microsoft::WRL::ComPtr (same pattern as ShaderCompiler.cpp)

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "gdiplus.lib")
// shell32.lib (CLSID_TaskbarList) and ole32.lib (Co* functions) are already
// linked into the Arcane project (premake5.lua's "system:windows" links
// block) -- only gdiplus.lib is new here.
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

        // Status line, guarded by textMutex: written from SetStatusText (any
        // thread -- the boot/main thread via BootSplashPresenter), read from
        // WM_PAINT on the splash thread. Always stored, even before the window
        // exists or after Close() -- SetStatusText's "safe to call anytime"
        // does not mean the value is dropped; it just means no repaint gets
        // scheduled while there is no window to repaint.
        std::mutex        textMutex;
        std::string       statusText;

        // Splash-thread-owned; touched ONLY on that thread (loaded once before
        // the message loop starts, released once it exits -- see the thread
        // lambda). A missing/unreadable image, or a GDI+ Startup failure,
        // leaves this null; WM_PAINT already tolerates that (the class
        // background brush, painted via the default WM_ERASEBKGND handling,
        // is the guaranteed floor).
        std::unique_ptr<Gdiplus::Bitmap> bitmap;

        // Splash-thread-owned; created lazily the first time kMsgSetProgress
        // is handled (see SplashProc below), so the thread that CREATES the
        // COM interface is always the same thread that CALLS it --
        // SetProgress() (called from the boot/main thread) never touches COM
        // itself, only PostMessageW's the request over.
        Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
        bool comInitialized = false;   // whether THIS thread's CoInitializeEx succeeded (pairs the CoUninitialize)
    };

    namespace
    {
        // Custom message: "a new progress fraction is available, quantized to
        // an integer percent in wParam" -- posted by SetProgress() (any
        // thread) and handled on the splash thread, which is the only thread
        // allowed to touch impl->taskbar (see Impl::taskbar's comment above).
        constexpr UINT kMsgSetProgress = WM_APP + 1;

        std::wstring Utf8ToWide(const std::string& s)
        {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0) return {};
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        // WM_PAINT's body, split out for readability. `impl` is never null
        // when called (SplashProc guards). Draws, in order: (nothing here --
        // the class background brush already filled via WM_ERASEBKGND, the
        // guaranteed floor), the splash bitmap (best-effort, scaled to fit
        // with a margin, skipped entirely if unavailable), the status line
        // (bottom-left, matching WindowsPlatformSplash.cpp:99-116's
        // StartupProgress slot).
        void PaintSplash(HWND h, BootSplashWindow::Impl& impl, HDC hdc)
        {
            RECT client{};
            GetClientRect(h, &client);
            const float clientW = static_cast<float>(client.right - client.left);
            const float clientH = static_cast<float>(client.bottom - client.top);

            if (impl.bitmap && impl.bitmap->GetLastStatus() == Gdiplus::Ok)
            {
                const UINT bw = impl.bitmap->GetWidth();
                const UINT bh = impl.bitmap->GetHeight();
                if (bw > 0 && bh > 0)
                {
                    constexpr float kMarginPx  = 12.0f;
                    constexpr float kTextRowPx = 24.0f;   // reserve room so the image never touches the status line
                    const float availW = clientW - 2.0f * kMarginPx;
                    const float availH = clientH - 2.0f * kMarginPx - kTextRowPx;
                    if (availW > 0.0f && availH > 0.0f)
                    {
                        const float scale = std::min(availW / static_cast<float>(bw), availH / static_cast<float>(bh));
                        const float dw = static_cast<float>(bw) * scale;
                        const float dh = static_cast<float>(bh) * scale;
                        const float dx = (clientW - dw) * 0.5f;
                        const float dy = kMarginPx + (availH - dh) * 0.5f;

                        Gdiplus::Graphics graphics(hdc);
                        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                        graphics.DrawImage(impl.bitmap.get(), dx, dy, dw, dh);
                    }
                }
            }

            std::string text;
            {
                std::lock_guard<std::mutex> lk(impl.textMutex);
                text = impl.statusText;
            }

            // The text row's own background is re-filled on EVERY repaint,
            // regardless of text content: SetStatusText's InvalidateRect calls
            // pass bErase=FALSE (matching WindowsPlatformSplash.cpp:808's same
            // choice, for the same reason -- redrawing the whole background on
            // every stage-label change would flicker), so without this a
            // SHORTER new label would leave the tail of a longer old one on
            // screen. GetClassLongPtrW reads back the same brush the window
            // class was registered with, rather than duplicating the colour
            // constant here.
            RECT textRow = client;
            textRow.top = client.bottom - 24;
            if (HBRUSH bg = reinterpret_cast<HBRUSH>(GetClassLongPtrW(h, GCLP_HBRBACKGROUND)))
                FillRect(hdc, &textRow, bg);

            if (!text.empty())
            {
                const std::wstring wtext = Utf8ToWide(text);
                RECT textRect = client;
                textRect.left   += 12;
                textRect.right  -= 12;
                textRect.top    = client.bottom - 24;
                textRect.bottom = client.bottom - 6;

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(160, 160, 160));   // matches WindowsPlatformSplash.cpp's StartupProgress colour
                DrawTextW(hdc, wtext.c_str(), -1, &textRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }

        LRESULT CALLBACK SplashProc(HWND h, UINT msg, WPARAM w, LPARAM l)
        {
            // GWLP_USERDATA is set right after CreateWindowExW succeeds, before
            // ShowWindow -- see the thread lambda below -- so it is always
            // valid by the time WM_PAINT or kMsgSetProgress can fire. Still
            // null-checked: WM_NCCREATE/WM_CREATE (sent synchronously inside
            // CreateWindowExW, before that store) could in principle reach here
            // first, and this must never crash.
            auto* impl = reinterpret_cast<BootSplashWindow::Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));

            if (msg == WM_PAINT)
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(h, &ps);
                if (impl) PaintSplash(h, *impl, hdc);
                EndPaint(h, &ps);
                return 0;
            }
            if (msg == kMsgSetProgress)
            {
                if (impl)
                {
                    // Lazy, this-thread-only creation (see Impl::taskbar).
                    if (!impl->taskbar && impl->comInitialized)
                    {
                        Microsoft::WRL::ComPtr<ITaskbarList3> tbl;
                        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                                        IID_PPV_ARGS(&tbl))) &&
                            SUCCEEDED(tbl->HrInit()))
                        {
                            impl->taskbar = tbl;
                        }
                        // Any failure above (COM not initialised, no shell
                        // integration, HrInit failing) leaves impl->taskbar
                        // null forever -- every future kMsgSetProgress is then
                        // a silent no-op, matching "no taskbar progress" as
                        // the documented degrade.
                    }
                    if (impl->taskbar)
                    {
                        const int percent = static_cast<int>(w);
                        // Mirrors WindowsPlatformSplash.cpp:769-781 exactly:
                        // 100% clears the overlay instead of leaving a full bar
                        // stuck on the taskbar icon after the splash is gone.
                        if (percent >= 100)
                            impl->taskbar->SetProgressState(h, TBPF_NOPROGRESS);
                        else
                            impl->taskbar->SetProgressValue(h, static_cast<ULONGLONG>(percent), 100ULL);
                    }
                }
                return 0;
            }
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
                // The ENTIRE thread body is one try/catch: this class's
                // contract is that it can NEVER fail boot, and an exception
                // escaping a std::thread's entry function calls
                // std::terminate() -- the one failure mode strictly worse
                // than "no splash". Everything below only sets flags and
                // atomics that Close()/IsOpen()/SetStatusText/SetProgress
                // already treat as optional, so catching here and falling
                // through to "the thread just ends" is safe.
                try
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
                    // GWLP_USERDATA before ShowWindow: SplashProc's first
                    // WM_PAINT (from ShowWindow/UpdateWindow below) must be
                    // able to find `impl`.
                    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
                    impl->hwnd.store(h);
                    impl->open.store(true);
                    impl->ready.store(true);
                    impl->ready.notify_all();
                    ShowWindow(h, SW_SHOW);
                    UpdateWindow(h);   // paints the background (+ whatever text was set ahead of time)

                    // Image + COM, AFTER the window is already up and painted
                    // once: neither gates "something on screen within ~100ms
                    // of the click" -- that promise is about the WINDOW
                    // appearing, not the fully-decorated splash. Best-effort;
                    // every failure below just leaves the plain background
                    // (image) or a taskbar with no progress overlay (COM).
                    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
                    ULONG_PTR gdiplusToken = 0;
                    const bool gdiplusOk =
                        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok;
                    if (gdiplusOk && !impl->imagePath.empty())
                    {
                        const std::wstring wpath = Utf8ToWide(impl->imagePath);
                        auto bmp = std::make_unique<Gdiplus::Bitmap>(wpath.c_str());
                        if (bmp->GetLastStatus() == Gdiplus::Ok)
                        {
                            impl->bitmap = std::move(bmp);
                            InvalidateRect(h, nullptr, FALSE);   // repaint now that the image is ready
                        }
                        // else: leave impl->bitmap null (missing/corrupt/
                        // unreadable file) -- degrade to the solid background.
                    }

                    impl->comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

                    MSG msg;
                    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    impl->open.store(false);

                    // Splash-thread-owned teardown, reverse order of acquisition.
                    impl->taskbar.Reset();
                    if (impl->comInitialized) CoUninitialize();
                    impl->bitmap.reset();
                    if (gdiplusOk) Gdiplus::GdiplusShutdown(gdiplusToken);
                }
                catch (...)
                {
                    // Never let an exception escape the thread entry point.
                    // If this fires before `ready` was ever stored (a throw
                    // during RegisterClassExW/CreateWindowExW setup -- none of
                    // the plain Win32 calls above actually throw, but the
                    // catch stays unconditional per this class's contract),
                    // release Close()'s wait so it cannot hang forever.
                    impl->open.store(false);
                    impl->ready.store(true);
                    impl->ready.notify_all();
                }
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

    void BootSplashWindow::SetStatusText(std::string text) noexcept
    {
        if (!m_impl) return;
        try
        {
            std::lock_guard<std::mutex> lk(m_impl->textMutex);
            m_impl->statusText = std::move(text);
        }
        catch (...) { return; }   // e.g. std::bad_alloc -- no status update, never fail boot
        // nullptr would invalidate EVERY top-level window on the desktop, not
        // "no-op" -- must check for a real hwnd before calling.
        if (HWND h = m_impl->hwnd.load())
            InvalidateRect(h, nullptr, FALSE);
    }

    void BootSplashWindow::SetProgress(float fraction01) noexcept
    {
        if (!m_impl) return;
        HWND h = m_impl->hwnd.load();
        if (!h) return;   // no window yet (or already closed): nothing to show progress on
        const float clamped = fraction01 < 0.0f ? 0.0f : (fraction01 > 1.0f ? 1.0f : fraction01);
        const int percent = static_cast<int>(clamped * 100.0f + 0.5f);
        // Post, never call directly: SetProgress() runs on the boot/main
        // thread, and impl->taskbar's COM interface belongs to the splash
        // thread (see Impl::taskbar's comment). PostMessageW is non-blocking,
        // matching IBootPresenter's tick-cadence contract.
        PostMessageW(h, kMsgSetProgress, static_cast<WPARAM>(percent), 0);
    }

    BootSplashWindow::~BootSplashWindow() { Close(); }
#else
    struct BootSplashWindow::Impl {};
    BootSplashWindow::BootSplashWindow(const char*) noexcept : m_impl(nullptr) {}
    BootSplashWindow::~BootSplashWindow() = default;
    void BootSplashWindow::Close() noexcept {}
    bool BootSplashWindow::IsOpen() const noexcept { return false; }
    void BootSplashWindow::SetStatusText(std::string) noexcept {}
    void BootSplashWindow::SetProgress(float) noexcept {}
#endif
}
