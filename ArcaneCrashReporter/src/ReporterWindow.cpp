#include "ReporterWindow.hpp"
#include "WarpImGui.hpp"
#include "Win32Text.hpp"

#include "Widgets/EditorTheme.hpp"

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <algorithm>
#include <filesystem>

static_assert(static_cast<int>(Arcane::Reporter::ReporterButton::OpenFolder) == Arcane::Reporter::ReporterWindow::kBtnOpenFolder);
static_assert(static_cast<int>(Arcane::Reporter::ReporterButton::Terminate)  == Arcane::Reporter::ReporterWindow::kBtnTerminate);

namespace Arcane::Reporter
{
    namespace
    {
        HFONT MessageFont(unsigned dpi)
        {
            NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
            return CreateFontIndirectW(&ncm.lfMessageFont);
        }
        HFONT MonoFont(unsigned dpi)
        {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(10, static_cast<int>(dpi), 72);
            lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
            wcscpy_s(lf.lfFaceName, L"Consolas");
            return CreateFontIndirectW(&lf);
        }
        HWND Child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font, DWORD exStyle = 0)
        {
            HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            if (h && font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return h;
        }
        // Multi-line EDIT controls want CRLF.
        std::wstring Crlf(std::string_view utf8)
        {
            std::wstring w = ToWide(utf8), out;
            out.reserve(w.size() + w.size() / 16);
            for (wchar_t c : w) { if (c == L'\n') out += L'\r'; out += c; }
            return out;
        }

        constexpr wchar_t kStyledProp[] = L"Arcane.ReporterWindow";

        bool IsStyledInput(UINT msg)
        {
            switch (msg)
            {
            case WM_MOUSEMOVE: case WM_MOUSELEAVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
            case WM_SETFOCUS: case WM_KILLFOCUS:
                return true;
            default:
                return false;
            }
        }

        // Real Win32 proc. Forwards into the friend so the class stays free of
        // windows.h in its header. x64 has one calling convention, so the
        // pointer-sized arguments match ReporterStyledProc's.
        LRESULT CALLBACK StyledWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            return static_cast<LRESULT>(ReporterStyledProc(hwnd, msg,
                                                           static_cast<std::uintptr_t>(wParam),
                                                           static_cast<std::intptr_t>(lParam)));
        }
    }

    std::intptr_t __stdcall ReporterStyledProc(void* hwnd, unsigned msg, std::uintptr_t wParam, std::intptr_t lParam)
    {
        HWND h = static_cast<HWND>(hwnd);
        auto* self = static_cast<ReporterWindow*>(GetPropW(h, kStyledProp));
        if (!self || !self->m_prevProc)
            return static_cast<std::intptr_t>(DefWindowProcW(h, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)));
        auto prev = reinterpret_cast<WNDPROC>(self->m_prevProc);
        if (!self->m_styled)
            return static_cast<std::intptr_t>(CallWindowProcW(prev, h, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)));

        // Gaining focus invalidates the window and DefWindowProc erases it
        // with the class brush, which is the light grey the Win32 fallback
        // needs. Swallowing the erase leaves the swap chain's last frame up.
        if (msg == WM_ERASEBKGND)
            return 1;

        const LRESULT eaten = ImGui_ImplWin32_WndProcHandler(h, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
        // WM_SIZE is not in this list: NativeWindow turns it into OnSize,
        // and OnSize is what rebuilds the bitmap. Framing here too would
        // draw once against the old surface and once against the new.
        if (IsStyledInput(msg)) self->Frame();
        if (eaten) return static_cast<std::intptr_t>(eaten);
        return static_cast<std::intptr_t>(CallWindowProcW(prev, h, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)));
    }

    ReporterWindow::ReporterWindow(ReportView initial, std::function<void(int)> onCommand)
        : m_view(std::move(initial)), m_onCommand(std::move(onCommand)) {}

    void ReporterWindow::Show(NativeWindow& window, const std::string& productForTitle)
    {
        m_window = &window;
        NativeWindowDesc d;
        d.className = L"ArcaneCrashReporter";
        d.title     = ToWide(m_view.title.empty() ? productForTitle : m_view.title);
        // R92 (task 8): 1000, not 900. The hang view shows all six buttons,
        // and at 96 DPI they need 2*12 + 6*150 + 5*8 = 964 px of client
        // area; a 900 px window has about 884, which clipped "Terminate and
        // Collect" off the right edge. Layout also shrinks the row to fit
        // (below), so this is the width at which nothing HAS to shrink.
        d.width = 1000; d.height = 640;
        d.popup = false; d.topmost = false; d.appWindow = true;
        d.foreground = true;   // UE's CRC forces itself to front (CrashReportClientApp.cpp:425-427)
        d.backgroundRgb = 0xF0F0F0;   // the system button face: plain Win32 controls draw on it
        d.dialogNavigation = true;    // R83: Tab between controls, Enter/Esc close via the default button / IDCANCEL
        window.Open(d, this);
        (void)window.WaitUntilReady(5000);
    }

    void ReporterWindow::SetView(ReportView v)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_crashView) ApplyCrashViewLocked(v);   // R35: the latch survives a later view
            m_view = std::move(v);
            m_symbolizing = false;
        }
        if (m_window) m_window->PostUser(kUserViewChanged);
    }

    void ReporterWindow::ApplyCrashViewLocked(ReportView& v) const
    {
        v.isHang = false;
        v.canRelaunch = !v.relaunchLine.empty();
        v.headline += m_crashSuffix;
        v.title += m_crashSuffix;
    }

    void ReporterWindow::BecomeCrashView(const std::string& headlineSuffix)
    {
        // The title is read back out under the SAME lock that just changed
        // it, rather than re-touching m_view once released (R84's don't-race
        // m_view principle applies here too, not only to ApplyView/
        // CurrentDetails): another thread's SetView could otherwise land
        // between the unlock and the read below.
        std::string title;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_crashView) return;   // R35: the first call wins
            m_crashView   = true;
            m_crashSuffix = headlineSuffix;
            ApplyCrashViewLocked(m_view);
            title = m_view.title;
        }
        if (m_window) { m_window->SetTitle(ToWide(title)); m_window->PostUser(kUserViewChanged); }
    }

    std::string ReporterWindow::CurrentDetails()
    {
        // The styled path has no Win32 combo. m_threadIndex is written only
        // on this thread (BuildUi), which is also the thread OnButton calls
        // this from, so reading it next to the lock is the same rule as the
        // ComboBox_GetCurSel below: never hold m_mutex across a callback
        // that might want it.
        if (m_styled)
        {
            const int sel = m_threadIndex;
            std::lock_guard<std::mutex> lk(m_mutex);
            const std::size_t index = sel < 0 ? 0 : static_cast<std::size_t>(sel);
            return DetailsText(m_view, index < m_view.threads.size() ? index : 0);
        }
        // R84: the combo's current selection is read with a SendMessage
        // (ComboBox_GetCurSel) BEFORE m_mutex is taken, never across it --
        // holding the lock into a SendMessage that lands back on this same
        // window's thread (the common case: this runs off the window
        // thread's own OnCommand -> m_onCommand -> here) risks a self-deadlock
        // the moment a child control's synchronous notification tries to
        // reach m_mutex too.
        const int sel = m_combo ? ComboBox_GetCurSel(static_cast<HWND>(m_combo)) : -1;
        std::lock_guard<std::mutex> lk(m_mutex);
        return DetailsText(m_view, sel < 0 ? 0 : static_cast<std::size_t>(sel));
    }

    std::string ReporterWindow::ReportFolder()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_view.reportFolder;
    }

    std::string ReporterWindow::RelaunchLine()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_view.relaunchLine;
    }

    void ReporterWindow::OnCreate(void* hwnd)
    {
        m_hwnd = hwnd;
        if (TryStyled(hwnd)) return;
        HWND h = static_cast<HWND>(hwnd);
        m_fontDpi = GetDpiForWindow(h);
        m_uiFont   = MessageFont(m_fontDpi);
        m_monoFont = MonoFont(m_fontDpi);
        HFONT ui = static_cast<HFONT>(m_uiFont), mono = static_cast<HFONT>(m_monoFont);
        m_header  = Child(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kHeader, ui);
        m_when    = Child(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kWhen, ui);
        m_reason  = Child(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kReason, ui);
        // R83: WS_TABSTOP on every control dialogNavigation is meant to reach
        // -- the combo, the details edit, and every button below. The static
        // labels above stay plain text: nothing to tab TO there.
        m_combo   = Child(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kThreadCombo, ui);
        m_details = Child(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
                          kDetails, mono, WS_EX_CLIENTEDGE);
        const wchar_t* labels[6] = { L"Open Report Folder", L"Copy Details", L"Close", L"Relaunch", L"Keep Waiting", L"Terminate and Collect" };
        for (int i = 0; i < 6; ++i)
        {
            // Close (index 2, kBtnClose) is the DEFAULT push button (R83): it
            // is what Enter invokes via IsDialogMessageW when no other button
            // has focus, and it is always visible (only Relaunch/Keep
            // Waiting/Terminate are ever hidden below), so a default button
            // always exists to receive it.
            const DWORD style = (kBtnOpenFolder + i == kBtnClose) ? (BS_DEFPUSHBUTTON | WS_TABSTOP) : (BS_PUSHBUTTON | WS_TABSTOP);
            m_buttons[i] = Child(h, L"BUTTON", labels[i], style, kBtnOpenFolder + i, ui);
        }
        ApplyView();
        // Fix round 1 (R91 minor 6): explicit initial focus. Without it Tab
        // starts from nothing, and while Enter still reaches Close correctly
        // (IsDialogMessageW's BS_DEFPUSHBUTTON path, not DM_GETDEFID -- this
        // is not a real dialog template), a visible focus rect on the
        // default button is the keyboard-navigation cue a user expects.
        SetFocus(static_cast<HWND>(m_buttons[kBtnClose - kBtnOpenFolder]));
    }

    void ReporterWindow::ApplyView()
    {
        if (m_styled)
        {
            m_detailsDirty = true;
            Frame();
            return;
        }
        // R84: copy m_view under the lock, then touch every control OUTSIDE
        // it. Child controls (the combo above all) send SYNCHRONOUS
        // WM_COMMANDs back to this window as a side effect of
        // ComboBox_SetCurSel/ResetContent -- OnCommand runs those through
        // RefreshDetails(), which takes m_mutex, so holding it here would
        // self-deadlock the first time that fires.
        ReportView v; bool symbolizing;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            v = m_view;
            symbolizing = m_symbolizing;
        }
        SetWindowTextW(static_cast<HWND>(m_header), ToWide(v.headline).c_str());
        SetWindowTextW(static_cast<HWND>(m_when),   ToWide(v.whenLine).c_str());
        SetWindowTextW(static_cast<HWND>(m_reason), ToWide(v.reasonText).c_str());
        HWND combo = static_cast<HWND>(m_combo);
        const int prev = ComboBox_GetCurSel(combo);
        ComboBox_ResetContent(combo);
        for (const ThreadView& t : v.threads) ComboBox_AddString(combo, ToWide(t.label).c_str());
        ComboBox_SetCurSel(combo, v.threads.empty() ? -1 : (prev >= 0 && prev < static_cast<int>(v.threads.size()) ? prev : 0));
        ShowWindow(combo, v.threads.size() > 1 ? SW_SHOW : SW_HIDE);
        const int cur = ComboBox_GetCurSel(combo);
        const std::size_t sel = cur < 0 ? 0 : static_cast<std::size_t>(cur);
        const std::string body = symbolizing ? "Symbolizing...\n\n" + DetailsBody(v, sel) : DetailsBody(v, sel);
        SetWindowTextW(static_cast<HWND>(m_details), Crlf(body).c_str());
        // Buttons: [0] folder [1] copy [2] close [3] relaunch [4] keep waiting [5] terminate
        ShowWindow(static_cast<HWND>(m_buttons[4]), v.isHang ? SW_SHOW : SW_HIDE);
        ShowWindow(static_cast<HWND>(m_buttons[5]), v.isHang ? SW_SHOW : SW_HIDE);
        ShowWindow(static_cast<HWND>(m_buttons[3]), v.relaunchLine.empty() ? SW_HIDE : SW_SHOW);
        EnableWindow(static_cast<HWND>(m_buttons[3]), v.canRelaunch ? TRUE : FALSE);
        SetWindowTextW(static_cast<HWND>(m_buttons[kBtnCopy - kBtnOpenFolder]), L"Copy Details");   // reverts NoteCopy
        m_order = VisibleButtons(v);
        RECT rc{}; GetClientRect(static_cast<HWND>(m_hwnd), &rc);
        Layout(rc.right, rc.bottom);
    }

    // R84 (plan defect): the combo's WM_COMMAND fires on every notification
    // code (CBN_DROPDOWN, CBN_SETFOCUS, ... -- NativeWindow's WM_COMMAND
    // handler drops HIWORD so OnCommand cannot tell them apart), and
    // ApplyView's ComboBox_ResetContent would empty the list out from under
    // an open dropdown. This re-renders only the details text for whatever
    // is currently selected; the combo's own content is untouched. Idempotent
    // under any notification code, which is what makes "just handle them all
    // the same way" safe here.
    void ReporterWindow::RefreshDetails()
    {
        const int cur = m_combo ? ComboBox_GetCurSel(static_cast<HWND>(m_combo)) : -1;
        const std::size_t sel = cur < 0 ? 0 : static_cast<std::size_t>(cur);
        ReportView v; bool symbolizing;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            v = m_view;
            symbolizing = m_symbolizing;
        }
        const std::string body = symbolizing ? "Symbolizing...\n\n" + DetailsBody(v, sel) : DetailsBody(v, sel);
        SetWindowTextW(static_cast<HWND>(m_details), Crlf(body).c_str());
        SetWindowTextW(static_cast<HWND>(m_buttons[kBtnCopy - kBtnOpenFolder]), L"Copy Details");   // reverts NoteCopy
    }

    // R85: NativeWindow's WM_DPICHANGED only resizes (SetWindowPos to the
    // suggested rect); the WM_SIZE that follows is the only DPI signal a
    // presenter gets. Compare against the DPI the fonts were built at and
    // rebuild both, re-WM_SETFONT every child BEFORE deleting the old
    // fonts -- a control must never be left pointing at a freed HFONT, even
    // for the instant between SetFont calls.
    void ReporterWindow::RebuildFonts(unsigned dpi)
    {
        HFONT newUi   = MessageFont(dpi);
        HFONT newMono = MonoFont(dpi);
        HFONT oldUi   = static_cast<HFONT>(m_uiFont);
        HFONT oldMono = static_cast<HFONT>(m_monoFont);
        auto setFont = [](void* ctrl, HFONT f)
        {
            if (ctrl) SendMessageW(static_cast<HWND>(ctrl), WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        };
        setFont(m_header, newUi); setFont(m_when, newUi); setFont(m_reason, newUi); setFont(m_combo, newUi);
        for (void* b : m_buttons) setFont(b, newUi);
        setFont(m_details, newMono);
        m_uiFont = newUi; m_monoFont = newMono;
        if (oldUi)   DeleteObject(oldUi);
        if (oldMono) DeleteObject(oldMono);
        m_fontDpi = dpi;
    }

    void ReporterWindow::Layout(int w, int h)
    {
        // Minimized: WM_SIZE (and GetClientRect, from ApplyView) report 0x0.
        // R92's shrink-to-fit would lay every button out at zero width, and a
        // zero-width button cannot be clicked even programmatically --
        // BM_CLICK's synthesized point falls outside it; the task 8 desk proof
        // hit exactly that. Keep the last real layout; the restore brings a
        // real size and lays out again.
        if (w <= 0 || h <= 0) return;
        const int dpi = static_cast<int>(GetDpiForWindow(static_cast<HWND>(m_hwnd)));
        auto px = [&](int v) { return MulDiv(v, dpi, 96); };
        const int m = px(12), line = px(20), btnH = px(28), gap = px(8);
        int y = m;
        MoveWindow(static_cast<HWND>(m_header), m, y, w - 2 * m, line, TRUE); y += line + px(2);
        MoveWindow(static_cast<HWND>(m_when),   m, y, w - 2 * m, line, TRUE); y += line + px(2);
        MoveWindow(static_cast<HWND>(m_reason), m, y, w - 2 * m, line, TRUE); y += line + px(6);
        MoveWindow(static_cast<HWND>(m_combo),  m, y, px(320), px(200), TRUE);
        if (IsWindowVisible(static_cast<HWND>(m_combo))) y += px(26) + px(4);
        const int detailsBottom = h - m - btnH - px(8);
        MoveWindow(static_cast<HWND>(m_details), m, y, w - 2 * m, detailsBottom - y, TRUE);
        // R92 (task 8): the button row never runs past the client edge. The
        // preferred width is 150 px; when the visible buttons do not fit, it
        // shrinks to share what there is, down to a 72 px floor (about the
        // shortest label, "Close", with padding). Below the floor -- a window
        // the user dragged narrower than any sensible row -- the edge still
        // wins: the last buttons are clipped to end AT the edge rather than
        // drawn past it.
        // R92 shrink-to-fit, now RIGHT-aligned in VisibleButtons order (node-page
        // phase s8.1): Close is the rightmost; the edge still wins below the floor.
        const int visible = static_cast<int>(m_order.size());
        const int room = w - 2 * m - (visible > 1 ? (visible - 1) * gap : 0);
        int btnW = px(150);
        if (visible > 0 && visible * btnW > room) btnW = (std::max)(room / visible, px(72));
        const int total = visible * btnW + (visible > 1 ? (visible - 1) * gap : 0);
        const int right = w - m;
        int x = (std::max)(m, right - total);
        for (const ReporterButton b : m_order)
        {
            HWND btn = static_cast<HWND>(m_buttons[static_cast<int>(b) - kBtnOpenFolder]);
            const int bw = (std::max)(0, (std::min)(btnW, right - x));
            MoveWindow(btn, x, h - m - btnH, bw, btnH, TRUE);
            x += btnW + gap;
        }
    }

    void ReporterWindow::OnPaint(void*, int, int, int, int)
    {
        // The styled path presents from the swap chain in Frame(). BeginPaint
        // (NativeWindow) is what validates the region; painting here would
        // only race that present.
    }

    void ReporterWindow::OnSize(int w, int h)
    {
        if (!m_hwnd) return;
        const unsigned dpi = GetDpiForWindow(static_cast<HWND>(m_hwnd));
        if (m_styled)
        {
            // Same signal the Win32 path uses (R85): WM_DPICHANGED only
            // resizes, and the WM_SIZE after it is the DPI signal we get.
            if (dpi && dpi != m_fontDpi)
            {
                m_fontDpi = dpi;
                ApplyScaledTheme(static_cast<float>(dpi) / 96.0f);
            }
            if (w > 0 && h > 0 && !WarpResize(m_warp, w, h)) return;
            Frame();
            return;
        }
        if (dpi != m_fontDpi) RebuildFonts(dpi);   // R85
        Layout(w, h);
    }

    void ReporterWindow::OnCommand(int id)
    {
        if (id == kThreadCombo) { RefreshDetails(); return; }   // R84: any notification -- refresh the selection only
        // R83: IsDialogMessageW synthesizes IDCANCEL on Esc, and IDOK on
        // Enter when no push button has focus; Close is the only
        // BS_DEFPUSHBUTTON, so Enter ordinarily reaches it directly as
        // kBtnClose via BN_CLICKED (NativeWindow's WM_COMMAND handler drops
        // HIWORD, so that arrives here as plain kBtnClose already). These two
        // map the same way explicitly, in case that assumption ever stops
        // holding -- NEVER to Relaunch, which is the one action Esc/Enter
        // must not be able to trigger by accident.
        if (id == IDCANCEL || id == IDOK) { if (m_onCommand) m_onCommand(kBtnClose); return; }
        if (id >= kBtnOpenFolder && id <= kBtnTerminate && m_onCommand) m_onCommand(id);
    }

    // Task 8: the host events arrive here as posted messages -- from the
    // reporter's waiter thread (exited / recovered) or its main thread (the
    // start-up identity check) -- and go out through the same m_onCommand the
    // buttons use, so ReporterMain decides every hang outcome in one place,
    // on this thread. A message posted after the window died is simply never
    // delivered (NativeWindow::PostUser posts to a null or dead HWND), which
    // is what makes a late host event harmless.
    bool ReporterWindow::OnUser(unsigned msg, std::uintptr_t w, std::intptr_t)
    {
        if (msg == kUserViewChanged) { ApplyView(); return true; }   // full refresh, incl. ComboBox_ResetContent (R84)
        if (msg == kUserHostExited)
        {
            m_hostExitCode.store(static_cast<std::uint32_t>(w));
            if (m_onCommand) m_onCommand(kHostExited);
            return true;
        }
        if (msg == kUserHostRecovered) { if (m_onCommand) m_onCommand(kHostRecovered); return true; }
        if (msg == kUserHostMismatch)  { if (m_onCommand) m_onCommand(kHostMismatch);  return true; }
        return false;
    }

    void ReporterWindow::OnDestroy()
    {
        HWND h = static_cast<HWND>(m_hwnd);
        if (h && m_prevProc)
        {
            SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_prevProc));
            m_prevProc = nullptr;
            RemovePropW(h, kStyledProp);
        }
        if (m_styled || m_imgui) ShutdownStyled();
        if (m_uiFont)   DeleteObject(static_cast<HFONT>(m_uiFont));
        if (m_monoFont) DeleteObject(static_cast<HFONT>(m_monoFont));
        m_uiFont = m_monoFont = nullptr;
        m_fontDpi = 0;
        m_hwnd = nullptr;
    }

    bool ReporterWindow::TryStyled(void* hwnd)
    {
        HWND h = static_cast<HWND>(hwnd);
        IMGUI_CHECKVERSION();
        m_imgui = ImGui::CreateContext();
        if (!m_imgui) return false;
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui));

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;    // do not drop an imgui.ini beside the host
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        m_fontDpi = GetDpiForWindow(h);
        if (!m_fontDpi) m_fontDpi = 96;
        ApplyScaledTheme(static_cast<float>(m_fontDpi) / 96.0f);
        LoadFaces();

        if (!ImGui_ImplWin32_Init(h)) { ShutdownStyled(); return false; }

        SetPropW(h, kStyledProp, this);
        m_prevProc = reinterpret_cast<void*>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&StyledWndProc)));
        if (!m_prevProc)
        {
            RemovePropW(h, kStyledProp);
            ShutdownStyled();
            return false;
        }

        m_warp = WarpCreate(h);
        if (!m_warp)
        {
            SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_prevProc));
            m_prevProc = nullptr;
            RemovePropW(h, kStyledProp);
            ShutdownStyled();
            return false;
        }

        // Dialog navigation would swallow Tab/Enter/Esc before our proc saw
        // them, and this path has no WS_TABSTOP children for it to walk.
        // The fallback leaves the flag Show() set (true).
        m_styled = true;
        if (m_window) m_window->SetDialogNavigation(false);
        m_detailsDirty = true;
        Frame();
        return true;
    }

    void ReporterWindow::ShutdownStyled()
    {
        m_styled = false;
        if (m_imgui)
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui));
            // DX11 shutdown uses the context, so it has to run before DestroyContext.
            if (m_warp) { WarpDestroy(m_warp); m_warp = nullptr; }
            if (ImGui::GetIO().BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(static_cast<ImGuiContext*>(m_imgui));
            m_imgui = nullptr;
        }
        else if (m_warp)
        {
            WarpDestroy(m_warp);
            m_warp = nullptr;
        }
        m_uiFace = m_monoFace = nullptr;
    }

    void ReporterWindow::ApplyScaledTheme(float dpiScale)
    {
        if (dpiScale < 1.0f) dpiScale = 1.0f;
        ImGuiStyle fresh;
        Arcane::Editor::ApplyEditorTheme(fresh, Arcane::Editor::EditorUiStyleSettings{});   // no editor registry here: the defaults
        if (dpiScale != 1.0f) fresh.ScaleAllSizes(dpiScale);
        fresh.FontSizeBase = 16.0f;     // the editor's UI size (InstallEditorFonts)
        fresh.FontScaleDpi = dpiScale;  // sizes above were scaled; text follows
        ImGui::GetStyle() = fresh;
    }

    void ReporterWindow::LoadFaces()
    {
        ImGuiIO& io = ImGui::GetIO();
        wchar_t exe[MAX_PATH]{};
        std::filesystem::path dir;
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0)
            dir = std::filesystem::path(exe).parent_path();
        const std::filesystem::path inter = dir / "data" / "font" / "inter" / "static" / "Inter_18pt-Regular.ttf";
        wchar_t windir[MAX_PATH]{};
        std::filesystem::path consolas;
        if (GetWindowsDirectoryW(windir, MAX_PATH) != 0)
            consolas = std::filesystem::path(windir) / "Fonts" / "consola.ttf";

        // Inter first, so it becomes Fonts[0] -- the same default the editor
        // installs. A missing file (this exe launched from its own bin dir,
        // not staged beside a host) falls back to the embedded font; the
        // theme colors still hold.
        ImFont* ui = io.Fonts->AddFontFromFileTTF(inter.string().c_str(), 16.0f);
        if (!ui) ui = io.Fonts->AddFontDefault();
        m_uiFace = ui;
        ImFont* mono = consolas.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(consolas.string().c_str(), 15.0f);
        m_monoFace = mono ? mono : ui;
    }

    void ReporterWindow::RebuildDetails(const ReportView& v, bool symbolizing)
    {
        if (m_threadIndex < 0 || m_threadIndex >= static_cast<int>(v.threads.size()))
            m_threadIndex = 0;
        const std::size_t sel = m_threadIndex < 0 ? 0 : static_cast<std::size_t>(m_threadIndex);
        std::string body = DetailsBody(v, sel);
        if (symbolizing) body.insert(0, "Symbolizing...\n\n");
        m_detailsBuf = std::move(body);
        m_detailsDirty = false;
    }

    int ReporterWindow::BuildUi(const ReportView& v, bool symbolizing)
    {
        if (m_detailsDirty) RebuildDetails(v, symbolizing);

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        int cmd = 0;
        if (ImGui::Begin("##report", nullptr, flags))
        {
            ImGui::PushFont(static_cast<ImFont*>(m_uiFace), 18.0f);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(v.headline.empty() ? "Arcane" : v.headline.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            if (!v.whenLine.empty()) ImGui::TextDisabled("%s", v.whenLine.c_str());
            if (!v.reasonText.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(v.reasonText.c_str());
                ImGui::PopTextWrapPos();
            }

            if (v.threads.size() > 1)
            {
                const float width = 320.0f * ImGui::GetStyle().FontScaleDpi;
                ImGui::SetNextItemWidth(width);
                const char* preview = (m_threadIndex >= 0 && m_threadIndex < static_cast<int>(v.threads.size()))
                    ? v.threads[static_cast<std::size_t>(m_threadIndex)].label.c_str() : "";
                if (ImGui::BeginCombo("##thread", preview))
                {
                    for (int i = 0; i < static_cast<int>(v.threads.size()); ++i)
                    {
                        ImGui::PushID(i);
                        const bool selected = i == m_threadIndex;
                        if (ImGui::Selectable(v.threads[static_cast<std::size_t>(i)].label.c_str(), selected))
                        {
                            m_threadIndex = i;
                            m_detailsDirty = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
            if (m_detailsDirty) RebuildDetails(v, symbolizing);

            const float footer = ImGui::GetFrameHeightWithSpacing();
            ImGui::PushFont(static_cast<ImFont*>(m_monoFace), 15.0f);
            // Read-only, so the buffer is not written. size()+1 counts the
            // trailing NUL std::string already stores (C++11).
            char empty = '\0';
            char* text = m_detailsBuf.empty() ? &empty : m_detailsBuf.data();
            const std::size_t textSize = m_detailsBuf.empty() ? 1 : m_detailsBuf.size() + 1;
            ImGui::InputTextMultiline("##details", text, textSize, ImVec2(-1.0f, -footer),
                                      ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
            ImGui::PopFont();

            const CopyState copyState = ImGui::GetTime() < m_copyUntil ? m_copyState : CopyState::Idle;
            const ImGuiStyle& st = ImGui::GetStyle();
            const auto labelFor = [&](ReporterButton b) -> std::string
            {
                switch (b)
                {
                    case ReporterButton::OpenFolder:  return "Open Report Folder";
                    case ReporterButton::Copy:        return std::string(CopyButtonLabel(copyState)) + "###copy";
                    case ReporterButton::Close:       return "Close";
                    case ReporterButton::Relaunch:    return "Relaunch";
                    case ReporterButton::KeepWaiting: return "Keep Waiting";
                    case ReporterButton::Terminate:   return "Terminate and Collect";
                }
                return "?";
            };
            float copyW = 0.0f;   // fixed: the widest of the three labels, so the row never shifts mid-flash
            for (const CopyState s : { CopyState::Idle, CopyState::Copied, CopyState::Failed })
                copyW = (std::max)(copyW, ImGui::CalcTextSize(std::string(CopyButtonLabel(s)).c_str()).x);
            copyW += st.FramePadding.x * 2.0f;
            const auto widthFor = [&](ReporterButton b)
            {
                return b == ReporterButton::Copy ? copyW
                    : ImGui::CalcTextSize(labelFor(b).c_str(), nullptr, true).x + st.FramePadding.x * 2.0f;
            };
            const std::vector<ReporterButton> order = VisibleButtons(v);
            float total = st.ItemSpacing.x * static_cast<float>(order.size() > 0 ? order.size() - 1 : 0);
            for (const ReporterButton b : order) total += widthFor(b);
            const float startX = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX((std::max)(startX, startX + ImGui::GetContentRegionAvail().x - total));
            bool placed = false;
            for (const ReporterButton b : order)
            {
                if (placed) ImGui::SameLine();
                placed = true;
                const int id = static_cast<int>(b);
                const bool enabled = b != ReporterButton::Relaunch || v.canRelaunch;
                const bool primary = b == ReporterButton::Close;   // accent-filled (drafting pick, 9.28)
                if (!enabled) ImGui::BeginDisabled();
                if (primary)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, Arcane::Editor::Theme::kAccent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Arcane::Editor::Theme::kAccentHovered);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Arcane::Editor::Theme::kAccentActive);
                }
                if (primary && m_focusClose) ImGui::SetKeyboardFocusHere();
                if (ImGui::Button(labelFor(b).c_str(), ImVec2(widthFor(b), 0.0f)) && cmd == 0) cmd = id;
                if (primary) ImGui::PopStyleColor(3);
                if (!enabled) ImGui::EndDisabled();
            }
            m_focusClose = false;

            const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
            if (cmd == 0 && !popup && ImGui::IsKeyPressed(ImGuiKey_Escape))
                cmd = kBtnClose;
            // Enter closes, like the Win32 default button, unless a button
            // already took it or the text well is active. Never Relaunch:
            // that is the one action Enter must not fire by accident (R83).
            const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
            if (cmd == 0 && !popup && enter && !ImGui::IsAnyItemActive())
                cmd = kBtnClose;
        }
        ImGui::End();
        return cmd;
    }

    void ReporterWindow::NoteCopy(bool ok)
    {
        const CopyState state = ok ? CopyState::Copied : CopyState::Failed;
        if (m_styled)
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui));
            m_copyState = state;
            m_copyUntil = ImGui::GetTime() + 0.75;
            if (m_window) m_window->PostUser(kUserViewChanged);   // repaint now; no timer
            return;
        }
        if (HWND copy = static_cast<HWND>(m_buttons[kBtnCopy - kBtnOpenFolder]))
            SetWindowTextW(copy, ToWide(std::string(CopyButtonLabel(state))).c_str());
    }

    void ReporterWindow::Frame()
    {
        if (!m_styled || m_inFrame || !m_imgui || !m_hwnd) return;
        HWND h = static_cast<HWND>(m_hwnd);
        RECT rc{};
        GetClientRect(h, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return;
        if (!WarpResize(m_warp, rc.right, rc.bottom)) return;

        m_inFrame = true;
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_imgui));
        WarpNewFrame(m_warp);
        ImGui_ImplWin32_NewFrame();

        ReportView v;
        bool symbolizing = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            v = m_view;
            symbolizing = m_symbolizing;
        }
        if (m_detailsDirty) RebuildDetails(v, symbolizing);

        ImGui::NewFrame();
        const int cmd = BuildUi(v, symbolizing);
        ImGui::Render();
        WarpDraw(m_warp, ImGui::GetDrawData());
        m_inFrame = false;
        if (cmd && m_onCommand) m_onCommand(cmd);
    }
}
