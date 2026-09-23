#include <Arcane/Platform/NativeWindow.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <thread>
#endif

namespace Arcane
{
#if defined(_WIN32)
    struct NativeWindow::Impl
    {
        NativeWindowDesc        desc;
        INativeWindowPresenter* presenter = nullptr;
        std::thread             thread;
        std::atomic<HWND>       hwnd{nullptr};
        std::atomic<bool>       open{false};
        std::atomic<bool>       everOpen{false};   // monotonic; stored right after `open`, never cleared
        std::atomic<bool>       ready{false};      // creation ATTEMPTED (either way) -- Close() waits on this, not on hwnd
        std::atomic<DWORD>      threadId{0};
    };

    namespace
    {
        constexpr UINT kMsgUserBase = WM_APP;           // OnUser(msg) <-> WM_APP + msg, msg < 0x100
        constexpr UINT kMsgSetTitle = WM_APP + 0x100;   // lParam = wchar_t[] the window thread frees

        LRESULT CALLBACK NativeProc(HWND h, UINT msg, WPARAM w, LPARAM l)
        {
            // Set right after CreateWindowExW, before OnCreate/ShowWindow; still
            // null-checked because WM_NCCREATE/WM_CREATE arrive inside
            // CreateWindowExW, before the store.
            auto* impl = reinterpret_cast<NativeWindow::Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            INativeWindowPresenter* p = impl ? impl->presenter : nullptr;
            switch (msg)
            {
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(h, &ps);
                if (p) p->OnPaint(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_COMMAND:
                if (p) p->OnCommand(static_cast<int>(LOWORD(w)));
                return 0;
            case WM_SIZE:
                if (p) p->OnSize(static_cast<int>(LOWORD(l)), static_cast<int>(HIWORD(l)));
                return 0;
            case WM_DPICHANGED:
            {
                // Take the suggested rect; the WM_SIZE that follows re-lays out.
                const RECT* r = reinterpret_cast<const RECT*>(l);
                SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case kMsgSetTitle:
            {
                wchar_t* text = reinterpret_cast<wchar_t*>(l);
                if (text) { SetWindowTextW(h, text); delete[] text; }
                return 0;
            }
            case WM_DESTROY:
                // ANY destroy path (Close() or the OS's Alt+F4 chain) lands
                // here: clear the handle so later posts address nothing.
                if (p) p->OnDestroy();
                if (impl) { impl->hwnd.store(nullptr); impl->open.store(false); }
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            if (msg >= kMsgUserBase && msg < kMsgSetTitle && p &&
                p->OnUser(msg - kMsgUserBase, static_cast<std::uintptr_t>(w), static_cast<std::intptr_t>(l)))
                return 0;
            return DefWindowProcW(h, msg, w, l);
        }

        void WindowThread(NativeWindow::Impl* impl) noexcept
        {
            // The whole body is one try/catch: an exception escaping a
            // std::thread entry is std::terminate, the one outcome strictly
            // worse than "no window".
            try
            {
                impl->threadId.store(GetCurrentThreadId());
                const NativeWindowDesc& d = impl->desc;

                HINSTANCE hInstance = GetModuleHandleW(nullptr);

                // R46: query first -- a second window of the same class name
                // would otherwise call RegisterClassExW unconditionally, which
                // fails with ERROR_CLASS_ALREADY_EXISTS on the second and later
                // registrations while orphaning the brush this thread just
                // created (the class keeps the first one). Create the brush
                // only on the path that actually registers the class.
                WNDCLASSEXW existing{};
                existing.cbSize = sizeof(existing);
                if (!GetClassInfoExW(hInstance, d.className.c_str(), &existing))
                {
                    WNDCLASSEXW wc{};
                    wc.cbSize        = sizeof(wc);
                    wc.lpfnWndProc   = &NativeProc;
                    wc.hInstance     = hInstance;
                    wc.hbrBackground = CreateSolidBrush(RGB((d.backgroundRgb >> 16) & 0xFF,
                                                            (d.backgroundRgb >> 8) & 0xFF,
                                                            d.backgroundRgb & 0xFF));
                    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
                    wc.lpszClassName = d.className.c_str();
                    // A second window of the same class fails registration with
                    // ERROR_CLASS_ALREADY_EXISTS and creates fine; a real failure
                    // fails CreateWindowExW below for the same reason. Not checked.
                    RegisterClassExW(&wc);
                }

                const DWORD style   = d.popup ? WS_POPUP : WS_OVERLAPPEDWINDOW;
                const DWORD exStyle = (d.appWindow ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW)
                                    | (d.topmost ? WS_EX_TOPMOST : 0);
                const int x = (GetSystemMetrics(SM_CXSCREEN) - d.width) / 2;
                const int y = (GetSystemMetrics(SM_CYSCREEN) - d.height) / 2;
                HWND h = CreateWindowExW(exStyle, d.className.c_str(), d.title.c_str(), style,
                                         x, y, d.width, d.height, nullptr, nullptr, hInstance, nullptr);
                if (!h)
                {
                    impl->ready.store(true);
                    impl->ready.notify_all();
                    return;
                }
                SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
                impl->hwnd.store(h);
                impl->open.store(true);
                impl->everOpen.store(true);   // after open, before ready -- the ordering BootSplashPresenter relies on
                if (impl->presenter) impl->presenter->OnCreate(h);   // child controls exist before the first paint
                impl->ready.store(true);
                impl->ready.notify_all();
                ShowWindow(h, SW_SHOW);
                UpdateWindow(h);
                if (d.foreground) SetForegroundWindow(h);   // honoured only if the spawner granted it (AllowSetForegroundWindow)

                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                impl->open.store(false);
            }
            catch (...)
            {
                impl->open.store(false);
                impl->ready.store(true);
                impl->ready.notify_all();
            }
        }
    }

    NativeWindow::NativeWindow() noexcept : m_impl(nullptr) {}
    NativeWindow::~NativeWindow() { Close(); }

    void NativeWindow::Open(const NativeWindowDesc& desc, INativeWindowPresenter* presenter) noexcept
    {
        if (m_impl) return;
        try
        {
            m_impl = std::make_unique<Impl>();
            m_impl->desc      = desc;
            m_impl->presenter = presenter;
            m_impl->thread    = std::thread(&WindowThread, m_impl.get());
        }
        catch (...) { m_impl.reset(); }
    }

    bool NativeWindow::WaitUntilReady(std::uint32_t timeoutMs) noexcept
    {
        if (!m_impl) return false;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (!m_impl->ready.load() && GetTickCount64() < deadline) Sleep(2);
        return m_impl->ready.load() && m_impl->open.load();
    }

    void NativeWindow::Close() noexcept
    {
        if (!m_impl) return;
        m_impl->ready.wait(false);   // creation attempted -- see BootSplashWindow's Impl::ready archaeology
        if (HWND h = m_impl->hwnd.exchange(nullptr))
            PostMessageW(h, WM_CLOSE, 0, 0);
        if (m_impl->thread.joinable()) m_impl->thread.join();
        m_impl->open.store(false);
    }

    void NativeWindow::Wait() noexcept
    {
        if (!m_impl) return;
        if (m_impl->thread.joinable()) m_impl->thread.join();
    }

    bool     NativeWindow::IsOpen() const noexcept      { return m_impl && m_impl->open.load(); }
    bool     NativeWindow::WasEverOpen() const noexcept { return m_impl && m_impl->everOpen.load(); }
    void*    NativeWindow::Hwnd() const noexcept        { return m_impl ? m_impl->hwnd.load() : nullptr; }
    bool     NativeWindow::OnWindowThread() const noexcept { return m_impl && m_impl->threadId.load() == GetCurrentThreadId(); }
    unsigned NativeWindow::Dpi() const noexcept
    {
        const HWND h = static_cast<HWND>(Hwnd());
        return h ? GetDpiForWindow(h) : 96u;
    }
    void NativeWindow::Invalidate() noexcept
    {
        if (HWND h = static_cast<HWND>(Hwnd())) InvalidateRect(h, nullptr, FALSE);
    }
    void NativeWindow::Invalidate(int left, int top, int right, int bottom) noexcept
    {
        if (HWND h = static_cast<HWND>(Hwnd())) { RECT r{ left, top, right, bottom }; InvalidateRect(h, &r, FALSE); }
    }
    void NativeWindow::PostUser(unsigned msg, std::uintptr_t w, std::intptr_t l) noexcept
    {
        if (msg >= 0x100) return;
        if (HWND h = static_cast<HWND>(Hwnd())) PostMessageW(h, kMsgUserBase + msg, static_cast<WPARAM>(w), static_cast<LPARAM>(l));
    }
    void NativeWindow::SetTitle(std::wstring title) noexcept
    {
        HWND h = static_cast<HWND>(Hwnd());
        if (!h) return;
        try
        {
            wchar_t* copy = new wchar_t[title.size() + 1];
            std::wmemcpy(copy, title.c_str(), title.size() + 1);
            if (!PostMessageW(h, kMsgSetTitle, 0, reinterpret_cast<LPARAM>(copy))) delete[] copy;
        }
        catch (...) {}
    }
#else
    struct NativeWindow::Impl {};
    NativeWindow::NativeWindow() noexcept : m_impl(nullptr) {}
    NativeWindow::~NativeWindow() = default;
    void NativeWindow::Open(const NativeWindowDesc&, INativeWindowPresenter*) noexcept {}
    bool NativeWindow::WaitUntilReady(std::uint32_t) noexcept { return false; }
    void NativeWindow::Close() noexcept {}
    void NativeWindow::Wait() noexcept {}
    bool NativeWindow::IsOpen() const noexcept { return false; }
    bool NativeWindow::WasEverOpen() const noexcept { return false; }
    void* NativeWindow::Hwnd() const noexcept { return nullptr; }
    unsigned NativeWindow::Dpi() const noexcept { return 96u; }
    bool NativeWindow::OnWindowThread() const noexcept { return false; }
    void NativeWindow::Invalidate() noexcept {}
    void NativeWindow::Invalidate(int, int, int, int) noexcept {}
    void NativeWindow::PostUser(unsigned, std::uintptr_t, std::intptr_t) noexcept {}
    void NativeWindow::SetTitle(std::wstring) noexcept {}
#endif
}
