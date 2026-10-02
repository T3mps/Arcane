#include "Project/OsShell.hpp"

#include <optional>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW
#include <shobjidl.h>   // IFileOperation, IFileOperationProgressSink, SHCreateItemFromParsingName
#include "Project/WinCom.hpp"   // CoScope, Com<T> (shared with IdeLaunch)
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace Arcane::Editor::OsShell
{
    namespace
    {
        bool Exists(const std::filesystem::path& path)
        {
            std::error_code ec;
            return !path.empty() && std::filesystem::exists(path, ec);
        }

        enum class Verb { Open, OpenWith, Select };

        // The ONE place this editor calls the Windows shell.
        ShellResult Run(Verb verb, const std::filesystem::path& path)
        {
#ifdef _WIN32
            const std::wstring file = path.wstring();
            HINSTANCE h = nullptr;
            switch (verb)
            {
            case Verb::Open:
                h = ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case Verb::OpenWith:
                h = ShellExecuteW(nullptr, L"openas", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case Verb::Select:
            {
                const std::wstring args = ExplorerSelectArgs(path);
                h = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                break;
            }
            }
            return ClassifyShellExecute(reinterpret_cast<std::intptr_t>(h));
#else
            (void)verb; (void)path;
            return ShellResult::Unsupported;
#endif
        }
    }

    std::wstring ExplorerSelectArgs(const std::filesystem::path& path)
    {
        std::filesystem::path native = path;
        native.make_preferred();
        return L"/select,\"" + native.wstring() + L"\"";
    }

    ShellResult ClassifyShellExecute(std::intptr_t code)
    {
        if (code > 32) return ShellResult::Ok;
        if (code == 2 || code == 3) return ShellResult::NotFound;   // SE_ERR_FNF / SE_ERR_PNF
        if (code == 31) return ShellResult::NoHandler;              // SE_ERR_NOASSOC
        return ShellResult::Failed;
    }

    std::string_view Describe(ShellResult r)
    {
        switch (r)
        {
        case ShellResult::Ok:          return "Done";
        case ShellResult::NotFound:    return "File not found on this machine";
        case ShellResult::NoHandler:   return "No program is associated with this file type";
        case ShellResult::Failed:      return "The shell refused the request";
        case ShellResult::Unsupported: return "Not supported on this platform";
        }
        return "Unknown shell result";
    }

    ShellResult ShellOpen(const std::filesystem::path& path)
    {
        if (!Exists(path)) return ShellResult::NotFound;
        return Run(Verb::Open, path);
    }

    ShellResult ShowInExplorer(const std::filesystem::path& path)
    {
        if (!Exists(path)) return ShellResult::NotFound;
        return Run(Verb::Select, path);
    }

    // Today's AssetPathAction sequence (EditorAppFrame.cpp:196-213): "open"
    // first; an extension with no association (.arcinput/.json on a stock
    // machine) has no open handler, so fall back to the Open With picker.
    ShellResult OpenAsText(const std::filesystem::path& path)
    {
        if (!Exists(path)) return ShellResult::NotFound;
        const ShellResult opened = Run(Verb::Open, path);
        if (opened == ShellResult::Ok || opened == ShellResult::Unsupported)
            return opened;
        return Run(Verb::OpenWith, path);
    }
}

// ---- T5 s7.4: ShellRecycle -------------------------------------------------

namespace Arcane::Editor::OsShell
{
    namespace
    {
        std::optional<RecycleResult> MissingForRecycle(std::span<const std::filesystem::path> files)
        {
            for (const auto& f : files)
            {
                std::error_code ec;
                if (!std::filesystem::exists(f, ec))
                    return RecycleResult{ false, { files.begin(), files.end() }, {},
                                          f.filename().string() + " was not found; nothing was recycled." };
            }
            return std::nullopt;
        }
    }
}

namespace Arcane::Editor::OsShell
{
#ifdef _WIN32
    namespace
    {
        // Stack-owned: Release never deletes. Records items deleted with no bin item.
        class RecycleSink final : public IFileOperationProgressSink
        {
        public:
            std::vector<std::filesystem::path> permanentlyDeleted;

            IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
            {
                if (!ppv) return E_POINTER;
                if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink))
                { *ppv = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK; }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
            IFACEMETHODIMP_(ULONG) Release() override { return --m_refs; }
            IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* item, HRESULT hrDelete, IShellItem* newlyCreated) override
            {
                if (SUCCEEDED(hrDelete) && !newlyCreated && item)
                {
                    PWSTR name = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
                    { permanentlyDeleted.emplace_back(name); ::CoTaskMemFree(name); }
                }
                return S_OK;
            }
            IFACEMETHODIMP StartOperations() override { return S_OK; }
            IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
            IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
            IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
            IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
            IFACEMETHODIMP ResetTimer() override { return S_OK; }
            IFACEMETHODIMP PauseTimer() override { return S_OK; }
            IFACEMETHODIMP ResumeTimer() override { return S_OK; }
        private:
            ULONG m_refs = 1;
        };
    }

    RecycleResult ShellRecycle(std::span<const std::filesystem::path> files, void* ownerHwnd)
    {
        if (auto missing = MissingForRecycle(files)) return *missing;
        RecycleResult r{ false, {}, {}, {} };
        const auto fail = [&](std::string why) { r.notRecycled.assign(files.begin(), files.end()); r.message = std::move(why); return r; };

        WinCom::CoScope co;
        if (!co.Usable()) return fail("Could not start the Windows shell (COM).");
        WinCom::Com<IFileOperation> op;
        if (FAILED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.Out()))))
            return fail("The Windows file-operation service is unavailable.");
        if (ownerHwnd) op->SetOwnerWindow(static_cast<HWND>(ownerHwnd));   // the nuke prompt never opens behind us
        constexpr DWORD kFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING;
        if (FAILED(op->SetOperationFlags(kFlags | FOFX_RECYCLEONDELETE)))   // Windows 8+
            op->SetOperationFlags(kFlags);                                    // FOF_ALLOWUNDO alone still recycles
        RecycleSink sink;
        DWORD cookie = 0;
        const bool advised = SUCCEEDED(op->Advise(&sink, &cookie));
        for (const auto& f : files)
        {
            WinCom::Com<IShellItem> item;
            const std::wstring native = std::filesystem::absolute(f).wstring();
            if (SUCCEEDED(::SHCreateItemFromParsingName(native.c_str(), nullptr, IID_PPV_ARGS(item.Out()))))
                op->DeleteItem(item.Get(), nullptr);
        }
        const HRESULT hr = op->PerformOperations();
        BOOL aborted = FALSE;
        op->GetAnyOperationsAborted(&aborted);
        if (advised) op->Unadvise(cookie);
        r.permanentlyDeleted = std::move(sink.permanentlyDeleted);
        for (const auto& f : files)
        {
            std::error_code ec;
            if (std::filesystem::exists(f, ec)) r.notRecycled.push_back(f);
        }
        r.ok = SUCCEEDED(hr) && !aborted && r.notRecycled.empty();
        if (!r.ok) r.message = aborted ? "The delete was cancelled." : "Some files could not be moved to the Recycle Bin.";
        return r;
    }

    bool RenameCaseOnly(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        return ::MoveFileExW(from.c_str(), to.c_str(), 0) != 0;
    }
#else
    RecycleResult ShellRecycle(std::span<const std::filesystem::path> files, void*)
    {
        if (auto missing = MissingForRecycle(files)) return *missing;
        return RecycleResult{ false, { files.begin(), files.end() }, {}, "Recycle Bin not supported on this platform" };
    }

    bool RenameCaseOnly(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        return !ec;
    }
#endif
}
