#pragma once
#include <Arcane/Core/Api.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace Arcane
{
    struct NativeWindowDesc
    {
        std::wstring  className = L"ArcaneNativeWindow";   // registered once per process per name
        std::wstring  title     = L"Arcane";
        int           width     = 480;
        int           height    = 270;
        bool          popup     = true;      // WS_POPUP (splash); false = WS_OVERLAPPEDWINDOW (reporter)
        bool          topmost   = false;     // WS_EX_TOPMOST
        bool          appWindow = true;      // WS_EX_APPWINDOW: taskbar button + Alt-Tab (else WS_EX_TOOLWINDOW)
        bool          foreground = false;    // SetForegroundWindow after ShowWindow -- the reporter's window may otherwise
                                             // open BEHIND the dead host's (UE's HACK_ForceToFront, WindowsWindow.cpp:654-657;
                                             // the spawning host must AllowSetForegroundWindow us first, see SpawnReporter)
        std::uint32_t backgroundRgb = 0x0D0D0F;   // 0xRRGGBB class brush
    };

    // Called on the WINDOW THREAD only. Every default is a no-op so a presenter
    // implements exactly what it draws.
    class ARCANE_CORE_API INativeWindowPresenter
    {
    public:
        virtual ~INativeWindowPresenter() = default;
        virtual void OnCreate(void* hwnd) { (void)hwnd; }            // create child controls; runs BEFORE the first paint
        virtual void OnPaint(void* hdc, int left, int top, int right, int bottom) { (void)hdc; (void)left; (void)top; (void)right; (void)bottom; }
        virtual void OnCommand(int id) { (void)id; }                  // WM_COMMAND's LOWORD(wParam)
        virtual void OnSize(int width, int height) { (void)width; (void)height; }
        virtual bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t l) { (void)msg; (void)w; (void)l; return false; }   // PostUser's msg (0..255)
        virtual void OnDestroy() {}
    };

    // A plain OS window owned by ITS OWN THREAD: class registration, creation
    // and the blocking GetMessageW loop all run there; every cross-thread call
    // is a posted message; an atomic handle plus an "ever opened" latch make
    // close-after-destroy and close-before-create both safe; Close() waits on
    // the thread. Lifted verbatim from the boot splash (two review rounds of
    // hardening). Windows-only; a no-op elsewhere. NEVER fails the caller: a
    // failed creation leaves IsOpen()/WasEverOpen() false and everything else
    // a silent no-op.
    class ARCANE_CORE_API NativeWindow
    {
    public:
        NativeWindow() noexcept;
        ~NativeWindow();   // Close()
        NativeWindow(const NativeWindow&)            = delete;
        NativeWindow& operator=(const NativeWindow&) = delete;

        // Starts the window thread and returns at once. `presenter` must
        // outlive the window (Close() first, then destroy the presenter).
        // A second Open on an open window is ignored.
        void Open(const NativeWindowDesc& desc, INativeWindowPresenter* presenter) noexcept;

        // Blocks until creation has been ATTEMPTED (succeeded or failed) or
        // the timeout passes; returns IsOpen().
        [[nodiscard]] bool WaitUntilReady(std::uint32_t timeoutMs) noexcept;

        // Posts WM_CLOSE if the window exists, then joins the thread. Never
        // call it FROM the window thread (it joins itself): a presenter that
        // wants to close posts WM_CLOSE to Hwnd() instead.
        void Close() noexcept;
        // Joins without closing: returns when the window is gone (the user
        // closed it, or Close() ran elsewhere).
        void Wait() noexcept;

        [[nodiscard]] bool  IsOpen() const noexcept;
        [[nodiscard]] bool  WasEverOpen() const noexcept;   // monotonic latch, see BootSplashWindow.hpp's contract
        [[nodiscard]] void* Hwnd() const noexcept;          // HWND, or nullptr
        [[nodiscard]] unsigned Dpi() const noexcept;        // GetDpiForWindow; 96 with no window
        [[nodiscard]] bool  OnWindowThread() const noexcept;

        void Invalidate() noexcept;
        void Invalidate(int left, int top, int right, int bottom) noexcept;   // client rect, bErase = FALSE
        void PostUser(unsigned msg, std::uintptr_t w = 0, std::intptr_t l = 0) noexcept;   // -> OnUser(msg, w, l), msg < 256
        void SetTitle(std::wstring title) noexcept;   // marshalled to the window thread

        struct Impl;   // public for the free WndProc in NativeWindow.cpp (same reason as BootSplashWindow::Impl)
    private:
        std::unique_ptr<Impl> m_impl;
    };
}
