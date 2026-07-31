#include <Arcane/Host/BootSplashWindow.hpp>

#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8() -- exe-relative image path resolution
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

        // Monotonic latch: set true just after the open.store(true) below, and
        // NEVER cleared -- "an OS window for this splash existed at some
        // point", as distinct from `open`'s "it exists right now". See that
        // store site for the exact ordering guarantee it does and does not
        // provide.
        //
        // Task 8d (2026-07-30): this is the fact BootSplashPresenter needs and
        // could not previously get. It used to infer it from its own call
        // history (an m_armed flag set the first time Present() happened to
        // observe IsOpen() == true), which silently required a Present() call
        // to LAND while the window was up. Measured on the real editor, the
        // first present() call does not happen until the first boot stage
        // COMPLETES -- about a second after the splash appears -- so a user
        // closing the splash in that window left the presenter with no
        // evidence it had ever been open, and the boot ran to completion. See
        // BootSplashWindow.hpp's BootSplashPresenter::Present for the full
        // reasoning. Owned here rather than there because THIS is the thread
        // that actually knows.
        std::atomic<bool> everOpen{false};

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

        // Gate for BootSplashPresenter::Present's forwarding of status text +
        // taskbar progress -- see SetShowProgress/ShowProgress's own comments
        // in the header. Defaults true (this class's behaviour before the flag
        // existed); RuntimeApp explicitly flips it false before BootSequence::
        // Run begins, per the spec default for a non-editor host. A plain
        // atomic<bool>, like open/everOpen/ready above: written from any
        // thread (the boot/main thread via RuntimeApp, or project_open's
        // worker-thread stage body once a project's manifest is known), read
        // from the boot/main thread inside BootSplashPresenter::Present.
        std::atomic<bool> showProgress{true};

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

        // Height of the status-text strip along the bottom edge, shared by
        // PaintSplash (the fill + text rect) and SetStatusText (the narrowed
        // InvalidateRect) so the two can never drift apart -- both need the
        // EXACT same rect, or a text-only repaint could invalidate a region
        // PaintSplash does not redraw (leaving stale pixels) or vice versa.
        constexpr LONG kTextRowHeightPx = 24;

        std::wstring Utf8ToWide(const std::string& s)
        {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0) return {};
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        // Resolve `imagePathUtf8` against the EXECUTABLE's own directory, not
        // the process CWD (2026-07-30 review round 2 minor). GDI+'s
        // Bitmap(filename) constructor otherwise resolves a relative path
        // against the CWD, which is not the codebase convention --
        // Window::SetIcon (Window.cpp:138-144) resolves exe-relative via
        // SDL_GetBasePath(). This uses Arcane::ExecutablePathUtf8() instead
        // (Engine.cpp): SDL's video subsystem is not initialised yet this
        // early (SDL_InitSubSystem(VIDEO) only happens inside
        // Window::Create, called from StageGpuCore, long after this
        // constructor runs), while ExecutablePathUtf8 is a bare
        // GetModuleFileNameW wrapper with no subsystem dependency at all --
        // already proven safe this early, since main() calls it for the
        // --print-engine-info probe before any engine boot. Both the exe
        // path and the caller's path are converted via the SAME explicit-
        // CP_UTF8 Utf8ToWide above, never through std::filesystem::path's
        // narrow constructor (which uses the ACTIVE CODE PAGE, not UTF-8, on
        // Windows) -- so a non-ASCII install path cannot mis-resolve here.
        // Every failure (an absolute path already given, ExecutablePathUtf8
        // returning empty, no path separator found) just returns the
        // caller's path unchanged -- the same CWD-relative behaviour as
        // before this fix, never worse.
        std::wstring ResolveImagePathWide(const std::string& imagePathUtf8)
        {
            if (imagePathUtf8.empty()) return {};
            const bool looksAbsolute =
                (imagePathUtf8.size() >= 2 && imagePathUtf8[1] == ':') ||          // "C:\..."
                (imagePathUtf8.size() >= 2 && imagePathUtf8[0] == '\\' && imagePathUtf8[1] == '\\') ||  // "\\server\..."
                (imagePathUtf8.size() >= 2 && imagePathUtf8[0] == '/'  && imagePathUtf8[1] == '/');     // "//server/..."
            const std::wstring wideImage = Utf8ToWide(imagePathUtf8);
            if (looksAbsolute) return wideImage;

            const std::string exeUtf8 = Arcane::ExecutablePathUtf8();
            if (exeUtf8.empty()) return wideImage;
            const std::wstring exeWide = Utf8ToWide(exeUtf8);
            const std::size_t slash = exeWide.find_last_of(L"/\\");
            if (slash == std::wstring::npos) return wideImage;

            return exeWide.substr(0, slash + 1) + wideImage;
        }

        // WM_PAINT's body, split out for readability. `impl` is never null
        // when called (SplashProc guards). `paintRect` is BeginPaint's own
        // PAINTSTRUCT::rcPaint -- the region actually invalidated -- so a
        // SetStatusText-only repaint (which invalidates just the text row;
        // see SetStatusText's own comment) can skip the expensive bicubic
        // bitmap redraw entirely instead of re-running it on every stage-
        // label change (2026-07-30 review round 2, finding 3: this fires up
        // to ~125/sec while a Worker stage overlaps -- BootSequence.cpp's
        // 8ms idle-pump cadence -- and was burning a core fraction on
        // exactly the CPU-bound overlap this DAG exists to exploit). Draws,
        // in order: (nothing here -- the class background brush already
        // filled via WM_ERASEBKGND, the guaranteed floor), the splash bitmap
        // (best-effort, scaled to fit with a margin, skipped when
        // unavailable OR when this repaint's region does not touch it), the
        // status line (bottom-left, matching WindowsPlatformSplash.cpp:
        // 99-116's StartupProgress slot).
        void PaintSplash(HWND h, BootSplashWindow::Impl& impl, HDC hdc, const RECT& paintRect)
        {
            RECT client{};
            GetClientRect(h, &client);
            const float clientW = static_cast<float>(client.right - client.left);
            const float clientH = static_cast<float>(client.bottom - client.top);

            RECT textRow = client;
            textRow.top = client.bottom - kTextRowHeightPx;

            RECT imageArea = client;
            imageArea.bottom = textRow.top;   // everything above the text row
            RECT dirtyImageArea{};
            const bool imageMaybeDirty = IntersectRect(&dirtyImageArea, &paintRect, &imageArea) != FALSE;

            if (imageMaybeDirty && impl.bitmap && impl.bitmap->GetLastStatus() == Gdiplus::Ok)
            {
                const UINT bw = impl.bitmap->GetWidth();
                const UINT bh = impl.bitmap->GetHeight();
                if (bw > 0 && bh > 0)
                {
                    constexpr float kMarginPx = 12.0f;
                    const float availW = clientW - 2.0f * kMarginPx;
                    // kTextRowHeightPx: reserve room so the image never touches the status line.
                    const float availH = clientH - 2.0f * kMarginPx - static_cast<float>(kTextRowHeightPx);
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
            // constant here. Cheap plain-GDI fill -- unlike the bitmap draw
            // above, this does not need a dirty-region gate.
            if (HBRUSH bg = reinterpret_cast<HBRUSH>(GetClassLongPtrW(h, GCLP_HBRBACKGROUND)))
                FillRect(hdc, &textRow, bg);

            if (!text.empty())
            {
                const std::wstring wtext = Utf8ToWide(text);
                RECT textRect = textRow;
                textRect.left   += 12;
                textRect.right  -= 12;
                textRect.bottom -= 6;

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
                if (impl) PaintSplash(h, *impl, hdc, ps.rcPaint);
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
            // Seed the status line BEFORE the thread exists. statusText
            // otherwise default-constructs empty and PaintSplash skips the row
            // entirely when it is, so the splash showed a blank status strip
            // for the whole first stretch of every launch -- about a second,
            // since BootSequence makes no present() call until its first stage
            // COMPLETES (the same gap that produced Task 8d's quit bug; it is
            // merely cosmetic now, but it is the first thing a user sees).
            // UE seeds its own splash the same way and for the same reason
            // (WindowsPlatformSplash.cpp:663-664 sets the startup-progress
            // slot before the splash thread starts).
            //
            // No mutex despite textMutex guarding this field everywhere else:
            // this store is sequenced-before the std::thread construction
            // below, which is itself a synchronisation point, so the splash
            // thread's first WM_PAINT is guaranteed to see it. There is no
            // other thread in existence yet to race with.
            m_impl->statusText = "Loading...";
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

                    // WS_EX_APPWINDOW, not WS_EX_TOOLWINDOW (2026-07-30 review
                    // round 2, finding 1): a tool window never gets a taskbar
                    // button, so SetProgress's ITaskbarList3 calls below had
                    // nowhere to render -- the overlay was silently a no-op
                    // for the ENTIRE splash lifetime, since the real window is
                    // also hidden until reveal and so has no taskbar button of
                    // its own either. UE forces exactly this for the editor
                    // (WindowsPlatformSplash.cpp:451-452: "Force the editor
                    // splash screen to show up in the taskbar and alt-tab
                    // lists" -> `GIsEditor ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW`)
                    // -- that style is WHY its SetProgress (:769-781) works at
                    // all. Consequence, taken deliberately: the splash now has
                    // a taskbar button and appears in Alt-Tab, matching UE's
                    // editor behaviour. WS_EX_TOPMOST stays.
                    HWND h = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST,
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
                    // everOpen right after open, before ready/ShowWindow, and
                    // never cleared. The ordering claim that actually holds is
                    // the one the quit detection needs: everOpen is stored
                    // while open is still true, and open is not cleared until
                    // the message loop exits (or Close() runs) much later --
                    // so any observer that reads open == false for a window
                    // that HAS existed is guaranteed to also see
                    // everOpen == true. That is the direction that matters.
                    //
                    // The reverse gap is real and deliberately tolerated:
                    // between the two stores above a reader can sample
                    // open == true with everOpen still false. That is
                    // harmless because the sole consumer short-circuits --
                    // BootSplashWindow.hpp's BootSplashPresenter::Present
                    // evaluates `!IsOpen() && WasEverOpen()`, so a true
                    // IsOpen() answers "no quit" without ever reading
                    // everOpen. A future consumer that reads everOpen WITHOUT
                    // that guard would need these two stores swapped; do not
                    // assume the stronger invariant holds today.
                    impl->everOpen.store(true);
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
                        const std::wstring wpath = ResolveImagePathWide(impl->imagePath);
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

    bool BootSplashWindow::WasEverOpen() const noexcept
    {
        return m_impl && m_impl->everOpen.load();
    }

    void BootSplashWindow::SetStatusText(std::string text) noexcept
    {
        if (!m_impl) return;
        bool changed = false;
        try
        {
            std::lock_guard<std::mutex> lk(m_impl->textMutex);
            // Dedupe (2026-07-30 review round 2, finding 3): BootSequence
            // calls present() with the SAME stageId up to ~125/sec while a
            // Worker stage overlaps (its 8ms idle-pump cadence), so without
            // this every one of those calls unconditionally re-stored and
            // repainted -- matches WindowsPlatformSplash.cpp:798-805's own
            // `bWasUpdated` guard.
            if (m_impl->statusText == text)
                return;
            m_impl->statusText = std::move(text);
            changed = true;
        }
        catch (...) { return; }   // e.g. std::bad_alloc -- no status update, never fail boot
        if (!changed) return;
        if (HWND h = m_impl->hwnd.load())
        {
            // Invalidate only the text row, not the whole window (same
            // finding): a full-window invalidate re-runs PaintSplash's
            // bicubic bitmap DrawImage on every stage-label change, which is
            // exactly the CPU cost this dedupe exists to avoid -- matches
            // WindowsPlatformSplash.cpp:809's InvalidateRect(...,
            // &GSplashScreenTextRects[InType], ...), one text slot only.
            // PaintSplash itself also gates the bitmap redraw on whether the
            // repaint's region reaches it (see its own comment), so this and
            // that guard are two halves of the same fix -- narrowing the
            // invalidated region alone would not help if the paint handler
            // redrew the bitmap unconditionally anyway.
            RECT client{};
            GetClientRect(h, &client);
            RECT textRow = client;
            textRow.top = client.bottom - kTextRowHeightPx;
            InvalidateRect(h, &textRow, FALSE);
        }
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

    void BootSplashWindow::SetShowProgress(bool show) noexcept
    {
        if (m_impl) m_impl->showProgress.store(show);
    }

    bool BootSplashWindow::ShowProgress() const noexcept
    {
        // No impl (construction failed) behaves like "showing" -- harmless,
        // since every consumer of the flag (SetStatusText/SetProgress) is
        // already a no-op with no window to draw into.
        return !m_impl || m_impl->showProgress.load();
    }

    BootSplashWindow::~BootSplashWindow() { Close(); }
#else
    struct BootSplashWindow::Impl {};
    BootSplashWindow::BootSplashWindow(const char*) noexcept : m_impl(nullptr) {}
    BootSplashWindow::~BootSplashWindow() = default;
    void BootSplashWindow::Close() noexcept {}
    bool BootSplashWindow::IsOpen() const noexcept { return false; }
    bool BootSplashWindow::WasEverOpen() const noexcept { return false; }
    void BootSplashWindow::SetStatusText(std::string) noexcept {}
    void BootSplashWindow::SetProgress(float) noexcept {}
    void BootSplashWindow::SetShowProgress(bool) noexcept {}
    bool BootSplashWindow::ShowProgress() const noexcept { return true; }
#endif
}
