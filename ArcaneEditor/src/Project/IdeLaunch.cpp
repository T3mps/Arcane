#include "Project/IdeLaunch.hpp"

#include "Project/RuntimeLaunch.hpp"   // QuoteArg (the one Win32 argv escaper)

#include <Arcane/Base/Log.hpp>
#include <Arcane/Build/Toolchain.hpp>  // ResolveDevenv (the one vswhere probe, shared with arcbuild)

#include <cwctype>
#include <optional>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI          // wingdi.h's GetObject macro would rewrite IRunningObjectTable::GetObject
#endif
#include <windows.h>
#include <objbase.h>   // CoInitializeEx, GetRunningObjectTable, CreateBindCtx
#include <oaidl.h>     // IDispatch, VARIANT, DISPPARAMS
#include <oleauto.h>   // SysAllocStringLen, VariantInit/Clear
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")     // IID_IDispatch
#pragma comment(lib, "user32.lib")   // IsIconic/ShowWindow/SetForegroundWindow
#endif

namespace Arcane::Editor::IdeLaunch
{
    // ---- pure halves ---------------------------------------------------------

    bool IsVisualStudioMoniker(std::wstring_view rotDisplayName)
    {
        constexpr std::wstring_view kPrefix = L"!VisualStudio.DTE.";
        return rotDisplayName.size() > kPrefix.size() &&
               rotDisplayName.substr(0, kPrefix.size()) == kPrefix;
    }

    namespace
    {
        // lexically_normal folds "." / ".." and unifies separators to the
        // platform's preferred one; the rest is a case-fold plus one more
        // separator pass so a '/'-spelled path compares equal on Windows
        // regardless of which side spelled it which way.
        std::wstring NormalizeForCompare(const std::filesystem::path& p)
        {
            std::wstring s = p.lexically_normal().wstring();
            for (wchar_t& c : s)
            {
                if (c == L'\\')
                    c = L'/';
                c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
            }
            return s;
        }
    }

    bool SameSolutionPath(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        if (a.empty() || b.empty())
            return false;
        return NormalizeForCompare(a) == NormalizeForCompare(b);
    }

    std::vector<std::wstring> ComposeLaunchArgs(const std::filesystem::path& solution,
                                                const std::filesystem::path& file)
    {
        std::vector<std::wstring> args;
        args.push_back(solution.wstring());
        if (!file.empty())
            args.push_back(file.wstring());
        return args;
    }

    const char* Describe(Outcome outcome)
    {
        switch (outcome)
        {
            case Outcome::Activated:        return "Visual Studio already has this solution open -- brought its window forward";
            case Outcome::OpenedInInstance: return "opened in the running Visual Studio instance";
            case Outcome::Launched:         return "launched Visual Studio with the solution";
            case Outcome::Blocked:          return "Visual Studio is running but busy (starting up, or in a modal) -- try again in a moment";
            case Outcome::DetectionFailed:  return "could not reach the COM Running Object Table to look for Visual Studio -- nothing launched";
            case Outcome::NoDevenv:         return "no Visual Studio install found (vswhere found no devenv.exe)";
            case Outcome::NoSolution:       return "no solution file to open -- generation must have failed (see the lines above)";
            case Outcome::NoFile:           return "the source file is not on disk";
            case Outcome::LaunchFailed:     return "CreateProcess refused to start devenv.exe (see the error above)";
            case Outcome::ActivateFailed:   return "found the running instance but could not activate its main window";
            case Outcome::OpenFailed:       return "found the running instance but ItemOperations.OpenFile refused the file";
            case Outcome::Count:            break;
        }
        return "unknown outcome";
    }

    // ---- resolution ----------------------------------------------------------

    std::filesystem::path ResolveDevenv()
    {
        return Arcane::Toolchain::ResolveDevenv();
    }

#ifdef _WIN32
    // ---- COM half: late-bound DTE over the Running Object Table --------------

    namespace
    {
        // COM for the duration of one click. S_FALSE (already initialised on
        // this thread, same model) is fine; RPC_E_CHANGED_MODE (already
        // initialised with the OTHER model) is fine too -- the ROT and DTE
        // proxies work from either apartment, and in that case the init is
        // not ours to undo.
        struct CoScope
        {
            HRESULT hr;
            CoScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
            ~CoScope() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
            [[nodiscard]] bool Usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
        };

        // Owned COM reference; Release on scope exit. `Out()` hands the slot to
        // an out-parameter API; `Detach()` gives the reference away.
        template <class T>
        class Com
        {
        public:
            Com() = default;
            Com(const Com&) = delete;
            Com& operator=(const Com&) = delete;
            Com(Com&& o) noexcept : m_p(std::exchange(o.m_p, nullptr)) {}
            Com& operator=(Com&& o) noexcept
            {
                if (this != &o)
                {
                    if (m_p) m_p->Release();
                    m_p = std::exchange(o.m_p, nullptr);
                }
                return *this;
            }
            ~Com() { if (m_p) m_p->Release(); }
            T** Out() { return &m_p; }
            T*  Get() const { return m_p; }
            T*  operator->() const { return m_p; }
            explicit operator bool() const { return m_p != nullptr; }
            T*  Detach() { return std::exchange(m_p, nullptr); }
        private:
            T* m_p = nullptr;
        };

        struct Bstr
        {
            BSTR b = nullptr;
            explicit Bstr(const std::wstring& s)
                : b(::SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
            ~Bstr() { if (b) ::SysFreeString(b); }
            Bstr(const Bstr&) = delete;
            Bstr& operator=(const Bstr&) = delete;
        };

        // A VS that is busy (a modal up, a build running, still initialising)
        // rejects incoming automation calls with RPC_E_CALL_REJECTED /
        // RPC_E_SERVERCALL_RETRYLATER rather than queueing them -- UE's
        // accessor retries its DTE calls a handful of times with a short sleep
        // for exactly this (VisualStudioSourceCodeAccessor.cpp, the
        // "Call was rejected by callee" comment). Same here, on BOTH halves
        // of a late-bound call (the name lookup is a call too).
        bool IsBusy(HRESULT hr)
        {
            return hr == RPC_E_CALL_REJECTED || hr == RPC_E_SERVERCALL_RETRYLATER;
        }

        // The one late-binding primitive: `obj.<name>(args...)` or
        // `obj.<name>` (property get), by DISPID lookup. `argsReversed` is in
        // DISPPARAMS order -- LAST argument FIRST -- which is COM's rule, not
        // ours; the two call sites below spell it out.
        HRESULT InvokeNamed(IDispatch* obj, const wchar_t* name, WORD flags,
                            std::vector<VARIANT>& argsReversed, VARIANT* result)
        {
            HRESULT hr = E_FAIL;
            for (int attempt = 0; attempt < 10; ++attempt)
            {
                DISPID id = DISPID_UNKNOWN;
                LPOLESTR names[1] = { const_cast<LPOLESTR>(name) };
                hr = obj->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
                if (SUCCEEDED(hr))
                {
                    DISPPARAMS dp{};
                    dp.cArgs  = static_cast<UINT>(argsReversed.size());
                    dp.rgvarg = argsReversed.empty() ? nullptr : argsReversed.data();
                    hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp,
                                     result, nullptr, nullptr);
                }
                if (!IsBusy(hr))
                    return hr;
                ::Sleep(100);
            }
            return hr;
        }

        // `obj.<prop>` as an owned IDispatch (null when absent / not an object).
        Com<IDispatch> GetProperty(IDispatch* obj, const wchar_t* prop)
        {
            Com<IDispatch> out;
            std::vector<VARIANT> none;
            VARIANT v;
            ::VariantInit(&v);
            if (SUCCEEDED(InvokeNamed(obj, prop, DISPATCH_PROPERTYGET, none, &v)) &&
                v.vt == VT_DISPATCH && v.pdispVal)
            {
                *out.Out() = v.pdispVal;   // take the reference Invoke handed us
                return out;
            }
            ::VariantClear(&v);
            return out;
        }

        // `obj.<prop>` as a string; nullopt when the call failed or it is not
        // a BSTR (a failed call is the "blocked" signal the caller acts on,
        // so it must stay distinguishable from an EMPTY string).
        std::optional<std::wstring> GetString(IDispatch* obj, const wchar_t* prop)
        {
            std::vector<VARIANT> none;
            VARIANT v;
            ::VariantInit(&v);
            if (FAILED(InvokeNamed(obj, prop, DISPATCH_PROPERTYGET, none, &v)))
                return std::nullopt;
            std::optional<std::wstring> out;
            if (v.vt == VT_BSTR)
                out = v.bstrVal ? std::wstring(v.bstrVal, ::SysStringLen(v.bstrVal)) : std::wstring();
            ::VariantClear(&v);
            return out;
        }

        // The ROT walk: UE's AccessVisualStudioViaDTE, late-bound. Every
        // VisualStudio.DTE moniker is asked for Solution.FullName; the first
        // whose answer names `solution` wins (its DTE handed back, owned).
        // A VS that could not be queried leaves the answer Blocked unless a
        // later instance matches -- UE's exact precedence: never launch a
        // duplicate while one instance is unaccounted for.
        Access FindInstance(const std::filesystem::path& solution, Com<IDispatch>& outDte)
        {
            Com<IRunningObjectTable> rot;
            if (FAILED(::GetRunningObjectTable(0, rot.Out())) || !rot)
            {
                ARC_WARN("IdeLaunch: could not get the Running Object Table");
                return Access::Unknown;
            }
            Com<IEnumMoniker> monikers;
            if (FAILED(rot->EnumRunning(monikers.Out())) || !monikers)
            {
                ARC_WARN("IdeLaunch: could not enumerate the Running Object Table");
                return Access::Unknown;
            }
            monikers->Reset();

            Access result = Access::NotOpen;
            for (;;)
            {
                Com<IMoniker> moniker;
                if (monikers->Next(1, moniker.Out(), nullptr) != S_OK)
                    break;

                Com<IBindCtx> ctx;
                LPOLESTR name = nullptr;
                if (FAILED(::CreateBindCtx(0, ctx.Out())) ||
                    FAILED(moniker->GetDisplayName(ctx.Get(), nullptr, &name)))
                {
                    result = Access::Unknown;
                    continue;
                }
                const bool ours = IsVisualStudioMoniker(name);
                ::CoTaskMemFree(name);
                if (!ours)
                    continue;

                Com<IUnknown> unk;
                if (FAILED(rot->GetObject(moniker.Get(), unk.Out())) || !unk)
                {
                    ARC_WARN("IdeLaunch: could not get a Visual Studio COM object from the ROT");
                    result = Access::Unknown;
                    continue;
                }
                Com<IDispatch> dte;
                if (FAILED(unk->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(dte.Out()))) || !dte)
                {
                    ARC_WARN("IdeLaunch: a Visual Studio instance did not answer IDispatch");
                    result = Access::Blocked;
                    continue;
                }

                Com<IDispatch> sol = GetProperty(dte.Get(), L"Solution");
                const std::optional<std::wstring> fullName =
                    sol ? GetString(sol.Get(), L"FullName") : std::nullopt;
                if (!fullName)
                {
                    ARC_INFO("IdeLaunch: Visual Studio is open but could not be queried -- "
                             "it may still be initializing or blocked by a modal operation");
                    result = Access::Blocked;
                    continue;
                }
                if (SameSolutionPath(*fullName, solution))
                {
                    *outDte.Out() = dte.Detach();
                    return Access::Open;
                }
            }
            return result;
        }

        bool Activate(IDispatch* dte)
        {
            Com<IDispatch> window = GetProperty(dte, L"MainWindow");
            if (!window)
                return false;
            std::vector<VARIANT> none;
            if (FAILED(InvokeNamed(window.Get(), L"Activate", DISPATCH_METHOD, none, nullptr)))
                return false;
            // Best effort on top of Activate: Window.HWnd lets the OS-level
            // foreground switch happen too (the click that got us here makes
            // the editor the foreground process, which is what entitles it to
            // hand foreground over). Failure here is not a failure of the
            // request -- Activate already succeeded.
            std::vector<VARIANT> noneAgain;
            VARIANT v;
            ::VariantInit(&v);
            if (SUCCEEDED(InvokeNamed(window.Get(), L"HWnd", DISPATCH_PROPERTYGET, noneAgain, &v)))
            {
                HWND hwnd = nullptr;
                // EnvDTE.Window.HWnd is an Int32 even on 64-bit VS: a window
                // handle's significant bits fit, but it must be ZERO-extended
                // (a handle with its top bit set is not a negative number).
                if (v.vt == VT_I4)        hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(static_cast<unsigned>(v.lVal)));
                else if (v.vt == VT_INT)  hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(static_cast<unsigned>(v.intVal)));
                else if (v.vt == VT_I8)   hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(v.llVal));
                if (hwnd)
                {
                    if (::IsIconic(hwnd))
                        ::ShowWindow(hwnd, SW_RESTORE);
                    ::SetForegroundWindow(hwnd);
                }
            }
            ::VariantClear(&v);
            return true;
        }

        bool OpenFileIn(IDispatch* dte, const std::filesystem::path& file)
        {
            Com<IDispatch> ops = GetProperty(dte, L"ItemOperations");
            if (!ops)
                return false;
            // ItemOperations.OpenFile(FileName, ViewKind) -- ViewKind is
            // vsViewKindTextView, the code editor (UE passes the same one).
            // DISPPARAMS order is LAST argument FIRST: [0] = ViewKind,
            // [1] = FileName. The BSTRs are owned by the two Bstr locals, so
            // the VARIANTs are NOT VariantClear'd (that would double-free).
            Bstr fileName(file.wstring());
            Bstr viewKind(L"{7651A701-06E5-11D1-8EBD-00A0C90F26EA}");
            std::vector<VARIANT> args(2);
            ::VariantInit(&args[0]);
            args[0].vt      = VT_BSTR;
            args[0].bstrVal = viewKind.b;
            ::VariantInit(&args[1]);
            args[1].vt      = VT_BSTR;
            args[1].bstrVal = fileName.b;
            VARIANT result;
            ::VariantInit(&result);
            const HRESULT hr = InvokeNamed(ops.Get(), L"OpenFile", DISPATCH_METHOD, args, &result);
            ::VariantClear(&result);
            if (FAILED(hr))
                ARC_WARN("IdeLaunch: ItemOperations.OpenFile('{}') failed (hr=0x{:08x})",
                         file.string(), static_cast<unsigned>(hr));
            return SUCCEEDED(hr);
        }

        // A NEW devenv, detached, with the solution's directory as cwd (the
        // one place a relative path in the args could ever resolve from). A
        // GUI app: no console, no redirect -- so this is deliberately not
        // RuntimeLaunch::SpawnDetached, whose CREATE_NO_WINDOW + log-redirect
        // contract is written for a console-subsystem game host.
        bool Launch(const std::filesystem::path& devenv, const std::vector<std::wstring>& args,
                    const std::filesystem::path& workDir)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(devenv, ec))
            {
                ARC_ERROR("IdeLaunch: '{}' does not exist", devenv.string());
                return false;
            }
            std::wstring cmdLine = RuntimeLaunch::QuoteArg(devenv.wstring());
            for (const std::wstring& a : args)
            {
                cmdLine.push_back(L' ');
                cmdLine += RuntimeLaunch::QuoteArg(a);
            }
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            const std::wstring cwd = workDir.wstring();
            // lpCommandLine must be MUTABLE (CreateProcessW may rewrite it).
            const BOOL ok = ::CreateProcessW(devenv.c_str(), cmdLine.data(), nullptr, nullptr,
                                             FALSE, 0, nullptr,
                                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
            if (!ok)
            {
                ARC_ERROR("IdeLaunch: CreateProcessW failed for '{}' (error {})",
                          devenv.string(), ::GetLastError());
                return false;
            }
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return true;
        }

        // The shared body of both public entry points: `file` empty means
        // "the solution only".
        Outcome Open(const std::filesystem::path& devenv, const std::filesystem::path& solution,
                     const std::filesystem::path& file)
        {
            std::error_code ec;
            if (solution.empty() || !std::filesystem::is_regular_file(solution, ec))
                return Outcome::NoSolution;
            if (!file.empty() && !std::filesystem::is_regular_file(file, ec))
                return Outcome::NoFile;

            CoScope co;
            if (!co.Usable())
            {
                ARC_ERROR("IdeLaunch: CoInitializeEx failed (hr=0x{:08x})", static_cast<unsigned>(co.hr));
                return Outcome::DetectionFailed;
            }

            Com<IDispatch> dte;
            switch (FindInstance(solution, dte))
            {
                case Access::Open:
                    if (!Activate(dte.Get()))
                        return Outcome::ActivateFailed;
                    if (file.empty())
                        return Outcome::Activated;
                    return OpenFileIn(dte.Get(), file) ? Outcome::OpenedInInstance : Outcome::OpenFailed;

                case Access::NotOpen:
                    if (devenv.empty())
                        return Outcome::NoDevenv;
                    return Launch(devenv, ComposeLaunchArgs(solution, file), solution.parent_path())
                               ? Outcome::Launched : Outcome::LaunchFailed;

                case Access::Blocked:
                    return Outcome::Blocked;

                case Access::Unknown:
                    break;
            }
            return Outcome::DetectionFailed;
        }
    }

    Access Probe(const std::filesystem::path& solution)
    {
        CoScope co;
        if (!co.Usable())
            return Access::Unknown;
        Com<IDispatch> dte;
        return FindInstance(solution, dte);
    }

    Outcome OpenSolution(const std::filesystem::path& devenv, const std::filesystem::path& solution)
    {
        return Open(devenv, solution, {});
    }

    Outcome OpenFile(const std::filesystem::path& devenv, const std::filesystem::path& solution,
                     const std::filesystem::path& file)
    {
        return Open(devenv, solution, file);
    }

#else   // !_WIN32 -- Visual Studio is a Windows IDE; nothing to find or launch.

    Access Probe(const std::filesystem::path&) { return Access::Unknown; }

    Outcome OpenSolution(const std::filesystem::path&, const std::filesystem::path&)
    {
        ARC_ERROR("IdeLaunch: Visual Studio integration is Windows-only");
        return Outcome::DetectionFailed;
    }

    Outcome OpenFile(const std::filesystem::path&, const std::filesystem::path&,
                     const std::filesystem::path&)
    {
        ARC_ERROR("IdeLaunch: Visual Studio integration is Windows-only");
        return Outcome::DetectionFailed;
    }

#endif
}
