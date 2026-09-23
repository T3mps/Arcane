#include "ReporterWindow.hpp"
#include "Win32Text.hpp"
#include <windowsx.h>

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
    }

    ReporterWindow::ReporterWindow(ReportView initial, std::function<void(int)> onCommand)
        : m_view(std::move(initial)), m_onCommand(std::move(onCommand)) {}

    void ReporterWindow::Show(NativeWindow& window, const std::string& productForTitle)
    {
        m_window = &window;
        NativeWindowDesc d;
        d.className = L"ArcaneCrashReporter";
        d.title     = ToWide(m_view.title.empty() ? productForTitle : m_view.title);
        d.width = 900; d.height = 640;
        d.popup = false; d.topmost = false; d.appWindow = true;
        d.foreground = true;   // UE's CRC forces itself to front (CrashReportClientApp.cpp:425-427)
        d.backgroundRgb = 0xF0F0F0;   // the system button face: plain Win32 controls draw on it
        d.dialogNavigation = true;    // R83: Tab between controls, Enter/Esc close via the default button / IDCANCEL
        window.Open(d, this);
        (void)window.WaitUntilReady(5000);
    }

    void ReporterWindow::SetView(ReportView v)
    {
        { std::lock_guard<std::mutex> lk(m_mutex); m_view = std::move(v); m_symbolizing = false; }
        if (m_window) m_window->PostUser(kUserViewChanged);
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
            m_view.isHang = false;
            m_view.canRelaunch = !m_view.relaunchLine.empty();
            m_view.headline += headlineSuffix;
            m_view.title += headlineSuffix;
            title = m_view.title;
        }
        if (m_window) { m_window->SetTitle(ToWide(title)); m_window->PostUser(kUserViewChanged); }
    }

    std::string ReporterWindow::CurrentDetails()
    {
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
        const std::string body = symbolizing ? "Symbolizing...\n\n" + DetailsText(v, sel) : DetailsText(v, sel);
        SetWindowTextW(static_cast<HWND>(m_details), Crlf(body).c_str());
        // Buttons: [0] folder [1] copy [2] close [3] relaunch [4] keep waiting [5] terminate
        ShowWindow(static_cast<HWND>(m_buttons[4]), v.isHang ? SW_SHOW : SW_HIDE);
        ShowWindow(static_cast<HWND>(m_buttons[5]), v.isHang ? SW_SHOW : SW_HIDE);
        ShowWindow(static_cast<HWND>(m_buttons[3]), v.relaunchLine.empty() ? SW_HIDE : SW_SHOW);
        EnableWindow(static_cast<HWND>(m_buttons[3]), v.canRelaunch ? TRUE : FALSE);
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
        const std::string body = symbolizing ? "Symbolizing...\n\n" + DetailsText(v, sel) : DetailsText(v, sel);
        SetWindowTextW(static_cast<HWND>(m_details), Crlf(body).c_str());
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
        const int dpi = static_cast<int>(GetDpiForWindow(static_cast<HWND>(m_hwnd)));
        auto px = [&](int v) { return MulDiv(v, dpi, 96); };
        const int m = px(12), line = px(20), btnW = px(150), btnH = px(28);
        int y = m;
        MoveWindow(static_cast<HWND>(m_header), m, y, w - 2 * m, line, TRUE); y += line + px(2);
        MoveWindow(static_cast<HWND>(m_when),   m, y, w - 2 * m, line, TRUE); y += line + px(2);
        MoveWindow(static_cast<HWND>(m_reason), m, y, w - 2 * m, line, TRUE); y += line + px(6);
        MoveWindow(static_cast<HWND>(m_combo),  m, y, px(320), px(200), TRUE);
        if (IsWindowVisible(static_cast<HWND>(m_combo))) y += px(26) + px(4);
        const int detailsBottom = h - m - btnH - px(8);
        MoveWindow(static_cast<HWND>(m_details), m, y, w - 2 * m, detailsBottom - y, TRUE);
        int x = m;
        for (int i = 0; i < 6; ++i)
        {
            HWND b = static_cast<HWND>(m_buttons[i]);
            if (!IsWindowVisible(b)) continue;
            MoveWindow(b, x, h - m - btnH, btnW, btnH, TRUE);
            x += btnW + px(8);
        }
    }

    void ReporterWindow::OnSize(int w, int h)
    {
        if (!m_hwnd) return;
        const unsigned dpi = GetDpiForWindow(static_cast<HWND>(m_hwnd));
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

    bool ReporterWindow::OnUser(unsigned msg, std::uintptr_t, std::intptr_t)
    {
        if (msg == kUserViewChanged) { ApplyView(); return true; }   // full refresh, incl. ComboBox_ResetContent (R84)
        return false;   // kUserHostRecovered / kUserHostExited: task 8
    }

    void ReporterWindow::OnDestroy()
    {
        if (m_uiFont)   DeleteObject(static_cast<HFONT>(m_uiFont));
        if (m_monoFont) DeleteObject(static_cast<HFONT>(m_monoFont));
        m_uiFont = m_monoFont = nullptr;
        m_fontDpi = 0;
        m_hwnd = nullptr;
    }
}
