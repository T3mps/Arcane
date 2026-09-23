// UTF-16 <-> UTF-8 for the reporter's Win32 files (crash window plan 2).
// Inline and header-only: the entry point takes its argv from
// CommandLineToArgvW (wide), the whole rest of the program speaks UTF-8
// because that is what the envelope is, and every Win32 call in between needs
// the wide form back. Deliberately NOT in ReporterArgs.hpp -- that TU is pure
// and source-compiles into ArcaneTests, which must not pull windows.h.
//
// Task 7 adds the three window-button actions here (OpenFolder,
// CopyToClipboard, SpawnDetached) rather than in ReporterWindow.cpp: task 9's
// hang-protocol buttons need SpawnDetached again and this is the file every
// Win32-touching TU in this exe already includes -- one home, per R44's
// reasoning for FileText.hpp.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW (OpenFolder)

#include <cstring>
#include <string>
#include <string_view>

namespace Arcane::Reporter
{
    inline std::wstring ToWide(std::string_view s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n > 0 ? static_cast<std::size_t>(n) : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    inline std::string ToUtf8(std::wstring_view w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // Open Explorer on `folder` (Open Report Folder). Best-effort: an empty
    // folder returns 0 (never calls ShellExecuteW -- there is nothing to
    // pass it); otherwise returns ShellExecuteW's raw result so the caller
    // can log a failure (R91 minor 3, fix round 1): > 32 means success, <= 32
    // is a real SE_ERR_*/ERROR_* code. There is no console to report to here
    // (D3, WindowedApp) and a MessageBox from a process reporting someone
    // ELSE's crash is not this button's job -- OnButton logs instead.
    inline INT_PTR OpenFolder(const std::wstring& folder)
    {
        if (folder.empty()) return 0;
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    }

    // Copy Details: the whole clipboard dance, UTF-16 (CF_UNICODETEXT) since
    // that is what every paste target on Windows wants.
    //
    // Fix round 1 (R91 minor 3): the payload is built (GlobalAlloc + fill)
    // BEFORE the clipboard is touched at all -- OpenClipboard/EmptyClipboard
    // moved after it. The original order called EmptyClipboard() first, so a
    // subsequent GlobalAlloc failure left the user's clipboard wiped with
    // nothing copied in its place; building the payload first means a
    // failure here never touches the clipboard the user already had.
    //
    // `mem` is only ever freed on a failure path BEFORE SetClipboardData --
    // once that call succeeds the system owns the handle, and freeing it
    // here would be a double free the next paste crashes on.
    inline bool CopyToClipboard(HWND owner, std::string_view text)
    {
        const std::wstring wide  = ToWide(text);
        const SIZE_T       bytes = (wide.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!mem) return false;
        void* dst = GlobalLock(mem);
        if (!dst) { GlobalFree(mem); return false; }
        std::memcpy(dst, wide.c_str(), bytes);
        GlobalUnlock(mem);
        if (!OpenClipboard(owner)) { GlobalFree(mem); return false; }
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, mem)) { GlobalFree(mem); CloseClipboard(); return false; }
        CloseClipboard();
        return true;
    }

    // Relaunch: a fire-and-forget child that must NOT inherit this process's
    // handles (bInheritHandles = FALSE) -- this exe may hold the host's
    // duplicated process handle (R33) and a relaunched copy of the same host
    // inheriting it would confuse a later `--host-created` identity check
    // with a handle it never opened itself. CREATE_NO_WINDOW | DETACHED_PROCESS
    // mirrors Diagnostics.cpp's own SpawnReporter (only meaningful for a
    // console-subsystem target; ignored for a windowed one, which every host
    // here is).
    //
    // Fix round 1 (R90): `outChildPid`, when given, is filled with the
    // child's pid on success so the caller can AllowSetForegroundWindow it
    // (D14 applies to a relaunched host too). On the empty-line refusal,
    // SetLastError is set explicitly so a caller that logs GetLastError()
    // right after a `false` return gets a real code instead of whatever
    // Win32 call happened to run last on this thread.
    inline bool SpawnDetached(std::string_view commandLine, DWORD* outChildPid = nullptr)
    {
        if (commandLine.empty()) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        std::wstring wide = ToWide(commandLine);   // CreateProcessW may write into this buffer; must be mutable
        STARTUPINFOW        si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, /*bInheritHandles=*/FALSE,
                                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
        if (!ok) return false;   // GetLastError() is still CreateProcessW's -- nothing Win32 runs between here and the caller's check
        if (outChildPid) *outChildPid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);   // fire-and-forget: this exe outlives nothing it relaunches
        return true;
    }
}
