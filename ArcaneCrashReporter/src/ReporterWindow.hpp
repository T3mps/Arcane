// The crash window's presenter (crash window plan 2, task 7; spec §6
// "Window"). Plain Win32 controls on Arcane::NativeWindow -- no toolkit.
//
// This is NOT one of the pure files: unlike ReporterArgs/ReportView/
// SymbolizedText, ReporterWindow.hpp/.cpp are never source-compiled into
// ArcaneTests (premake5.lua's ArcaneTests `files` list names ReportView.cpp
// only), so pulling in NativeWindow.hpp and, from the .cpp, windows.h is
// fine here. Everything this presenter DRAWS is still Task 6's pure
// ReportView/DetailsText -- this file only pushes that content into
// controls and reports which button fired.
#pragma once
#include "ReportView.hpp"
#include <Arcane/Platform/NativeWindow.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace Arcane::Reporter
{
    class ReporterWindow final : public INativeWindowPresenter
    {
    public:
        enum Command : int
        {
            kBtnOpenFolder = 100, kBtnCopy = 101, kBtnClose = 102, kBtnRelaunch = 103,
            kBtnKeepWaiting = 104, kBtnTerminate = 105,
            kThreadCombo = 200, kDetails = 300, kHeader = 301, kWhen = 302, kReason = 303,
        };
        enum User : unsigned { kUserViewChanged = 1, kUserHostRecovered = 2, kUserHostExited = 3, kUserHostMismatch = 4 };
        // Task 8: the hang protocol's HOST events, delivered through the SAME
        // onCommand callback the buttons use (negative so they can never
        // collide with a control id), so every hang decision -- a click or a
        // host event -- is taken on the window thread by one function. Posted
        // by the reporter's waiter thread / main thread as the kUserHost*
        // messages above; OnUser turns them into these.
        enum HostEvent : int { kHostExited = -1, kHostRecovered = -2, kHostMismatch = -3 };

        // `onCommand` runs ON THE WINDOW THREAD with a Command id. The reporter's
        // main decides what a button means (relaunch, terminate); the presenter
        // only draws and reports.
        ReporterWindow(ReportView initial, std::function<void(int)> onCommand);

        // Open the window on its own thread and wait for it. The initial view
        // is drawn with "Symbolizing..." in the details until SetView arrives.
        void Show(NativeWindow& window, const std::string& productForTitle);
        // From ANY thread: replace the view and repaint (posts kUserViewChanged).
        void SetView(ReportView v);
        // From any thread: the hang was terminated / the host exited -- the
        // window becomes the crash view (hang buttons hidden, Relaunch enabled
        // when a line exists, title updated).
        //
        // R35: LATCHES. The first call wins and every later one is a no-op --
        // Terminate and Collect is followed by the waiter's own "host exited
        // (11)", and a suffix appended twice would read "-- terminated and
        // collected -- the host exited (11)". The latch also survives a later
        // SetView: symbolization may finish AFTER the user terminated, and the
        // finished view must arrive already turned into the crash view rather
        // than bring the hang buttons back.
        void BecomeCrashView(const std::string& headlineSuffix);

        // The exit code the waiter delivered with kUserHostExited; read by the
        // onCommand callback for kHostExited, on the window thread.
        [[nodiscard]] std::uint32_t LastHostExitCode() const noexcept { return m_hostExitCode.load(); }

        [[nodiscard]] std::string CurrentDetails();   // DetailsText of the selected thread (Copy Details)
        // R40: the folder and the relaunch line, read from the CURRENT view
        // (SetView may have replaced the initial one) under m_mutex -- the
        // button handler in ReporterMain.cpp has no other way at m_view.
        [[nodiscard]] std::string ReportFolder();      // m_view.reportFolder
        [[nodiscard]] std::string RelaunchLine();      // m_view.relaunchLine

    private:
        void OnCreate(void* hwnd) override;
        void OnSize(int w, int h) override;
        void OnCommand(int id) override;
        bool OnUser(unsigned msg, std::uintptr_t w, std::intptr_t l) override;
        void OnDestroy() override;
        void ApplyView();          // window thread: pushes m_view into every control, incl. the combo's list (R84)
        void RefreshDetails();     // window thread: re-renders ONLY the details edit for the current combo selection (R84)
        void RebuildFonts(unsigned dpi);   // window thread: R85 -- DPI changed since the fonts were built
        void Layout(int w, int h);
        void ApplyCrashViewLocked(ReportView& v) const;   // m_mutex held: the R35 latch's transformation

        NativeWindow*           m_window = nullptr;
        std::mutex              m_mutex;
        ReportView              m_view;
        bool                    m_symbolizing = true;
        std::function<void(int)> m_onCommand;
        void* m_hwnd = nullptr; void* m_header = nullptr; void* m_when = nullptr; void* m_reason = nullptr;
        void* m_combo = nullptr; void* m_details = nullptr; void* m_buttons[6] = {};
        void* m_uiFont = nullptr; void* m_monoFont = nullptr;
        unsigned m_fontDpi = 0;    // R85: the DPI the two fonts above were built at
        bool        m_crashView = false;   // R35: BecomeCrashView has run (guarded by m_mutex)
        std::string m_crashSuffix;         // ...with this suffix (guarded by m_mutex)
        std::atomic<std::uint32_t> m_hostExitCode{0};   // R40: set by OnUser(kUserHostExited)
    };
}
