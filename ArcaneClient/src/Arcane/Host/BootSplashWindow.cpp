#include <Arcane/Host/BootSplashWindow.hpp>

#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8() -- exe-relative image path resolution
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/NativeWindow.hpp>

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
    namespace
    {
        // NativeWindow::PostUser ids (OnUser's `msg`), handled on the window
        // thread -- the direct replacement for the old kMsgSetProgress (a raw
        // WM_APP+1) now that thread/class/message-loop ownership lives in
        // NativeWindow.
        constexpr unsigned kUserSetProgress = 1;   // wParam = integer percent
        constexpr unsigned kUserLoadImage   = 2;   // posted from OnCreate so the decode runs AFTER the first paint

        // Height of the status-text strip along the bottom edge, shared by
        // PaintSplash (the fill + text rect) and SetStatusText (the narrowed
        // Invalidate) so the two can never drift apart -- both need the EXACT
        // same rect, or a text-only repaint could invalidate a region
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
    }

    // The splash IS a presenter now (spec S7): NativeWindow owns the thread,
    // the class, the message loop and the close rules; this owns only what
    // the splash draws -- the image, the status line, the taskbar progress --
    // and the window-thread-only resources behind them (GDI+, COM). Lifted
    // out of this same file in Task 2 (see NativeWindow.hpp/.cpp); this Impl
    // is what is left once thread/class/message-loop ownership moves there.
    struct BootSplashWindow::Impl final : INativeWindowPresenter
    {
        NativeWindow      window;
        std::string       imagePath;
        std::mutex        textMutex;     // statusText: written by any thread, read by OnPaint
        std::string       statusText;
        std::atomic<bool> showProgress{true};
        int               lastPercent = -1;   // SetProgress dedupe (boot/main thread only)

        // Window-thread-owned, acquired in OnCreate, released in OnDestroy --
        // the same rule the old splash thread's own body followed (create
        // near the top, tear down in reverse order once the message loop
        // exits); OnCreate/OnDestroy are NativeWindow's equivalent hooks, both
        // called on the window thread only.
        std::unique_ptr<Gdiplus::Bitmap>      bitmap;
        ULONG_PTR                             gdiplusToken   = 0;
        bool                                  gdiplusOk      = false;
        Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
        bool                                  comInitialized = false;

        void OnCreate(void*) override
        {
            Gdiplus::GdiplusStartupInput in;
            gdiplusOk      = Gdiplus::GdiplusStartup(&gdiplusToken, &in, nullptr) == Gdiplus::Ok;
            comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            // The image decodes AFTER the window is up and painted once:
            // NativeWindow shows the window right after OnCreate returns and
            // UpdateWindow paints synchronously, so this posted message is
            // dequeued only after that first paint -- the "~100 ms to
            // something on screen" promise is about the WINDOW, not the image.
            window.PostUser(kUserLoadImage);
        }

        void OnPaint(void* hdc, int left, int top, int right, int bottom) override;   // defined below, after PaintSplash

        bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t) override
        {
            if (msg == kUserLoadImage)
            {
                if (gdiplusOk && !imagePath.empty())
                {
                    const std::wstring wpath = ResolveImagePathWide(imagePath);
                    auto bmp = std::make_unique<Gdiplus::Bitmap>(wpath.c_str());
                    if (bmp->GetLastStatus() == Gdiplus::Ok)
                    {
                        bitmap = std::move(bmp);
                        window.Invalidate();
                    }
                    // else: missing/corrupt/unreadable -> the class brush stays the whole splash
                }
                return true;
            }
            if (msg == kUserSetProgress)
            {
                // Lazy, this-thread-only creation (same reasoning as before:
                // the thread that CREATES the COM interface must be the same
                // thread that CALLS it -- SetProgress() itself only posts).
                if (!taskbar && comInitialized)
                {
                    Microsoft::WRL::ComPtr<ITaskbarList3> tbl;
                    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tbl))) &&
                        SUCCEEDED(tbl->HrInit()))
                        taskbar = tbl;
                    // Any failure above leaves `taskbar` null forever -- every
                    // future kUserSetProgress is then a silent no-op, matching
                    // "no taskbar progress" as the documented degrade.
                }
                if (taskbar)
                {
                    const HWND h = static_cast<HWND>(window.Hwnd());
                    const int percent = static_cast<int>(w);
                    // Mirrors WindowsPlatformSplash.cpp:769-781 exactly: 100%
                    // clears the overlay instead of leaving a full bar stuck
                    // on the taskbar icon after the splash is gone.
                    if (percent >= 100) taskbar->SetProgressState(h, TBPF_NOPROGRESS);
                    else                taskbar->SetProgressValue(h, static_cast<ULONGLONG>(percent), 100ULL);
                }
                return true;
            }
            return false;
        }

        void OnDestroy() override
        {
            taskbar.Reset();
            if (comInitialized) CoUninitialize();
            bitmap.reset();
            if (gdiplusOk) Gdiplus::GdiplusShutdown(gdiplusToken);
        }
    };

    namespace
    {
        // WM_PAINT's body, split out for readability. `impl` is never
        // invalid when called: Impl::OnPaint (below) is invoked by
        // NativeWindow only on the window thread, passing *this, and Impl
        // outlives the window (NativeWindow::Close() -- and the dtor's Close()
        // before it -- joins the window thread before Impl can be destroyed).
        // `paintRect` is BeginPaint's own PAINTSTRUCT::rcPaint -- the region
        // actually invalidated -- so a SetStatusText-only repaint (which
        // invalidates just the text row; see SetStatusText's own comment) can
        // skip the expensive bicubic bitmap redraw entirely instead of
        // re-running it on every stage-label change (2026-07-30 review round
        // 2, finding 3: this fires up to ~125/sec while a Worker stage
        // overlaps -- BootSequence.cpp's 8ms idle-pump cadence -- and was
        // burning a core fraction on exactly the CPU-bound overlap this DAG
        // exists to exploit). Draws, in order: (nothing here -- the class
        // background brush already filled via WM_ERASEBKGND, the guaranteed
        // floor), the splash bitmap (best-effort, scaled to fit with a
        // margin, skipped when unavailable OR when this repaint's region
        // does not touch it), the status line (bottom-left, matching
        // WindowsPlatformSplash.cpp:99-116's StartupProgress slot).
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
            // regardless of text content: SetStatusText's Invalidate calls
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
    }

    void BootSplashWindow::Impl::OnPaint(void* hdc, int left, int top, int right, int bottom)
    {
        const RECT paint{ left, top, right, bottom };
        PaintSplash(static_cast<HWND>(window.Hwnd()), *this, static_cast<HDC>(hdc), paint);
    }

    BootSplashWindow::BootSplashWindow(const char* imagePath) noexcept
        : m_impl(nullptr)
    {
        try
        {
            m_impl = std::make_unique<Impl>();
            m_impl->imagePath = imagePath ? imagePath : "";
            // Seed the status line BEFORE the window thread exists.
            // statusText otherwise default-constructs empty and PaintSplash
            // skips the row entirely when it is, so the splash showed a blank
            // status strip for the whole first stretch of every launch --
            // about a second, since BootSequence makes no present() call
            // until its first stage COMPLETES (the same gap that produced
            // Task 8d's quit bug; it is merely cosmetic now, but it is the
            // first thing a user sees). UE seeds its own splash the same way
            // and for the same reason (WindowsPlatformSplash.cpp:663-664 sets
            // the startup-progress slot before the splash thread starts).
            //
            // No mutex despite textMutex guarding this field everywhere else:
            // this store is sequenced-before window.Open() below, which
            // starts the window thread and is itself a synchronisation point,
            // so the window thread's first WM_PAINT is guaranteed to see it.
            // There is no other thread in existence yet to race with.
            m_impl->statusText = "Loading...";

            NativeWindowDesc d;
            d.className     = L"ArcaneBootSplash";   // BootSplashPresenterTest finds the window by this name
            d.title         = L"Arcane";
            d.width         = 480;
            d.height        = 270;
            d.popup         = true;
            d.topmost       = true;
            d.appWindow     = true;                  // taskbar button, so ITaskbarList3 has somewhere to draw (WindowsPlatformSplash.cpp:451-452)
            d.backgroundRgb = 0x0D0D0F;              // RGB(13, 13, 15), the brush PaintSplash reads back via GCLP_HBRBACKGROUND
            m_impl->window.Open(d, m_impl.get());
        }
        catch (...) { m_impl.reset(); }   // never fail boot for a splash
    }

    void BootSplashWindow::Close() noexcept { if (m_impl) m_impl->window.Close(); }

    bool BootSplashWindow::IsOpen() const noexcept { return m_impl && m_impl->window.IsOpen(); }

    // "An OS window for this splash existed at some point" -- see
    // NativeWindow::WasEverOpen()'s own doc comment for the ordering
    // guarantee (stored right after `open`, never cleared); the archaeology
    // of WHY this exists (Task 8d, 2026-07-30) now lives in
    // BootSplashWindow.hpp's BootSplashPresenter::Present, the one consumer.
    bool BootSplashWindow::WasEverOpen() const noexcept { return m_impl && m_impl->window.WasEverOpen(); }

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
            if (m_impl->statusText == text) return;
            m_impl->statusText = std::move(text);
            changed = true;
        }
        catch (...) { return; }   // e.g. std::bad_alloc -- no status update, never fail boot
        if (!changed) return;
        if (HWND h = static_cast<HWND>(m_impl->window.Hwnd()))
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
            m_impl->window.Invalidate(client.left, client.bottom - kTextRowHeightPx, client.right, client.bottom);
        }
    }

    void BootSplashWindow::SetProgress(float fraction01) noexcept
    {
        if (!m_impl || !m_impl->window.Hwnd()) return;   // no window yet (or already closed): nothing to show progress on
        const float clamped = fraction01 < 0.0f ? 0.0f : (fraction01 > 1.0f ? 1.0f : fraction01);
        const int percent = static_cast<int>(clamped * 100.0f + 0.5f);
        // Dedupe (2026-07-31 review, polish 3): same reasoning as
        // SetStatusText's dedupe above -- BootSequence's present() cadence
        // posts up to ~125/sec while a Worker stage overlaps, and an
        // unchanged percent would otherwise post, then (once handled) call
        // ITaskbarList3::SetProgressValue, every single time.
        if (m_impl->lastPercent == percent) return;
        m_impl->lastPercent = percent;
        // Post, never call directly: SetProgress() runs on the boot/main
        // thread, and impl->taskbar's COM interface belongs to the window
        // thread (see Impl::taskbar's comment). PostUser is non-blocking,
        // matching IBootPresenter's tick-cadence contract.
        m_impl->window.PostUser(kUserSetProgress, static_cast<std::uintptr_t>(percent));
    }

    void BootSplashWindow::SetShowProgress(bool show) noexcept { if (m_impl) m_impl->showProgress.store(show); }

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
